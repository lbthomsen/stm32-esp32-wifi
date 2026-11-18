/*
 * wifi_at.c - Wi-Fi station and TCP echo server through ESP-AT (no RTOS).
 */

#include <stdio.h>
#include <string.h>
#include "main.h"
#include "wifi_at.h"
#include "at_client.h"
#include "at_spi.h"

#define READY_TIMEOUT_MS    5000
#define CMD_TIMEOUT_MS      2000
#define JOIN_TIMEOUT_MS     20000
#define SEND_TIMEOUT_MS     5000
#define RETRY_DELAY_MS      5000
#define LIVENESS_IDLE_MS    10000   // probe the ESP after this long without traffic
#define WAIT_IP_TIMEOUT_MS  60000   // give ESP-AT's own reconnect this long

static wifi_at_state_t state;
static const char *sta_ssid;
static const char *sta_password;
static char hostname[16];
static char ip[16];
static uint32_t state_tick;
static uint32_t last_probe_tick;
static uint8_t ready_seen;
static uint8_t got_ip;

// Echo FIFO: bytes received on echo_link, sent back with CIPSEND in pieces
// of up to AT_SEND_MAX. One echo client at a time.
#define ECHO_FIFO_SIZE 8192
static uint8_t echo_fifo[ECHO_FIFO_SIZE];
static size_t echo_head, echo_tail;
static int echo_link;
static size_t echo_in_flight;   // bytes handed to the CIPSEND in progress
static uint32_t echo_dropped;
static uint8_t echo_chunk[AT_SEND_MAX];

static size_t echo_used(void) {
    return (echo_head + ECHO_FIFO_SIZE - echo_tail) % ECHO_FIFO_SIZE;
}

static void echo_clear(void) {
    echo_head = echo_tail = 0;
    echo_in_flight = 0;
}

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

static void restart_esp(const char *why) {
    printf("wifi_at: %s - resetting the ESP\n", why);
    at_spi_reset_esp();
    at_client_reset();
    ready_seen = 0;
    got_ip = 0;
    echo_clear();
    ip[0] = 0;
    set_state(WIFI_AT_WAIT_READY);
}

static void on_urc(const char *line) {
    if (strcmp(line, "ready") == 0) {
        ready_seen = 1;
        if (state != WIFI_AT_WAIT_READY) {
            // It restarted under us: start over
            printf("wifi_at: ESP restarted\n");
            at_client_reset();
            got_ip = 0;
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
            set_state(WIFI_AT_WAIT_IP);
        }
    }
    printf("wifi_at: < %s\n", line);
}

static void on_ipd(int link, const uint8_t *data, size_t len) {
    if (link != echo_link && echo_used() == 0 && echo_in_flight == 0) {
        echo_link = link;
    }
    if (link != echo_link) {
        echo_dropped += len;
        return;
    }
    for (size_t i = 0; i < len; ++i) {
        size_t next = (echo_head + 1) % ECHO_FIFO_SIZE;
        if (next == echo_tail) {
            echo_dropped += len - i;
            break;
        }
        echo_fifo[echo_head] = data[i];
        echo_head = next;
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
        set_state(WIFI_AT_MUX);
        break;
    case WIFI_AT_MUX:
        set_state(r == AT_OK ? WIFI_AT_SERVER : WIFI_AT_RETRY_WAIT);
        break;
    case WIFI_AT_SERVER:
        // ERROR here usually means the server is already running (after a reconnect)
        printf("wifi_at: TCP echo server on port %d -> %s\n", WIFI_AT_ECHO_PORT, at_result_name(r));
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

static void on_echo_done(at_result_t r) {
    if (r == AT_OK) {
        echo_tail = (echo_tail + echo_in_flight) % ECHO_FIFO_SIZE;
    } else {
        // The link is gone (closed by the peer) or the send failed: drop it
        printf("wifi_at: echo send -> %s\n", at_result_name(r));
        echo_clear();
    }
    echo_in_flight = 0;
    if (r == AT_TIMEOUT) {
        restart_esp("send timed out");
    }
}

void wifi_at_init(const char *ssid, const char *password) {
    sta_ssid = ssid;
    sta_password = password;
    make_hostname();
    printf("wifi_at: hostname %s\n", hostname);
    at_client_init(on_urc, on_ipd);
    ready_seen = 0;
    got_ip = 0;
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

    case WIFI_AT_SERVER:
        snprintf(cmd, sizeof(cmd), "AT+CIPSERVER=1,%d", WIFI_AT_ECHO_PORT);
        at_client_command(cmd, CMD_TIMEOUT_MS, NULL, on_step_done);
        break;

    case WIFI_AT_RUNNING: {
        size_t n = echo_used();
        if (n) {
            if (n > sizeof(echo_chunk)) {
                n = sizeof(echo_chunk);
            }
            for (size_t i = 0; i < n; ++i) {
                echo_chunk[i] = echo_fifo[(echo_tail + i) % ECHO_FIFO_SIZE];
            }
            if (at_client_send(echo_link, echo_chunk, n, SEND_TIMEOUT_MS, on_echo_done) == 0) {
                echo_in_flight = n;
            }
        }
        break;
    }

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

wifi_at_state_t wifi_at_get_state(void) {
    return state;
}

const char* wifi_at_get_ip(void) {
    return ip;
}

const char* wifi_at_get_hostname(void) {
    return hostname;
}
