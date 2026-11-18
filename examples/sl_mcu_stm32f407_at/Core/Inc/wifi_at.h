/*
 * wifi_at.h - Wi-Fi station and TCP links through ESP-AT (no RTOS).
 *
 * The TCP/IP stack runs on the ESP. wifi_at joins the network with AT
 * commands, runs one TCP server (ESP-AT allows only one) and opens client
 * connections. Each link is owned by a service, which gets its connect,
 * data and close events through a wifi_at_link_handler_t.
 */

#ifndef WIFI_AT_H_
#define WIFI_AT_H_

#include <stdint.h>
#include <stddef.h>
#include "at_client.h"

#define WIFI_AT_MAX_LINKS       5   // ESP-AT link IDs 0..4
#define WIFI_AT_SERVER_MAX_CONN 4   // leaves one link free for client connections

typedef enum {
    WIFI_AT_WAIT_READY = 0,
    WIFI_AT_ECHO_OFF,
    WIFI_AT_MODE,
    WIFI_AT_SLEEP_OFF,
    WIFI_AT_HOSTNAME,
    WIFI_AT_JOIN,
    WIFI_AT_GET_IP,
    WIFI_AT_MUX,
    WIFI_AT_SERVER_MAX_CONN_SET,
    WIFI_AT_SERVER,
    WIFI_AT_RUNNING,
    WIFI_AT_WAIT_IP,    // lost Wi-Fi, ESP-AT reconnects by itself
    WIFI_AT_RETRY_WAIT
} wifi_at_state_t;

typedef struct {
    void (*on_connect)(int link);
    void (*on_data)(int link, const uint8_t *data, size_t len);
    void (*on_closed)(int link);
} wifi_at_link_handler_t;

// The server (port, handler) is started once the station has an IP.
void wifi_at_init(const char *ssid, const char *password, uint16_t server_port,
        const wifi_at_link_handler_t *server_handler);
void wifi_at_poll(void);

// Joined, have an IP and the server is up: services may use the links.
int wifi_at_running(void);

// Open a TCP connection. Returns the link ID, -1 if busy or no link is
// free. done_cb gets AT_OK once connected (the handler's on_connect runs
// before that); on failure the link is released.
int wifi_at_connect(const char *host, uint16_t port, const wifi_at_link_handler_t *handler,
        at_done_cb_t done_cb, uint32_t timeout_ms);

// Close a link (AT+CIPCLOSE). Returns 0 if the command was sent.
int wifi_at_close(int link, at_done_cb_t done_cb);

wifi_at_state_t wifi_at_get_state(void);
const char* wifi_at_get_ip(void);
const char* wifi_at_get_hostname(void);
const char* wifi_at_get_ssid(void);
int wifi_at_get_rssi(void);     // dBm, 0 if unknown

#endif /* WIFI_AT_H_ */
