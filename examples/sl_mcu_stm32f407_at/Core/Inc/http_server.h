/*
 * http_server.h - minimal HTTP/1.0 status page server over ESP-AT (no RTOS).
 *
 * Runs on wifi_at's TCP server (port HTTP_SERVER_PORT). Event driven: the
 * link handler collects each request as its data arrives; once the headers
 * are complete, http_server_poll() sends the reply and closes the link.
 *
 *   GET /     status page (HTML)
 *   anything else: 404
 */

#ifndef HTTP_SERVER_H_
#define HTTP_SERVER_H_

#include <stdint.h>
#include "wifi_at.h"

#define HTTP_SERVER_PORT 80

extern const wifi_at_link_handler_t http_server_handler;

void http_server_poll(void);

uint32_t http_server_requests(void);

#endif /* HTTP_SERVER_H_ */
