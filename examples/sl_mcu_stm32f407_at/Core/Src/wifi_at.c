/*
 * wifi_at.c - Wi-Fi station and TCP links through ESP-AT (no RTOS).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "main.h"
#include "wifi_at.h"
#include "at_client.h"
#include "at_spi.h"

#define READY_TIMEOUT_MS    5000
#define CMD_TIMEOUT_MS      2000
#define JOIN_TIMEOUT_MS     20000
#define RETRY_DELAY_MS      5000
#define LIVENESS_IDLE_MS    10000   // probe the ESP after this long without traffic
#define WAIT_IP_TIMEOUT_MS  60000   // give ESP-AT's own reconnect this long
#define RSSI_PERIOD_MS      30000

static wifi_at_state_t state;
static const char *sta_ssid;
static const char *sta_password;
static char hostname[16];
static char ip[16];
static int rssi;
static uint32_t state_tick;
static uint32_t last_probe_tick;
static uint32_t last_rssi_tick;
static uint8_t ready_seen;
static uint8_t got_ip;
static uint8_t server_up;

static uint16_t server_port;
static const wifi_at_link_handler_t *server_handler;

// Who owns each ESP-AT link (NULL: free)
static const wifi_at_link_handler_t *links[WIFI_AT_MAX_LINKS];
static uint8_t link_connected[WIFI_AT_MAX_LINKS];
static int connecting_link = -1;    // link of the AT+CIPSTART in progress
static at_done_cb_t connect_done_cb;

static void set_state(wifi_at_state_t s) {
    state = s;
    state_tick = HAL_GetTick();
}

// Same naming as the ESP-Hosted example: "stm32-" plus 6 hex digits folded
// from the 96 bit unique ID.
static void make_hostname(void) {
    uint32_t id = HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2();
    id ^= id >> 24;
    snprintf(hostname, sizeof(hostname), "stm32-%06lx", id & 0xffffff);
}

static void release_link(int link) {
    const wifi_at_link_handler_t *h = links[link];
    uint8_t was_connected = link_connected[link];
    links[link] = NULL;
    link_connected[link] = 0;
    if (h && was_connected && h->on_closed) {
        h->on_closed(link);
    }
}

// The ESP dropped every connection (restart, reset, Wi-Fi lost)
static void release_all_links(void) {
    for (int i = 0; i < WIFI_AT_MAX_LINKS; ++i) {
        release_link(i);
    }
    if (connecting_link >= 0) {
        // The AT+CIPSTART in progress won't finish now
        at_done_cb_t cb = connect_done_cb;
        connecting_link = -1;
        connect_done_cb = NULL;
        if (cb) {
            cb(AT_ERROR);
        }
    }
}

static void restart_esp(const char *why) {
    printf("wifi_at: %s - resetting the ESP\n", why);
    at_spi_reset_esp();
    at_client_reset();
    ready_seen = 0;
    got_ip = 0;
    server_up = 0;
    release_all_links();
    ip[0] = 0;
    rssi = 0;
    set_state(WIFI_AT_WAIT_READY);
}

// "<link>,CONNECT" / "<link>,CLOSED" / "<link>,CONNECT FAIL"
static int link_urc(const char *line) {
    if (!(line[0] >= '0' && line[0] < '0' + WIFI_AT_MAX_LINKS && line[1] == ',')) {
        return 0;
    }
    int link = line[0] - '0';
    const char *what = line + 2;

    if (strcmp(what, "CONNECT") == 0) {
        if (!links[link]) {
            links[link] = server_handler;   // incoming connection on our server
        }
        link_connected[link] = 1;
        if (links[link] && links[link]->on_connect) {
            links[link]->on_connect(link);
        }
    } else if (strncmp(what, "CLOSED", 6) == 0 || strncmp(what, "CONNECT FAIL", 12) == 0) {
        // (A client link can close before its AT+CIPSTART has finished,
        // e.g. a daytime server that sends one line and hangs up.)
        release_link(link);
    }
    return 1;
}

static void on_urc(const char *line) {
    if (link_urc(line)) {
        return;
    }

    if (strcmp(line, "ready") == 0) {
        ready_seen = 1;
        if (state != WIFI_AT_WAIT_READY) {
            // It restarted under us: start over
            printf("wifi_at: ESP restarted\n");
            at_client_reset();
            got_ip = 0;
            server_up = 0;
            release_all_links();
            ip[0] = 0;
            set_state(WIFI_AT_ECHO_OFF);
        }
    } else if (strcmp(line, "WIFI GOT IP") == 0) {
        got_ip = 1;
        if (state == WIFI_AT_WAIT_IP) {
            set_state(WIFI_AT_GET_IP);
        }
    } else if (strcmp(line, "WIFI DISCONNECT") == 0) {
        got_ip = 0;
        if (state == WIFI_AT_RUNNING) {
            printf("wifi_at: Wi-Fi lost, waiting for ESP-AT to reconnect\n");
            release_all_links();
            set_state(WIFI_AT_WAIT_IP);
        }
    }
    printf("wifi_at: < %s\n", line);
}

static void on_ipd(int link, const uint8_t *data, size_t len) {
    if (link >= 0 && link < WIFI_AT_MAX_LINKS && links[link] && links[link]->on_data) {
        links[link]->on_data(link, data, len);
    }
}

static void on_ip_line(const char *line) {
    // +CIPSTA:ip:"10.1.128.120"
    const char *p = strstr(line, "+CIPSTA:ip:\"");
    if (p) {
        p += 12;
        size_t n = 0;
        while (p[n] && p[n] != '"' && n < sizeof(ip) - 1) {
            ip[n] = p[n];
            n++;
        }
        ip[n] = 0;
    }
}

static void on_cwjap_line(const char *line) {
    // +CWJAP:"stm32","66:66:b3:7b:a1:4f",9,-49,0,1,3,0,1
    if (strncmp(line, "+CWJAP:", 7) != 0) {
        return;
    }
    const char *p = line + 7;
    int commas = 0;
    uint8_t quoted = 0;
    for (; *p && commas < 3; ++p) {
        if (*p == '"') {
            quoted = !quoted;
        } else if (*p == ',' && !quoted) {
            commas++;
        }
    }
    if (commas == 3) {
        rssi = (int) strtol(p, NULL, 10);
    }
}

static void on_step_done(at_result_t r) {
    switch (state) {
    case WIFI_AT_ECHO_OFF:
        set_state(r == AT_OK ? WIFI_AT_MODE : WIFI_AT_RETRY_WAIT);
        break;
    case WIFI_AT_MODE:
        set_state(r == AT_OK ? WIFI_AT_SLEEP_OFF : WIFI_AT_RETRY_WAIT);
        break;
    case WIFI_AT_SLEEP_OFF:
        printf("wifi_at: modem sleep off -> %s\n", at_result_name(r));
        set_state(WIFI_AT_HOSTNAME);    // not fatal
        break;
    case WIFI_AT_HOSTNAME:
        printf("wifi_at: hostname %s -> %s\n", hostname, at_result_name(r));
        set_state(WIFI_AT_JOIN);        // not fatal
        break;
    case WIFI_AT_JOIN:
        printf("wifi_at: join \"%s\" -> %s\n", sta_ssid, at_result_name(r));
        set_state(r == AT_OK ? WIFI_AT_GET_IP : WIFI_AT_RETRY_WAIT);
        break;
    case WIFI_AT_GET_IP:
        printf("wifi_at: ip %s\n", ip);
        // After a Wi-Fi reconnect the server is still there
        set_state(server_up ? WIFI_AT_RUNNING : WIFI_AT_MUX);
        break;
    case WIFI_AT_MUX:
        set_state(r == AT_OK ? WIFI_AT_SERVER_MAX_CONN_SET : WIFI_AT_RETRY_WAIT);
        break;
    case WIFI_AT_SERVER_MAX_CONN_SET:
        set_state(WIFI_AT_SERVER);      // not fatal
        break;
    case WIFI_AT_SERVER:
        // ERROR here usually means the server is already running
        printf("wifi_at: TCP server on port %u -> %s\n", server_port, at_result_name(r));
        server_up = 1;
        last_rssi_tick = HAL_GetTick() - RSSI_PERIOD_MS;
        set_state(WIFI_AT_RUNNING);
        break;
    default:
        break;
    }

    if (r == AT_TIMEOUT) {
        restart_esp("command timed out");
    }
}

static void on_probe_done(at_result_t r) {
    if (r != AT_OK) {
        restart_esp("ESP not responding");
    }
}

static void on_rssi_done(at_result_t r) {
    if (r == AT_TIMEOUT) {
        restart_esp("ESP not responding");
    }
}

static void on_connect_done(at_result_t r) {
    int link = connecting_link;
    at_done_cb_t cb = connect_done_cb;
    connecting_link = -1;
    connect_done_cb = NULL;
    if (link >= 0 && r != AT_OK && !link_connected[link]) {
        // Never connected: give the link back without a close event
        links[link] = NULL;
    }
    if (cb) {
        cb(r);
    }
}

int wifi_at_connect(const char *host, uint16_t port, const wifi_at_link_handler_t *handler,
        at_done_cb_t done_cb, uint32_t timeout_ms) {
    if (state != WIFI_AT_RUNNING || at_client_busy() || connecting_link >= 0) {
        return -1;
    }
    // The server hands out the low link IDs first; take a free one from the top.
    int link = -1;
    for (int i = WIFI_AT_MAX_LINKS - 1; i >= 0; --i) {
        if (!links[i]) {
            link = i;
            break;
        }
    }
    if (link < 0) {
        return -1;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=%d,\"TCP\",\"%s\",%u", link, host, port);
    links[link] = handler;
    link_connected[link] = 0;
    connecting_link = link;
    connect_done_cb = done_cb;
    if (at_client_command(cmd, timeout_ms, NULL, on_connect_done) != 0) {
        links[link] = NULL;
        connecting_link = -1;
        connect_done_cb = NULL;
        return -1;
    }
    return link;
}

int wifi_at_close(int link, at_done_cb_t done_cb) {
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+CIPCLOSE=%d", link);
    return at_client_command(cmd, CMD_TIMEOUT_MS, NULL, done_cb) == 0 ? 0 : -1;
}

void wifi_at_init(const char *ssid, const char *password, uint16_t port,
        const wifi_at_link_handler_t *handler) {
    sta_ssid = ssid;
    sta_password = password;
    server_port = port;
    server_handler = handler;
    make_hostname();
    printf("wifi_at: hostname %s\n", hostname);
    at_client_init(on_urc, on_ipd);
    ready_seen = 0;
    got_ip = 0;
    server_up = 0;
    set_state(WIFI_AT_WAIT_READY);
}

void wifi_at_poll(void) {
    char cmd[128];
    uint32_t now = HAL_GetTick();

    if (at_client_busy()) {
        return;
    }

    // Liveness: a quiet link might be a dead ESP. Ask it something cheap.
    if (state != WIFI_AT_WAIT_READY && state != WIFI_AT_RETRY_WAIT
            && now - at_client_last_rx() > LIVENESS_IDLE_MS && now - last_probe_tick > LIVENESS_IDLE_MS) {
        last_probe_tick = now;
        at_client_command("AT", CMD_TIMEOUT_MS, NULL, on_probe_done);
        return;
    }

    switch (state) {
    case WIFI_AT_WAIT_READY:
        if (ready_seen) {
            set_state(WIFI_AT_ECHO_OFF);
        } else if (now - state_tick > READY_TIMEOUT_MS) {
            restart_esp("no \"ready\" from the ESP");
        }
        break;

    case WIFI_AT_ECHO_OFF:
        at_client_command("ATE0", CMD_TIMEOUT_MS, NULL, on_step_done);
        break;

    case WIFI_AT_MODE:
        at_client_command("AT+CWMODE=1", CMD_TIMEOUT_MS, NULL, on_step_done);
        break;

    case WIFI_AT_SLEEP_OFF:
        // Modem sleep off: radio always on, low ping latency, more current
        at_client_command("AT+SLEEP=0", CMD_TIMEOUT_MS, NULL, on_step_done);
        break;

    case WIFI_AT_HOSTNAME:
        snprintf(cmd, sizeof(cmd), "AT+CWHOSTNAME=\"%s\"", hostname);
        at_client_command(cmd, CMD_TIMEOUT_MS, NULL, on_step_done);
        break;

    case WIFI_AT_JOIN:
        printf("wifi_at: joining \"%s\"\n", sta_ssid);
        snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", sta_ssid, sta_password);
        at_client_command(cmd, JOIN_TIMEOUT_MS, NULL, on_step_done);
        break;

    case WIFI_AT_GET_IP:
        at_client_command("AT+CIPSTA?", CMD_TIMEOUT_MS, on_ip_line, on_step_done);
        break;

    case WIFI_AT_MUX:
        at_client_command("AT+CIPMUX=1", CMD_TIMEOUT_MS, NULL, on_step_done);
        break;

    case WIFI_AT_SERVER_MAX_CONN_SET:
        snprintf(cmd, sizeof(cmd), "AT+CIPSERVERMAXCONN=%d", WIFI_AT_SERVER_MAX_CONN);
        at_client_command(cmd, CMD_TIMEOUT_MS, NULL, on_step_done);
        break;

    case WIFI_AT_SERVER:
        snprintf(cmd, sizeof(cmd), "AT+CIPSERVER=1,%u", server_port);
        at_client_command(cmd, CMD_TIMEOUT_MS, NULL, on_step_done);
        break;

    case WIFI_AT_RUNNING:
        if (now - last_rssi_tick > RSSI_PERIOD_MS) {
            last_rssi_tick = now;
            at_client_command("AT+CWJAP?", CMD_TIMEOUT_MS, on_cwjap_line, on_rssi_done);
        }
        break;

    case WIFI_AT_WAIT_IP:
        if (got_ip) {
            set_state(WIFI_AT_GET_IP);
        } else if (now - state_tick > WAIT_IP_TIMEOUT_MS) {
            restart_esp("no reconnect");
        }
        break;

    case WIFI_AT_RETRY_WAIT:
        if (now - state_tick > RETRY_DELAY_MS) {
            set_state(WIFI_AT_ECHO_OFF);
        }
        break;

    default:
        break;
    }
}

int wifi_at_running(void) {
    return state == WIFI_AT_RUNNING;
}

wifi_at_state_t wifi_at_get_state(void) {
    return state;
}

const char* wifi_at_get_ip(void) {
    return ip;
}

const char* wifi_at_get_hostname(void) {
    return hostname;
}

const char* wifi_at_get_ssid(void) {
    return sta_ssid;
}

int wifi_at_get_rssi(void) {
    return rssi;
}
