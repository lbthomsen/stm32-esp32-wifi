/*
 * at_client.h - AT command layer over at_spi (no RTOS).
 *
 * One command at a time, non-blocking: at_client_command() queues it and the
 * done callback reports OK / ERROR / timeout. Lines that belong to the
 * response (e.g. "+CIPSTA:ip:...") go to the line callback. Unsolicited lines
 * ("ready", "WIFI GOT IP", "0,CONNECT", ...) go to the URC callback, and
 * socket data ("+IPD,<link>,<len>:<data>") to the IPD callback, in chunks of
 * up to AT_IPD_CHUNK bytes as it arrives (a large +IPD gives several calls).
 */

#ifndef AT_CLIENT_H_
#define AT_CLIENT_H_

#include <stdint.h>
#include <stddef.h>

#define AT_LINE_MAX         256
#define AT_SEND_MAX         2048    // largest payload for at_client_send()
#define AT_IPD_CHUNK        512     // +IPD data is passed on in pieces of at most this

typedef enum {
    AT_OK = 0,
    AT_ERROR,
    AT_TIMEOUT,
    AT_SEND_FAIL
} at_result_t;

typedef void (*at_line_cb_t)(const char *line);
typedef void (*at_done_cb_t)(at_result_t result);
typedef void (*at_urc_cb_t)(const char *line);
typedef void (*at_ipd_cb_t)(int link, const uint8_t *data, size_t len);

// urc_cb gets unsolicited lines, ipd_cb socket data.
void at_client_init(at_urc_cb_t urc_cb, at_ipd_cb_t ipd_cb);

// Forget any command in progress and partial input (after resetting the ESP).
void at_client_reset(void);

// Main loop: parse what at_spi received, time out the command in progress.
void at_client_poll(void);

// 1 while a command is in progress (only one at a time).
int at_client_busy(void);

// Send "cmd\r\n". Returns 0 if accepted, -1 if busy, -2 if it doesn't fit.
int at_client_command(const char *cmd, uint32_t timeout_ms, at_line_cb_t line_cb, at_done_cb_t done_cb);

// AT+CIPSEND=<link>,<len>, wait for '>', send the data, wait for SEND OK.
int at_client_send(int link, const uint8_t *data, size_t len, uint32_t timeout_ms, at_done_cb_t done_cb);

// HAL tick of the last byte received from the ESP
uint32_t at_client_last_rx(void);

// Result as text, for logs.
const char* at_result_name(at_result_t r);

#endif /* AT_CLIENT_H_ */
