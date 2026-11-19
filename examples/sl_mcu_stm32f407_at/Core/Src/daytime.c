/*
 * daytime.c - UTC time from NIST's daytime service over ESP-AT (no RTOS).
 *
 * Every DAYTIME_PERIOD_MS: wifi_at_connect() opens a TCP link to
 * time.nist.gov:13 (ESP-AT resolves the name), the server sends its one
 * line and closes, and the CLOSED event triggers parsing. daytime_poll()
 * starts each query and handles the receive timeout.
 */

#include <stdio.h>
#include <string.h>
#include "main.h"
#include "daytime.h"
#include "wifi_at.h"

#define CONNECT_TIMEOUT_MS  15000
#define RECEIVE_TIMEOUT_MS  10000

typedef enum {
    DT_IDLE = 0,
    DT_CONNECTING,
    DT_RECEIVING,
    DT_CLOSING
} dt_state_t;

static dt_state_t state;
static int link = -1;
static uint32_t state_tick;
static uint32_t next_tick;      // when to query next
static uint8_t synced;
static uint32_t sync_unix;      // NIST time at sync_tick
static uint32_t sync_tick;
static char reply[96];
static size_t reply_len;
static daytime_stats_t stats;

static void on_connect(int l);
static void on_data(int l, const uint8_t *data, size_t len);
static void on_closed(int l);

static const wifi_at_link_handler_t handler = {
    .on_connect = on_connect,
    .on_data = on_data,
    .on_closed = on_closed,
};

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

// A query failed: log it and try again after DAYTIME_RETRY_MS.
static void failed(const char *why) {
    printf("daytime: %s\n", why);
    stats.failures++;
    next_tick = HAL_GetTick() + DAYTIME_RETRY_MS;
    link = -1;
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
    link = -1;
    set_state(DT_IDLE);
}

// wifi_at: connected.
static void on_connect(int l) {
    (void) l;
    reply_len = 0;
}

// wifi_at: data from the server; collect it.
static void on_data(int l, const uint8_t *data, size_t len) {
    (void) l;
    for (size_t i = 0; i < len && reply_len < sizeof(reply) - 1; ++i) {
        reply[reply_len++] = (char) data[i];
    }
}

// wifi_at: the link closed.
static void on_closed(int l) {
    (void) l;
    // The server hangs up after the line: that's the normal end
    if (state == DT_CONNECTING || state == DT_RECEIVING || state == DT_CLOSING) {
        finish_reply();
    }
}

// AT+CIPSTART finished.
static void on_connect_done(at_result_t r) {
    if (state != DT_CONNECTING) {
        return;     // already closed and handled
    }
    if (r == AT_OK) {
        set_state(DT_RECEIVING);
    } else {
        char why[48];
        snprintf(why, sizeof(why), "connect to %s -> %s", DAYTIME_HOST, at_result_name(r));
        failed(why);
    }
}

// AT+CIPCLOSE (after a receive timeout) finished.
static void on_close_done(at_result_t r) {
    (void) r;
    // The CLOSED event finishes it; if that got lost, don't hang
    if (state == DT_CLOSING) {
        failed("no reply");
    }
}

// Main loop: start a query when one is due and Wi-Fi is running; time out
// a reply that doesn't come.
void daytime_poll(void) {
    uint32_t now = HAL_GetTick();

    switch (state) {
    case DT_IDLE:
        if (!wifi_at_running() || (int32_t) (now - next_tick) < 0) {
            break;
        }
        reply_len = 0;
        set_state(DT_CONNECTING);
        link = wifi_at_connect(DAYTIME_HOST, DAYTIME_PORT, &handler, on_connect_done, CONNECT_TIMEOUT_MS);
        if (link < 0) {
            set_state(DT_IDLE);     // busy or no free link: try again on the next poll
        }
        break;

    case DT_RECEIVING:
        if (now - state_tick > RECEIVE_TIMEOUT_MS && wifi_at_close(link, on_close_done) == 0) {
            set_state(DT_CLOSING);
        }
        break;

    case DT_CONNECTING:
    case DT_CLOSING:
    default:
        break;
    }
}
