/*
 * at_client.c - AT command layer over at_spi (no RTOS).
 *
 * ESP-AT sends lines ending in "\r\n". Two things are not lines:
 *  - "+IPD,<link>,<len>:" is followed by <len> raw bytes (socket data).
 *  - After AT+CIPSEND the ESP sends "OK" and then a bare '>' prompt; the
 *    host then sends the raw data and gets "Recv N bytes" and "SEND OK".
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "main.h"
#include "at_client.h"
#include "at_spi.h"

typedef enum {
    CMD_IDLE = 0,
    CMD_WAIT_FINAL,     // waiting for OK / ERROR
    CMD_WAIT_PROMPT,    // CIPSEND: waiting for '>'
    CMD_WAIT_SEND_OK    // CIPSEND: data sent, waiting for SEND OK / SEND FAIL
} cmd_state_t;

static at_urc_cb_t urc_callback;
static at_ipd_cb_t ipd_callback;

static cmd_state_t cmd_state;
static at_line_cb_t line_callback;
static at_done_cb_t done_callback;
static uint32_t cmd_tick;
static uint32_t cmd_timeout;

static char line[AT_LINE_MAX];
static size_t line_len;

// +IPD payload being received, passed on in chunks
static uint8_t ipd_buf[AT_IPD_CHUNK];
static size_t ipd_fill;
static int ipd_link;
static size_t ipd_len;
static size_t ipd_got;
static uint8_t in_ipd;

// CIPSEND payload waiting for the prompt
static uint8_t send_buf[AT_SEND_MAX];
static size_t send_len;

static uint32_t last_rx_tick;

const char* at_result_name(at_result_t r) {
    switch (r) {
    case AT_OK: return "OK";
    case AT_ERROR: return "ERROR";
    case AT_TIMEOUT: return "timeout";
    case AT_SEND_FAIL: return "SEND FAIL";
    default: return "?";
    }
}

static void finish(at_result_t result) {
    at_done_cb_t cb = done_callback;
    cmd_state = CMD_IDLE;
    line_callback = NULL;
    done_callback = NULL;
    if (cb) {
        cb(result);
    }
}

// Lines the ESP sends on its own, whether or not a command is running.
static int is_urc(const char *s) {
    if (strcmp(s, "ready") == 0 || strncmp(s, "WIFI ", 5) == 0 || strncmp(s, "+LINK_CONN", 10) == 0
            || strncmp(s, "+STA_", 5) == 0 || strncmp(s, "+TIME_UPDATED", 13) == 0) {
        return 1;
    }
    // "<link>,CONNECT", "<link>,CLOSED", "<link>,CONNECT FAIL"
    if (s[0] >= '0' && s[0] <= '9' && s[1] == ',') {
        return strncmp(s + 2, "CONNECT", 7) == 0 || strncmp(s + 2, "CLOSED", 6) == 0;
    }
    return 0;
}

static void handle_line(const char *s) {
    if (s[0] == 0) {
        return;
    }

    if (is_urc(s)) {
        if (urc_callback) {
            urc_callback(s);
        }
        return;
    }

    switch (cmd_state) {
    case CMD_WAIT_FINAL:
        if (strcmp(s, "OK") == 0) {
            finish(AT_OK);
        } else if (strcmp(s, "ERROR") == 0 || strcmp(s, "FAIL") == 0) {
            finish(AT_ERROR);
        } else if (strncmp(s, "busy", 4) == 0 || strncmp(s, "AT", 2) == 0) {
            // "busy p..." while it works on a previous command; "AT..." is the echo before ATE0
        } else if (line_callback) {
            line_callback(s);
        }
        break;

    case CMD_WAIT_PROMPT:
        if (strcmp(s, "ERROR") == 0) {
            finish(AT_ERROR);
        }
        // "OK" comes before the prompt; nothing else to do
        break;

    case CMD_WAIT_SEND_OK:
        if (strcmp(s, "SEND OK") == 0) {
            finish(AT_OK);
        } else if (strcmp(s, "SEND FAIL") == 0) {
            finish(AT_SEND_FAIL);
        } else if (strcmp(s, "ERROR") == 0) {
            finish(AT_ERROR);
        }
        // "Recv N bytes" is just progress
        break;

    case CMD_IDLE:
    default:
        if (urc_callback) {
            urc_callback(s);
        }
        break;
    }
}

// "+IPD,<link>,<len>:" (CIPMUX=1) or "+IPD,<len>:" (CIPMUX=0)
static void start_ipd(void) {
    line[line_len] = 0;
    char *p = line + 5;
    long a = strtol(p, &p, 10);
    if (*p == ',') {
        ipd_link = (int) a;
        ipd_len = (size_t) strtol(p + 1, &p, 10);
    } else {
        ipd_link = 0;
        ipd_len = (size_t) a;
    }
    ipd_got = 0;
    ipd_fill = 0;
    in_ipd = ipd_len > 0;
    line_len = 0;
}

static void ipd_byte(uint8_t b) {
    ipd_buf[ipd_fill++] = b;
    ipd_got++;
    if (ipd_fill == sizeof(ipd_buf) || ipd_got >= ipd_len) {
        if (ipd_callback) {
            ipd_callback(ipd_link, ipd_buf, ipd_fill);
        }
        ipd_fill = 0;
    }
    if (ipd_got >= ipd_len) {
        in_ipd = 0;
    }
}

static void rx_byte(uint8_t b) {
    if (in_ipd) {
        ipd_byte(b);
        return;
    }

    // The CIPSEND prompt has no line ending
    if (cmd_state == CMD_WAIT_PROMPT && line_len == 0 && b == '>') {
        at_spi_write(send_buf, send_len);
        cmd_state = CMD_WAIT_SEND_OK;
        return;
    }

    if (b == '\n') {
        line[line_len] = 0;
        handle_line(line);
        line_len = 0;
        return;
    }
    if (b == '\r') {
        return;
    }
    if (line_len < sizeof(line) - 1) {
        line[line_len++] = (char) b;
    }

    if (b == ':' && line_len > 5 && strncmp(line, "+IPD,", 5) == 0) {
        start_ipd();
    }
}

void at_client_init(at_urc_cb_t urc_cb, at_ipd_cb_t ipd_cb) {
    urc_callback = urc_cb;
    ipd_callback = ipd_cb;
    at_client_reset();
}

void at_client_reset(void) {
    cmd_state = CMD_IDLE;
    line_callback = NULL;
    done_callback = NULL;
    line_len = 0;
    in_ipd = 0;
    send_len = 0;
    last_rx_tick = HAL_GetTick();
}

void at_client_poll(void) {
    uint8_t buf[64];
    size_t n;
    while ((n = at_spi_read(buf, sizeof(buf))) > 0) {
        last_rx_tick = HAL_GetTick();
        for (size_t i = 0; i < n; ++i) {
            rx_byte(buf[i]);
        }
    }

    if (cmd_state != CMD_IDLE && HAL_GetTick() - cmd_tick > cmd_timeout) {
        finish(AT_TIMEOUT);
    }
}

int at_client_busy(void) {
    return cmd_state != CMD_IDLE;
}

int at_client_command(const char *cmd, uint32_t timeout_ms, at_line_cb_t line_cb, at_done_cb_t done_cb) {
    if (cmd_state != CMD_IDLE) {
        return -1;
    }
    size_t len = strlen(cmd);
    if (len + 2 > at_spi_tx_free()) {
        return -2;
    }
    at_spi_write(cmd, len);
    at_spi_write("\r\n", 2);

    line_callback = line_cb;
    done_callback = done_cb;
    cmd_timeout = timeout_ms;
    cmd_tick = HAL_GetTick();
    cmd_state = CMD_WAIT_FINAL;
    return 0;
}

int at_client_send(int link, const uint8_t *data, size_t len, uint32_t timeout_ms, at_done_cb_t done_cb) {
    if (len == 0 || len > sizeof(send_buf)) {
        return -2;
    }
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d,%u", link, (unsigned) len);
    memcpy(send_buf, data, len);
    send_len = len;

    int r = at_client_command(cmd, timeout_ms, NULL, done_cb);
    if (r == 0) {
        cmd_state = CMD_WAIT_PROMPT;
    }
    return r;
}

uint32_t at_client_last_rx(void) {
    return last_rx_tick;
}
