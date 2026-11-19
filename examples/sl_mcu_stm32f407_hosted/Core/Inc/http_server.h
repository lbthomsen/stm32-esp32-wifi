/*
 * http_server.h - minimal HTTP/1.0 status page server on lwIP (no RTOS).
 *
 * Uses lwIP's raw TCP API, so it is event driven: lwIP calls back on a new
 * connection, on received data, when sent data is acknowledged and on
 * errors. Those callbacks run from wifi_netif_poll() in the main loop.
 *
 *   GET /     status page (HTML)
 *   anything else: 404
 */

#ifndef HTTP_SERVER_H_
#define HTTP_SERVER_H_

#include <stdint.h>

#define HTTP_SERVER_PORT 80

// Start listening (call once, after wifi_netif_init()).
void http_server_init(void);

// Requests answered since start-up.
uint32_t http_server_requests(void);

#endif /* HTTP_SERVER_H_ */
