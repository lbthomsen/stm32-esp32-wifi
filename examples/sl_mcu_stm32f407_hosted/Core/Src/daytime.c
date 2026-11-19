/*
 * daytime.c - UTC time from NIST's daytime service over lwIP (no RTOS).
 *
 * Every DAYTIME_PERIOD_MS: look up time.nist.gov (lwIP DNS, servers from
 * DHCP), connect to port 13, collect the one line the server sends and
 * parse it when the server closes the connection. All callbacks run from
 * wifi_netif_poll() in the main loop; daytime_poll() starts each query and
 * enforces the timeouts.
 */

#include <stdio.h>
#include <string.h>
#include "main.h"
#include "daytime.h"
#include "wifi_netif.h"

#include "lwip/tcp.h"
#include "lwip/dns.h"

#define QUERY_TIMEOUT_MS    15000   // DNS + connect + reply

typedef enum {
    DT_IDLE = 0,
    DT_RESOLVING,       // waiting for DNS
    DT_CONNECTED        // connecting / receiving
} dt_state_t;

static dt_state_t state;
static uint32_t state_tick;
static uint32_t next_tick;      // when to query next
static struct tcp_pcb *pcb;
static uint8_t synced;
static uint32_t sync_unix;      // NIST time at sync_tick
static uint32_t sync_tick;
static char reply[96];
static size_t reply_len;
static daytime_stats_t stats;

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// days_from_civil).
static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d) {
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t) (y - era * 400);
    uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t) doe - 719468;
}

// The inverse: date from days since 1970-01-01.
static void civil_from_days(int32_t z, int32_t *y, uint32_t *m, uint32_t *d) {
    z += 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t) (z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int32_t) yoe + era * 400 + (*m <= 2);
}

// Unix seconds as "YYYY-MM-DD HH:MM:SS" (UTC, no leap seconds, like Unix time).
void daytime_format(uint32_t unix_s, char *buf, size_t size) {
    int32_t y;
    uint32_t m, d;
    uint32_t secs = unix_s % 86400;
    civil_from_days((int32_t) (unix_s / 86400), &y, &m, &d);
    snprintf(buf, size, "%04ld-%02lu-%02lu %02lu:%02lu:%02lu", (long) y, (unsigned long) m,
            (unsigned long) d, (unsigned long) (secs / 3600), (unsigned long) (secs / 60 % 60),
            (unsigned long) (secs % 60));
}

// NIST time at the last sync plus the ticks since then. The HSE crystal
// keeps this within a second or so over the 10 minutes between syncs.
int daytime_now(uint32_t *unix_s) {
    if (!synced) {
        return 0;
    }
    *unix_s = sync_unix + (HAL_GetTick() - sync_tick) / 1000;
    return 1;
}

const daytime_stats_t* daytime_get_stats(void) {
    return &stats;
}

// Change state and note when (for the timeouts).
static void set_state(dt_state_t s) {
    state = s;
    state_tick = HAL_GetTick();
}

// Drop the connection (if any) without callbacks reaching us any more.
// Returns ERR_ABRT if it had to be aborted (a callback must pass that back).
static err_t drop_pcb(void) {
    err_t r = ERR_OK;
    if (pcb) {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        if (tcp_close(pcb) != ERR_OK) {
            tcp_abort(pcb);
            r = ERR_ABRT;
        }
        pcb = NULL;
    }
    return r;
}

// A query failed: log it and try again after DAYTIME_RETRY_MS.
static void failed(const char *why) {
    printf("daytime: %s\n", why);
    stats.failures++;
    next_tick = HAL_GetTick() + DAYTIME_RETRY_MS;
    set_state(DT_IDLE);
}

// "\n61362 26-11-18 08:34:08 00 0 0 310.3 UTC(NIST) *\n"
// Parse the collected reply into Unix seconds. Checks the "UTC(NIST)" label,
// the field ranges, and that the date agrees with the MJD in front of it.
static int parse_reply(uint32_t *unix_s) {
    long mjd;
    int yy, mo, dd, hh, mi, ss;
    reply[reply_len] = 0;
    if (!strstr(reply, "UTC(NIST)")) {
        return 0;
    }
    if (sscanf(reply, " %ld %d-%d-%d %d:%d:%d", &mjd, &yy, &mo, &dd, &hh, &mi, &ss) != 7) {
        return 0;
    }
    if (mo < 1 || mo > 12 || dd < 1 || dd > 31 || hh > 23 || mi > 59 || ss > 60) {
        return 0;
    }
    int32_t days = days_from_civil(2000 + yy, (uint32_t) mo, (uint32_t) dd);
    // MJD 40587 is 1970-01-01: a free cross-check of the date
    if (days != mjd - 40587) {
        return 0;
    }
    *unix_s = (uint32_t) days * 86400 + (uint32_t) (hh * 3600 + mi * 60 + ss);
    return 1;
}

// The server has closed: use the reply, schedule the next query.
static void finish_reply(void) {
    uint32_t t;
    if (!parse_reply(&t)) {
        failed("no valid reply");
        return;
    }

    uint32_t ours;
    int had_time = daytime_now(&ours);
    stats.last_drift_s = had_time ? (int32_t) (t - ours) : 0;
    sync_unix = t;
    sync_tick = HAL_GetTick();
    synced = 1;
    stats.syncs++;
    stats.last_sync_tick = sync_tick;

    char buf[24];
    daytime_format(t, buf, sizeof(buf));
    if (had_time) {
        printf("daytime: %s UTC from %s (our clock was off by %ld s)\n", buf, DAYTIME_HOST,
                (long) stats.last_drift_s);
    } else {
        printf("daytime: %s UTC from %s\n", buf, DAYTIME_HOST);
    }

    next_tick = HAL_GetTick() + DAYTIME_PERIOD_MS;
    set_state(DT_IDLE);
}

/* --- lwIP callbacks ----------------------------------------------------- */

// lwIP: data from the server (collect it), or p == NULL when it closes.
static err_t on_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void) arg;
    (void) err;
    if (!p) {
        // The server hangs up after the line: that's the normal end
        err_t r = drop_pcb();
        finish_reply();
        return r;
    }
    for (struct pbuf *q = p; q; q = q->next) {
        const char *data = q->payload;
        for (u16_t i = 0; i < q->len && reply_len < sizeof(reply) - 1; ++i) {
            reply[reply_len++] = data[i];
        }
    }
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

// lwIP: the connection failed (refused, reset, ...); the pcb is already freed.
static void on_err(void *arg, err_t err) {
    (void) arg;
    pcb = NULL;     // lwIP has freed it
    char why[40];
    snprintf(why, sizeof(why), "connection error %d", err);
    failed(why);
}

// lwIP: connected. Nothing to send; the server talks first.
static err_t on_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    (void) arg;
    (void) tpcb;
    (void) err;     // always ERR_OK in lwIP 2.x; failures go to on_err
    return ERR_OK;
}

// Open the TCP connection to the server's port 13.
static void connect_to(const ip_addr_t *addr) {
    pcb = tcp_new();
    if (!pcb) {
        failed("out of TCP pcbs");
        return;
    }
    reply_len = 0;
    tcp_recv(pcb, on_recv);
    tcp_err(pcb, on_err);
    if (tcp_connect(pcb, addr, DAYTIME_PORT, on_connected) != ERR_OK) {
        drop_pcb();
        failed("connect failed");
        return;
    }
    set_state(DT_CONNECTED);
}

// lwIP DNS: the lookup finished (addr NULL if it failed).
static void on_dns(const char *name, const ip_addr_t *addr, void *arg) {
    (void) name;
    (void) arg;
    if (state != DT_RESOLVING) {
        return;     // timed out meanwhile
    }
    if (!addr) {
        failed("DNS lookup failed");
        return;
    }
    connect_to(addr);
}

/* --- Main loop --------------------------------------------------------- */

// Main loop: start a query when one is due and the network is up; enforce
// the overall timeout of a query in progress.
void daytime_poll(void) {
    uint32_t now = HAL_GetTick();

    switch (state) {
    case DT_IDLE: {
        if (!wifi_netif_get_ip() || (int32_t) (now - next_tick) < 0) {
            break;
        }
        ip_addr_t addr;
        set_state(DT_RESOLVING);
        err_t r = dns_gethostbyname(DAYTIME_HOST, &addr, on_dns, NULL);
        if (r == ERR_OK) {
            connect_to(&addr);      // cached
        } else if (r != ERR_INPROGRESS) {
            failed("DNS lookup failed");
        }
        break;
    }

    case DT_RESOLVING:
    case DT_CONNECTED:
        if (now - state_tick > QUERY_TIMEOUT_MS) {
            drop_pcb();
            failed("no reply");
        }
        break;

    default:
        break;
    }
}
