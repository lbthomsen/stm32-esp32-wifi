/*
 * esp_rpc.c - ESP-Hosted RPC (control path), host side (no RTOS).
 *
 * Requests: esp_rpc_request() wraps the encoded request message in the Rpc
 * envelope (msg_type, msg_id, uid, body) and the two serial TLVs, and keeps
 * it until esp_rpc_poll() can hand it to the SPI transport. Only one request
 * is outstanding at a time; its callback runs when the matching response
 * (msg_id + 0x100) arrives, or with ESP_RPC_TIMEOUT after ESP_RPC_TIMEOUT_MS.
 *
 * Receive: serial frames (if_type 3) are collected until a frame without
 * the "more fragments" flag, then the TLVs are split and the Rpc envelope is
 * decoded. Events ("RPCEvt" endpoint, or msg_type 3) go to the event
 * callback, responses to the waiting request.
 *
 * Everything here runs in main loop context.
 */

#include <stdio.h>
#include <string.h>
#include "esp_rpc.h"
#include "pb.h"

/* Serial TLVs around each RPC message */
#define TLV_EPNAME              0x01
#define TLV_DATA                0x02

/* Endpoint names: requests and responses use "RPCRsp", events "RPCEvt" */
#define EP_REQ                  "RPCRsp"
#define EP_EVT                  "RPCEvt"

/* Rpc envelope (rpc_v2.proto): msg_type values and field numbers */
#define RPC_TYPE_REQ            1
#define RPC_TYPE_RESP           2
#define RPC_TYPE_EVENT          3

#define RPC_FIELD_MSG_TYPE      1
#define RPC_FIELD_MSG_ID        2
#define RPC_FIELD_UID           3

/* Frame header flag: the message continues in the next frame */
#define FLAG_MORE_FRAGMENT      (1 << 0)

#define RX_BUF_SIZE             4096    // largest reassembled message (scan results are the big ones)
#define TX_BUF_SIZE             512     // largest request (set config with SSID and password)

static esp_rpc_event_cb_t event_callback;

// Reassembly of a message split over several frames
static uint8_t rx_buf[RX_BUF_SIZE];
static size_t rx_len;
static uint8_t rx_overflow;     // the message didn't fit: drop it when complete

// The encoded request, until the SPI transmit queue has room for it
static uint8_t tx_buf[TX_BUF_SIZE];
static size_t tx_len;
static uint8_t tx_pending;

// The outstanding request (req_msg_id 0: none)
static uint32_t next_uid = 1;
static uint16_t req_msg_id;
static uint32_t req_uid;
static uint32_t req_tick;
static esp_rpc_resp_cb_t req_callback;

// event_cb gets every RPC event from the coprocessor.
void esp_rpc_init(esp_rpc_event_cb_t event_cb) {
    event_callback = event_cb;
    rx_len = 0;
    rx_overflow = 0;
    tx_pending = 0;
    req_callback = NULL;
    req_msg_id = 0;
}

// Forget the outstanding request (without calling back) and any partial
// message, e.g. after the coprocessor was reset.
void esp_rpc_reset(void) {
    rx_len = 0;
    rx_overflow = 0;
    tx_pending = 0;
    req_callback = NULL;
    req_msg_id = 0;
}

// 1 while a request is waiting for its response.
int esp_rpc_busy(void) {
    return req_msg_id != 0;
}

// Encode a request and queue it. Layout of tx_buf:
//   [0x01][len16]["RPCRsp"] [0x02][len16][Rpc { msg_type, msg_id, uid, <msg_id>: body }]
// The protobuf part is written first, at its final offset, because its
// length goes into the TLV header in front of it.
int esp_rpc_request(uint16_t msg_id, const uint8_t *body, size_t len, esp_rpc_resp_cb_t cb) {
    if (req_msg_id) {
        return -1;
    }

    // Build the Rpc envelope after the TLV headers, then fill those in.
    const size_t ep_len = sizeof(EP_REQ) - 1;
    const size_t data_off = 3 + ep_len + 3;

    pb_writer_t w;
    pb_writer_init(&w, tx_buf + data_off, sizeof(tx_buf) - data_off);
    pb_write_varint_field(&w, RPC_FIELD_MSG_TYPE, RPC_TYPE_REQ);
    pb_write_varint_field(&w, RPC_FIELD_MSG_ID, msg_id);
    pb_write_varint_field(&w, RPC_FIELD_UID, next_uid);
    pb_write_bytes_field(&w, msg_id, body, len);
    if (w.error) {
        return -2;
    }

    uint8_t *p = tx_buf;
    *p++ = TLV_EPNAME;
    *p++ = ep_len & 0xff;
    *p++ = ep_len >> 8;
    memcpy(p, EP_REQ, ep_len);
    p += ep_len;
    *p++ = TLV_DATA;
    *p++ = w.pos & 0xff;
    *p++ = w.pos >> 8;

    tx_len = data_off + w.pos;
    tx_pending = 1;

    req_msg_id = msg_id;
    req_uid = next_uid++;
    req_callback = cb;
    req_tick = HAL_GetTick();
    return 0;
}

// End the outstanding request and run its callback. The request is
// cleared first, so the callback may send the next one.
static void finish_request(esp_rpc_status_t status, uint16_t msg_id, const uint8_t *body, size_t len) {
    esp_rpc_resp_cb_t cb = req_callback;
    req_msg_id = 0;
    req_callback = NULL;
    if (cb) {
        cb(status, msg_id, body, len);
    }
}

// Main loop: send the queued request when the transport has room, and time
// out a request that got no response.
void esp_rpc_poll(void) {
    if (tx_pending && !esp_spi_tx_busy()) {
        if (esp_spi_send(ESP_IF_SERIAL, 0, 0, 0, tx_buf, (uint16_t) tx_len) == 0) {
            tx_pending = 0;
        }
    }

    if (req_msg_id && (HAL_GetTick() - req_tick) > ESP_RPC_TIMEOUT_MS) {
        printf("esp_rpc: request 0x%03x timed out\n", req_msg_id);
        tx_pending = 0;
        finish_request(ESP_RPC_TIMEOUT, req_msg_id, NULL, 0);
    }
}

// Decode one Rpc envelope. The body is the length delimited field whose
// number equals msg_id (always >= 0x100), so any such field is the body.
static void handle_rpc(const uint8_t *data, size_t len, int is_event) {
    pb_reader_t r;
    pb_field_t f;
    uint32_t msg_type = 0, msg_id = 0, uid = 0;
    const uint8_t *body = NULL;
    size_t body_len = 0;

    // msg_id comes before the body on the wire, but don't rely on it.
    pb_reader_init(&r, data, len);
    while (pb_read_field(&r, &f) == 1) {
        if (f.field == RPC_FIELD_MSG_TYPE) {
            msg_type = (uint32_t) f.varint;
        } else if (f.field == RPC_FIELD_MSG_ID) {
            msg_id = (uint32_t) f.varint;
        } else if (f.field == RPC_FIELD_UID) {
            uid = (uint32_t) f.varint;
        } else if (f.wire_type == PB_WT_LEN && f.field >= 0x100) {
            body = f.data;
            body_len = f.len;
        }
    }
    if (r.error || !msg_id) {
        printf("esp_rpc: undecodable message (%u bytes)\n", (unsigned) len);
        if (req_msg_id && !is_event) {
            finish_request(ESP_RPC_DECODE_ERROR, 0, NULL, 0);
        }
        return;
    }

    if (msg_type == RPC_TYPE_EVENT || is_event) {
        if (event_callback) {
            event_callback((uint16_t) msg_id, body, body_len);
        }
        return;
    }

    if (msg_type == RPC_TYPE_RESP && req_msg_id && msg_id == RPC_ID_RESP(req_msg_id)) {
        if (uid != req_uid) {
            printf("esp_rpc: response uid %lu, expected %lu\n", uid, req_uid);
        }
        finish_request(ESP_RPC_OK, (uint16_t) msg_id, body, body_len);
        return;
    }

    printf("esp_rpc: unexpected message type=%lu id=0x%03lx\n", msg_type, msg_id);
}

// Split a complete serial message into its TLVs: endpoint name and data.
static void handle_serial(const uint8_t *buf, size_t len) {
    char ep[16] = { 0 };
    const uint8_t *data = NULL;
    size_t data_len = 0;

    while (len >= 3) {
        uint8_t type = buf[0];
        size_t vlen = buf[1] | (buf[2] << 8);
        if (vlen > len - 3) {
            printf("esp_rpc: truncated TLV\n");
            return;
        }
        if (type == TLV_EPNAME && vlen < sizeof(ep)) {
            memcpy(ep, buf + 3, vlen);
        } else if (type == TLV_DATA) {
            data = buf + 3;
            data_len = vlen;
        }
        buf += 3 + vlen;
        len -= 3 + vlen;
    }

    if (!data) {
        printf("esp_rpc: serial message without data\n");
        return;
    }
    handle_rpc(data, data_len, strcmp(ep, EP_EVT) == 0);
}

// Frames from the serial interface (called by esp_host). Collects
// fragments until the last one, then handles the whole message.
void esp_rpc_rx(const esp_frame_t *frame) {
    if (rx_len + frame->len > sizeof(rx_buf)) {
        rx_overflow = 1;
    } else {
        memcpy(rx_buf + rx_len, frame->payload, frame->len);
        rx_len += frame->len;
    }

    if (frame->flags & FLAG_MORE_FRAGMENT) {
        return;
    }

    if (rx_overflow) {
        printf("esp_rpc: message too big for rx buffer, dropped\n");
    } else {
        handle_serial(rx_buf, rx_len);
    }
    rx_len = 0;
    rx_overflow = 0;
}
