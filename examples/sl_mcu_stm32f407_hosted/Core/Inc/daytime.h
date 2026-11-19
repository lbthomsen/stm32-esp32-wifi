/*
 * daytime.h - UTC time from NIST's daytime service (RFC 867, TCP port 13).
 *
 * Every DAYTIME_PERIOD_MS the client connects to time.nist.gov:13, which
 * sends one line and hangs up:
 *
 *   "\n61362 26-11-18 08:34:08 00 0 0 310.3 UTC(NIST) *\n"
 *
 * (MJD, YY-MM-DD, HH:MM:SS, DST flag, leap second flag, health, ms advance,
 * label, on-time marker). Between syncs the time runs on HAL_GetTick().
 * NIST asks clients not to query more often than once every 4 seconds.
 */

#ifndef DAYTIME_H_
#define DAYTIME_H_

#include <stdint.h>
#include <stddef.h>

#define DAYTIME_HOST            "time.nist.gov"
#define DAYTIME_PORT            13
#define DAYTIME_PERIOD_MS       (10 * 60 * 1000)
#define DAYTIME_RETRY_MS        (60 * 1000)

typedef struct {
    uint32_t syncs;
    uint32_t failures;
    uint32_t last_sync_tick;    // HAL tick of the last good sync
    int32_t last_drift_s;       // NIST minus our clock at the last sync (0 on the first)
} daytime_stats_t;

// Main loop: starts a query when one is due, handles timeouts.
void daytime_poll(void);

// Current UTC time as Unix seconds. Returns 0 if not synced yet.
int daytime_now(uint32_t *unix_s);

// "YYYY-MM-DD HH:MM:SS" (needs 20 bytes)
void daytime_format(uint32_t unix_s, char *buf, size_t size);

// Sync counters for the status page.
const daytime_stats_t* daytime_get_stats(void);

#endif /* DAYTIME_H_ */
