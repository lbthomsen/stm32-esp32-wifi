/*
 * http_server.c - minimal HTTP/1.0 status page server on lwIP (no RTOS).
 *
 * One connection's life:
 *   accept     -> allocate a conn_t, register the callbacks
 *   recv       -> collect the request line, watch for the blank line that
 *                 ends the headers; then queue the whole reply with
 *                 tcp_write() (lwIP copies it) and wait for it to be acked
 *   sent       -> once every byte is acknowledged, close the connection
 *   poll       -> (every 2 s) give up on a client that stays silent
 *   err        -> lwIP has already freed the pcb; just free the conn_t
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "main.h"
#include "http_server.h"
#include "wifi_netif.h"
#include "wifi_sta.h"
#include "esp_spi.h"
#include "daytime.h"

#include "lwip/tcp.h"

#define MAX_CONNS           8
#define REQ_LINE_MAX        64
#define POLL_INTERVAL       4       // tcp_poll units of 500 ms: every 2 s
#define REQUEST_TIMEOUT_MS  5000    // a client that connects and says nothing

typedef struct {
    struct tcp_pcb *pcb;    // NULL: slot free
    uint32_t tick;          // when the connection came in
    char req_line[REQ_LINE_MAX];
    uint8_t req_len;
    uint8_t line_done;      // request line complete
    uint8_t eoh;            // progress through the blank line ending the headers
    uint8_t replied;        // reply queued, waiting for the acks
    uint16_t unacked;       // bytes of the reply not acknowledged yet
} conn_t;

static conn_t conns[MAX_CONNS];
static uint32_t requests;

static char body[1536];
static size_t body_len;
static char response[2048];

/* --- Page ------------------------------------------------------------ */

// Bounded printf-style append to body (truncates instead of overflowing).
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
    uint32_t ip = wifi_netif_get_ip();
    const daytime_stats_t *dt = daytime_get_stats();
    const esp_spi_stats_t *spi = esp_spi_get_stats();

    body_len = 0;
    add("<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width\">"
            "<meta http-equiv=\"refresh\" content=\"10\">"
            "<title>%s</title>"
            "<style>body{font-family:sans-serif;margin:1.5em}td{padding:3px 16px 3px 0}"
            "td:first-child{color:#666}</style></head><body>\n"
            "<h1>%s</h1>\n<table>\n", wifi_netif_get_hostname(), wifi_netif_get_hostname());

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
    add_row("Wi-Fi", "%s, %d dBm", wifi_sta_get_ssid(), wifi_sta_get_rssi());
    add_row("IP", "%lu.%lu.%lu.%lu", ip & 0xff, (ip >> 8) & 0xff, (ip >> 16) & 0xff, ip >> 24);
    add_row("Host", "STM32F407 (no RTOS, lwIP), ESP32-C3 running ESP-Hosted over SPI");
    add_row("SPI", "%lu transactions, %lu frames in, %lu frames out",
            spi->transactions, spi->rx_frames, spi->tx_frames);
    add_row("SPI errors", "checksum %lu, header %lu, spi %lu",
            spi->rx_bad_checksum, spi->rx_bad_header, spi->spi_errors);
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

/* --- Connection handling ----------------------------------------------- */

// Mark a slot free.
static void conn_free(conn_t *c) {
    memset(c, 0, sizeof(*c));
}

// Detach our callbacks and close (or abort if lwIP is out of memory).
// Returns ERR_ABRT if the pcb was aborted, which callers must pass back.
static err_t conn_close(conn_t *c) {
    struct tcp_pcb *pcb = c->pcb;
    err_t r = ERR_OK;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    if (tcp_close(pcb) != ERR_OK) {
        tcp_abort(pcb);
        r = ERR_ABRT;
    }
    conn_free(c);
    return r;
}

// Queue the reply. Returns what the recv callback must return.
static err_t send_reply(conn_t *c) {
    size_t len = build_response(c);
    requests++;
    if (tcp_write(c->pcb, response, (u16_t) len, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        printf("http: tcp_write failed\n");
        return conn_close(c);
    }
    c->unacked = (uint16_t) len;
    c->replied = 1;
    tcp_output(c->pcb);
    return ERR_OK;
}

// lwIP: `len` more bytes were acknowledged. Close once the reply is all acked.
static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    (void) pcb;
    conn_t *c = arg;
    c->unacked = c->unacked > len ? c->unacked - len : 0;
    if (c->replied && c->unacked == 0) {
        return conn_close(c);
    }
    return ERR_OK;
}

// lwIP: data arrived (p), or the client closed (p == NULL). Collects the
// request; anything after the headers (a body) is ignored.
static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    conn_t *c = arg;
    (void) err;

    if (!p) {
        // The client closed its side
        return conn_close(c);
    }

    tcp_recved(pcb, p->tot_len);

    if (!c->replied) {
        for (struct pbuf *q = p; q; q = q->next) {
            const char *data = q->payload;
            for (u16_t i = 0; i < q->len; ++i) {
                char ch = data[i];
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
                        pbuf_free(p);
                        return send_reply(c);
                    }
                } else if (ch != '\r') {
                    c->eoh = 0;
                }
            }
        }
    }
    pbuf_free(p);
    return ERR_OK;
}

// lwIP: periodic callback (POLL_INTERVAL). Drops clients that hang around.
static err_t on_poll(void *arg, struct tcp_pcb *pcb) {
    (void) pcb;
    conn_t *c = arg;
    if (HAL_GetTick() - c->tick > REQUEST_TIMEOUT_MS) {
        return conn_close(c);
    }
    return ERR_OK;
}

// lwIP: the connection is gone (reset by the peer, out of memory, ...).
static void on_err(void *arg, err_t err) {
    (void) err;
    // lwIP has freed the pcb already
    conn_t *c = arg;
    if (c) {
        conn_free(c);
    }
}

// lwIP: a new connection on port 80. Give it a slot and our callbacks.
static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void) arg;
    if (err != ERR_OK || !pcb) {
        return ERR_VAL;
    }
    conn_t *c = NULL;
    for (int i = 0; i < MAX_CONNS; ++i) {
        if (!conns[i].pcb) {
            c = &conns[i];
            break;
        }
    }
    if (!c) {
        tcp_abort(pcb);     // all slots busy: reset this one
        return ERR_ABRT;
    }
    memset(c, 0, sizeof(*c));
    c->pcb = pcb;
    c->tick = HAL_GetTick();
    tcp_arg(pcb, c);
    tcp_recv(pcb, on_recv);
    tcp_sent(pcb, on_sent);
    tcp_err(pcb, on_err);
    tcp_poll(pcb, on_poll, POLL_INTERVAL);
    return ERR_OK;
}

// Create the listening pcb on port 80.
void http_server_init(void) {
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb || tcp_bind(pcb, IP_ANY_TYPE, HTTP_SERVER_PORT) != ERR_OK) {
        printf("http: can't bind port %d\n", HTTP_SERVER_PORT);
        return;
    }
    pcb = tcp_listen(pcb);
    tcp_accept(pcb, on_accept);
    printf("http: listening on port %d\n", HTTP_SERVER_PORT);
}

// Requests answered since start-up.
uint32_t http_server_requests(void) {
    return requests;
}
