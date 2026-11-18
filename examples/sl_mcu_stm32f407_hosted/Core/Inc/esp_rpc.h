/*
 * esp_rpc.h - ESP-Hosted RPC (control path), host side (no RTOS).
 *
 * RPC messages travel on the serial interface (if_type 3) as two TLVs:
 *   [0x01][len16 LE]["RPCRsp" or "RPCEvt"]   endpoint name
 *   [0x02][len16 LE][protobuf Rpc message]    data
 * The Rpc envelope (rpc_v2.proto) carries msg_type, msg_id, uid and the
 * request/response/event body in the field whose number equals msg_id.
 */

#ifndef ESP_RPC_H_
#define ESP_RPC_H_

#include <stdint.h>
#include <stddef.h>
#include "esp_spi.h"

/* RpcId values (rpc_v2.proto). Response = request + 0x100. */
#define RPC_ID_GET_MAC_ADDRESS          0x101
#define RPC_ID_GET_WIFI_MODE            0x103
#define RPC_ID_SET_WIFI_MODE            0x104
#define RPC_ID_WIFI_SET_PS              0x10e
#define RPC_ID_WIFI_INIT                0x116
#define RPC_ID_WIFI_START               0x118
#define RPC_ID_WIFI_STOP                0x119
#define RPC_ID_WIFI_CONNECT             0x11a
#define RPC_ID_WIFI_DISCONNECT          0x11b
#define RPC_ID_WIFI_SET_CONFIG          0x11c
#define RPC_ID_WIFI_SCAN_START          0x11e
#define RPC_ID_WIFI_SCAN_GET_AP_NUM     0x120
#define RPC_ID_WIFI_SCAN_GET_AP_RECORDS 0x121
#define RPC_ID_GET_CP_FW_VERSION        0x15e
#define RPC_ID_RESP(req)                ((req) + 0x100)

#define RPC_EVENT_ESP_INIT              0x301
#define RPC_EVENT_WIFI_NO_ARGS          0x305
#define RPC_EVENT_STA_SCAN_DONE         0x306
#define RPC_EVENT_STA_CONNECTED         0x307
#define RPC_EVENT_STA_DISCONNECTED      0x308

#define ESP_RPC_TIMEOUT_MS              5000

typedef enum {
    ESP_RPC_OK = 0,
    ESP_RPC_TIMEOUT,
    ESP_RPC_DECODE_ERROR
} esp_rpc_status_t;

// body/len: the response message (e.g. Rpc_Resp_GetMacAddress) or NULL on error
typedef void (*esp_rpc_resp_cb_t)(esp_rpc_status_t status, uint16_t msg_id, const uint8_t *body, size_t len);
typedef void (*esp_rpc_event_cb_t)(uint16_t msg_id, const uint8_t *body, size_t len);

void esp_rpc_init(esp_rpc_event_cb_t event_cb);
void esp_rpc_poll(void);
void esp_rpc_rx(const esp_frame_t *frame);

// Send a request. body is the encoded request message (may be empty).
// Returns 0 if accepted, -1 if a request is already outstanding, -2 if too big.
int esp_rpc_request(uint16_t msg_id, const uint8_t *body, size_t len, esp_rpc_resp_cb_t cb);
int esp_rpc_busy(void);

// Drop any outstanding request (no callback) and partial frames, e.g. after a coprocessor reset.
void esp_rpc_reset(void);

#endif /* ESP_RPC_H_ */
