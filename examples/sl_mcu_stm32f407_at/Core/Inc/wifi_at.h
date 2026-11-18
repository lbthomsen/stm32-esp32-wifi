/*
 * wifi_at.h - Wi-Fi station and TCP echo server through ESP-AT (no RTOS).
 *
 * The TCP/IP stack runs on the ESP. The STM32 joins the network with AT
 * commands, answers pings through the ESP, and runs a TCP echo server
 * (port WIFI_AT_ECHO_PORT) to show socket data in both directions.
 */

#ifndef WIFI_AT_H_
#define WIFI_AT_H_

#include <stdint.h>

#define WIFI_AT_ECHO_PORT 7

typedef enum {
    WIFI_AT_WAIT_READY = 0,
    WIFI_AT_ECHO_OFF,
    WIFI_AT_MODE,
    WIFI_AT_SLEEP_OFF,
    WIFI_AT_HOSTNAME,
    WIFI_AT_JOIN,
    WIFI_AT_GET_IP,
    WIFI_AT_MUX,
    WIFI_AT_SERVER,
    WIFI_AT_RUNNING,
    WIFI_AT_WAIT_IP,    // lost Wi-Fi, ESP-AT reconnects by itself
    WIFI_AT_RETRY_WAIT
} wifi_at_state_t;

void wifi_at_init(const char *ssid, const char *password);
void wifi_at_poll(void);
wifi_at_state_t wifi_at_get_state(void);
const char* wifi_at_get_ip(void);
const char* wifi_at_get_hostname(void);

#endif /* WIFI_AT_H_ */
