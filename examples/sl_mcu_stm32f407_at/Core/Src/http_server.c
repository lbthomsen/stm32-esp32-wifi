/*
 * http_server.c - minimal HTTP/1.0 status page server over ESP-AT (no RTOS).
 *
 * One connection's life (per ESP-AT link):
 *   on_connect   CONNECT event: start collecting the request
 *   on_data      +IPD data: request line and headers; READY at the blank line
 *   poll         READY: AT+CIPSEND the reply -> SENDING -> CLOSE:
 *                AT+CIPCLOSE -> CLOSING -> FREE
 *   on_closed    CLOSED event (client hung up, or after our close): FREE
 * Only one AT command runs at a time, so http_server_poll() handles one
 * connection step per call.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "main.h"
#include "http_server.h"
#include "at_client.h"
#include "at_spi.h"
#include "daytime.h"

#define REQUEST_TIMEOUT_MS  5000    // a client that connects and says nothing
#define SEND_TIMEOUT_MS     5000
#define REQ_LINE_MAX        64

typedef enum {
    CONN_FREE = 0,
    CONN_RECV,          // collecting the request
    CONN_READY,         // request complete, reply not sent yet
    CONN_SENDING,
    CONN_CLOSE,         // reply sent (or given up), close the link
    CONN_CLOSING
} conn_state_t;

typedef struct {
    conn_state_t state;
    uint32_t tick;
    char req_line[REQ_LINE_MAX];
    uint8_t req_len;
    uint8_t line_done;
    uint8_t eoh;        // progress through the blank line that ends the headers
} conn_t;

static conn_t conns[WIFI_AT_MAX_LINKS];
static int busy_link = -1;  // link of the send or close in progress
static uint32_t requests;

static char body[1536];
static char response[AT_SEND_MAX];

static void on_connect(int link);
static void on_data(int link, const uint8_t *data, size_t len);
static void on_closed(int link);

const wifi_at_link_handler_t http_server_handler = {
    .on_connect = on_connect,
    .on_data = on_data,
    .on_closed = on_closed,
};

// Change a connection's state and note when (for the request timeout).
static void set_conn(int link, conn_state_t s) {
    conns[link].state = s;
    conns[link].tick = HAL_GetTick();
}

// wifi_at: a client connected on this link.
static void on_connect(int link) {
    conn_t *c = &conns[link];
    memset(c, 0, sizeof(*c));
    set_conn(link, CONN_RECV);
}

// wifi_at: data on this link. Collects the request line and watches for
// the blank line that ends the headers.
static void on_data(int link, const uint8_t *data, size_t len) {
    conn_t *c = &conns[link];
    if (c->state != CONN_RECV) {
        return;     // anything after the request is ignored
    }
    for (size_t i = 0; i < len; ++i) {
        char ch = (char) data[i];
        if (!c->line_done) {
            if (ch == '\n') {
                c->line_done = 1;
            } else if (ch != '\r' && c->req_len < REQ_LINE_MAX - 1) {
                c->req_line[c->req_len++] = ch;
            }
        }
        // The headers end with an empty line: "\r\n\r\n" (or "\n\n")
        if (ch == '\n') {
            if (++c->eoh == 2) {
                set_conn(link, CONN_READY);
                return;
            }
        } else if (ch != '\r') {
            c->eoh = 0;
        }
    }
}

// wifi_at: the link closed.
static void on_closed(int link) {
    conns[link].state = CONN_FREE;
}

// Bounded printf-style append to body (truncates instead of overflowing).
static size_t body_len;
static void add(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(body + body_len, sizeof(body) - body_len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        body_len += (size_t) n;
        if (body_len >= sizeof(body)) {
            body_len = sizeof(body) - 1;
        }
    }
}

// One table row: name and a printf-formatted value.
static void add_row(const char *name, const char *fmt, ...) {
    char value[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(value, sizeof(value), fmt, ap);
    va_end(ap);
    add("<tr><td>%s</td><td>%s</td></tr>\n", name, value);
}

// The HTML status page, into body. Plain HTML with a 10 s auto-refresh.
static void build_status_page(void) {
    uint32_t up = HAL_GetTick() / 1000;
    uint32_t now;
    char when[24];
    const daytime_stats_t *dt = daytime_get_stats();
    const at_spi_stats_t *spi = at_spi_get_stats();

    body_len = 0;
    add("<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width\">"
            "<meta http-equiv=\"refresh\" content=\"10\">"
            "<title>%s</title>"
            "<style>body{font-family:sans-serif;margin:1.5em}td{padding:3px 16px 3px 0}"
            "td:first-child{color:#666}</style></head><body>\n"
            "<h1>%s</h1>\n<table>\n", wifi_at_get_hostname(), wifi_at_get_hostname());

    if (daytime_now(&now)) {
        daytime_format(now, when, sizeof(when));
        add_row("Time", "%s UTC", when);
        add_row("Last NIST sync", "%lu s ago (%lu syncs, %lu failures, last correction %ld s)",
                (HAL_GetTick() - dt->last_sync_tick) / 1000, dt->syncs, dt->failures,
                (long) dt->last_drift_s);
    } else {
        add_row("Time", "not synced yet (%lu failures)", dt->failures);
    }
    add_row("Uptime", "%lu d %02lu:%02lu:%02lu", up / 86400, up / 3600 % 24, up / 60 % 60, up % 60);
    add_row("Wi-Fi", "%s, %d dBm", wifi_at_get_ssid(), wifi_at_get_rssi());
    add_row("IP", "%s", wifi_at_get_ip());
    add_row("Host", "STM32F407 (no RTOS), ESP32-C3 running ESP-AT over SPI");
    add_row("SPI", "%lu transactions, %lu bytes in, %lu bytes out",
            spi->transactions, spi->rx_bytes, spi->tx_bytes);
    add_row("SPI errors", "seq %lu, len %lu, spi %lu, overflow %lu (stale status skipped %lu)",
            spi->seq_errors, spi->len_errors, spi->spi_errors, spi->rx_overflows, spi->stale_status);
    add_row("HTTP requests", "%lu", requests);
    add("</table>\n</body></html>\n");
}

// The reply to the request in c, in `response`. Returns its length.
static size_t build_response(conn_t *c) {
    char method[8] = "";
    char path[REQ_LINE_MAX] = "";
    sscanf(c->req_line, "%7s %63s", method, path);
    int head = strcmp(method, "HEAD") == 0;
    int ok = (strcmp(method, "GET") == 0 || head) && strcmp(path, "/") == 0;

    printf("http: %s\n", c->req_line);

    if (ok) {
        build_status_page();
    } else {
        body_len = (size_t) snprintf(body, sizeof(body), "404 Not Found\n");
    }

    int n = snprintf(response, sizeof(response),
            "HTTP/1.0 %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %u\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n"
            "\r\n",
            ok ? "200 OK" : "404 Not Found",
            ok ? "text/html; charset=utf-8" : "text/plain",
            (unsigned) body_len);
    size_t len = (size_t) n;
    if (!head) {
        size_t room = sizeof(response) - len;
        size_t copy = body_len < room ? body_len : room;
        memcpy(response + len, body, copy);
        len += copy;
    }
    return len;
}

// AT+CIPSEND finished: close the link next.
static void on_sent(at_result_t r) {
    int link = busy_link;
    busy_link = -1;
    if (link >= 0 && conns[link].state == CONN_SENDING) {
        if (r != AT_OK) {
            printf("http: send on link %d -> %s\n", link, at_result_name(r));
        }
        set_conn(link, CONN_CLOSE);
    }
}

// AT+CIPCLOSE finished.
static void on_close_done(at_result_t r) {
    (void) r;
    int link = busy_link;
    busy_link = -1;
    if (link >= 0 && conns[link].state == CONN_CLOSING) {
        conns[link].state = CONN_FREE;  // the CLOSED event may come too, or may not
    }
}

// Main loop: the next AT step of one connection (timeout, reply, close).
void http_server_poll(void) {
    uint32_t now = HAL_GetTick();

    if (!wifi_at_running() || at_client_busy()) {
        return;
    }

    for (int link = 0; link < WIFI_AT_MAX_LINKS; ++link) {
        conn_t *c = &conns[link];

        switch (c->state) {
        case CONN_RECV:
            if (now - c->tick > REQUEST_TIMEOUT_MS) {
                set_conn(link, CONN_CLOSE);
            }
            break;

        case CONN_READY: {
            size_t len = build_response(c);
            busy_link = link;
            if (at_client_send(link, (const uint8_t*) response, len, SEND_TIMEOUT_MS, on_sent) == 0) {
                requests++;
                set_conn(link, CONN_SENDING);
            } else {
                busy_link = -1;
                set_conn(link, CONN_CLOSE);
            }
            return;     // one AT command at a time
        }

        case CONN_CLOSE:
            busy_link = link;
            if (wifi_at_close(link, on_close_done) == 0) {
                set_conn(link, CONN_CLOSING);
            } else {
                busy_link = -1;
            }
            return;

        default:
            break;
        }
    }
}

// Requests answered since start-up.
uint32_t http_server_requests(void) {
    return requests;
}
