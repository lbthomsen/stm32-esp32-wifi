/*
 * wifi_sta.c - Wi-Fi station control over ESP-Hosted RPC (no RTOS).
 *
 * The coprocessor initialises its Wi-Fi driver itself at boot, so the host
 * only sets the mode, starts Wi-Fi, scans and connects. Message and field
 * numbers are from esp_hosted common/serializers/eh_proto_v2/src/proto/rpc_v2.proto.
 *
 * A state machine, one RPC request at a time: wifi_sta_poll() sends the
 * request for the current state, and the response callback moves to the
 * next state. The sequence is
 *   WAIT_CP -> GET_FW -> SET_MODE -> GET_MAC -> START -> SET_PS -> SCAN ->
 *   SCAN_WAIT -> SCAN_RESULTS -> SET_CONFIG -> CONNECT -> CONNECT_WAIT ->
 *   CONNECTED
 * Events (connected, disconnected, scan done, init) come in through
 * wifi_sta_rpc_event(). A failed request or a silent coprocessor (no frame
 * for LIVENESS_IDLE_MS, then a probe that times out) resets the coprocessor
 * and starts over; a Wi-Fi disconnect retries the connect after a delay.
 */

#include <stdio.h>
#include <string.h>
#include "main.h"
#include "wifi_sta.h"
#include "esp_rpc.h"
#include "esp_host.h"
#include "pb.h"

#define WIFI_MODE_STA           1
#define WIFI_IF_STA             0
#define WIFI_PS_NONE            0   // radio always on: low latency, more current
#define SCAN_MAX_RECORDS        16
#define RETRY_DELAY_MS          5000
#define NOT_INIT_RETRY_MS       500
#define LIVENESS_IDLE_MS        10000   // probe the coprocessor after this long without traffic
#define RSSI_PERIOD_MS          30000   // ask for the AP's signal strength this often
#define ESP_ERR_WIFI_NOT_INIT   0x3001  // CP announced itself before its app_main ran esp_wifi_init()

/* wifi_event_t values (IDF esp_wifi_types.h) for Event_WifiEventNoArgs, for the log */
static const char* wifi_event_name(int32_t id) {
    switch (id) {
    case 0: return "WIFI_READY";
    case 1: return "SCAN_DONE";
    case 2: return "STA_START";
    case 3: return "STA_STOP";
    case 4: return "STA_CONNECTED";
    case 5: return "STA_DISCONNECTED";
    default: return "?";
    }
}

// wifi_auth_mode_t as text, for the scan list
static const char* auth_name(int32_t a) {
    static const char *names[] = { "open", "wep", "wpa", "wpa2", "wpa/wpa2", "wpa2-ent", "wpa3", "wpa2/wpa3", "wapi", "owe", "wpa3-ent", "wpa3-ent-192" };
    return (a >= 0 && a < (int32_t) (sizeof(names) / sizeof(names[0]))) ? names[a] : "?";
}

static wifi_sta_state_t state;
static const char *sta_ssid;        // credentials from wifi_secrets.h
static const char *sta_password;
static uint8_t mac[6];
static uint8_t cp_up;               // the coprocessor's RPC init event has arrived
static uint8_t scan_done;           // scan done event seen
static uint32_t retry_tick;         // RETRY_WAIT: go to retry_state after retry_delay
static uint32_t retry_delay;
static wifi_sta_state_t retry_state;
static uint8_t req_in_flight;       // a request of ours is waiting for its response
static uint32_t last_probe_tick;
static uint32_t last_rssi_tick;
static int rssi;

static void set_state(wifi_sta_state_t s) {
    state = s;
}

/* Coprocessor gone (request timed out) or restarted: reset it through EN and
 * start over. The netif follows the state, so the link goes down and DHCP
 * runs again once Wi-Fi is back. */
static void restart_cp(void) {
    req_in_flight = 0;
    rssi = 0;
    cp_up = 0;
    scan_done = 0;
    esp_host_restart_cp();
    set_state(WIFI_STA_WAIT_CP);
}

// Go to state s after delay_ms (via RETRY_WAIT).
static void retry_later(wifi_sta_state_t s, uint32_t delay_ms) {
    retry_state = s;
    retry_delay = delay_ms;
    retry_tick = HAL_GetTick();
    set_state(WIFI_STA_RETRY_WAIT);
}

// The esp_err_t result in a response: the varint in `field` (absent = 0).
static int32_t resp_code(const uint8_t *body, size_t len, uint32_t field) {
    pb_reader_t r;
    pb_field_t f;
    pb_reader_init(&r, body, len);
    while (pb_read_field(&r, &f) == 1) {
        if (f.field == field && f.wire_type == PB_WT_VARINT) {
            return pb_as_int32(&f);
        }
    }
    return 0;   // proto3: absent means 0 (ESP_OK)
}

/* Generic response: fields named "resp" are field 1 in most messages. */
static void on_simple_resp(esp_rpc_status_t status, uint16_t msg_id, const uint8_t *body, size_t len) {
    req_in_flight = 0;
    if (status != ESP_RPC_OK) {
        printf("wifi_sta: request failed (status %d) in state %d\n", status, state);
        restart_cp();
        return;
    }
    int32_t resp = resp_code(body, len, 1);

    switch (state) {
    case WIFI_STA_SET_MODE:
        printf("wifi_sta: set mode STA -> %ld\n", resp);
        if (resp == ESP_ERR_WIFI_NOT_INIT) {
            retry_later(WIFI_STA_SET_MODE, NOT_INIT_RETRY_MS);
        } else {
            set_state(resp ? WIFI_STA_ERROR : WIFI_STA_GET_MAC);
        }
        break;
    case WIFI_STA_START:
        printf("wifi_sta: wifi start -> %ld\n", resp);
        set_state(resp ? WIFI_STA_ERROR : WIFI_STA_SET_PS);
        break;
    case WIFI_STA_SET_PS:
        printf("wifi_sta: power save off -> %ld\n", resp);
        set_state(WIFI_STA_SCAN);   // not fatal if it fails
        break;
    case WIFI_STA_SCAN:
        printf("wifi_sta: scan start -> %ld\n", resp);
        set_state(resp ? WIFI_STA_ERROR : WIFI_STA_SCAN_WAIT);
        break;
    case WIFI_STA_SET_CONFIG:
        printf("wifi_sta: set config -> %ld\n", resp);
        set_state(resp ? WIFI_STA_ERROR : WIFI_STA_CONNECT);
        break;
    case WIFI_STA_CONNECT:
        printf("wifi_sta: connect -> %ld\n", resp);
        set_state(resp ? WIFI_STA_ERROR : WIFI_STA_CONNECT_WAIT);
        break;
    default:
        printf("wifi_sta: response 0x%03x in state %d\n", msg_id, state);
        break;
    }
}

// Rpc_Resp_GetCoprocessorFwVersion: major/minor/patch in fields 2..4, target name in 9.
static void on_fw_version(esp_rpc_status_t status, uint16_t msg_id, const uint8_t *body, size_t len) {
    (void) msg_id;
    req_in_flight = 0;
    if (status != ESP_RPC_OK) {
        printf("wifi_sta: request 0x%03x failed (status %d)\n", msg_id, status);
        restart_cp();
        return;
    }
    uint32_t v[5] = { 0 };
    char target[16] = { 0 };
    pb_reader_t r;
    pb_field_t f;
    pb_reader_init(&r, body, len);
    while (pb_read_field(&r, &f) == 1) {
        if (f.field >= 2 && f.field <= 4) {
            v[f.field] = (uint32_t) f.varint;
        } else if (f.field == 9 && f.wire_type == PB_WT_LEN && f.len < sizeof(target)) {
            memcpy(target, f.data, f.len);
        }
    }
    printf("wifi_sta: coprocessor firmware %lu.%lu.%lu on %s\n", v[2], v[3], v[4], target);
    set_state(WIFI_STA_SET_MODE);
}

// Rpc_Resp_GetMacAddress: the 6 byte MAC in field 1 (lwIP needs it for the netif).
static void on_mac(esp_rpc_status_t status, uint16_t msg_id, const uint8_t *body, size_t len) {
    (void) msg_id;
    req_in_flight = 0;
    if (status != ESP_RPC_OK) {
        printf("wifi_sta: request 0x%03x failed (status %d)\n", msg_id, status);
        restart_cp();
        return;
    }
    pb_reader_t r;
    pb_field_t f;
    pb_reader_init(&r, body, len);
    while (pb_read_field(&r, &f) == 1) {
        if (f.field == 1 && f.wire_type == PB_WT_LEN && f.len == 6) {
            memcpy(mac, f.data, 6);
        }
    }
    printf("wifi_sta: STA MAC %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    set_state(WIFI_STA_START);
}

// One wifi_ap_record from the scan: bssid 1, ssid 2, channel 3, rssi 5, authmode 6.
static void print_ap_record(const uint8_t *data, size_t len) {
    char ssid[33] = { 0 };
    uint8_t bssid[6] = { 0 };
    uint32_t channel = 0;
    int32_t rssi = 0, auth = 0;
    pb_reader_t r;
    pb_field_t f;
    pb_reader_init(&r, data, len);
    while (pb_read_field(&r, &f) == 1) {
        switch (f.field) {
        case 1:
            if (f.len == 6) {
                memcpy(bssid, f.data, 6);
            }
            break;
        case 2:
            memcpy(ssid, f.data, f.len < 32 ? f.len : 32);
            break;
        case 3:
            channel = (uint32_t) f.varint;
            break;
        case 5:
            rssi = pb_as_int32(&f);
            break;
        case 6:
            auth = pb_as_int32(&f);
            break;
        }
    }
    printf("  %-32s %02x:%02x:%02x:%02x:%02x:%02x ch %2lu rssi %4ld %s\n", ssid,
            bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], channel, rssi, auth_name(auth));
}

// Rpc_Resp_WifiScanGetApRecords: count in field 2, records in field 3.
static void on_ap_records(esp_rpc_status_t status, uint16_t msg_id, const uint8_t *body, size_t len) {
    (void) msg_id;
    req_in_flight = 0;
    if (status != ESP_RPC_OK) {
        printf("wifi_sta: request 0x%03x failed (status %d)\n", msg_id, status);
        restart_cp();
        return;
    }
    int32_t number = 0;
    pb_reader_t r;
    pb_field_t f;
    pb_reader_init(&r, body, len);
    while (pb_read_field(&r, &f) == 1) {
        if (f.field == 2) {
            number = pb_as_int32(&f);
            printf("wifi_sta: %ld access points:\n", number);
        } else if (f.field == 3 && f.wire_type == PB_WT_LEN) {
            print_ap_record(f.data, f.len);
        }
    }
    set_state(WIFI_STA_SET_CONFIG);
}

// Liveness probe answered (or not).
static void on_probe(esp_rpc_status_t status, uint16_t msg_id, const uint8_t *body, size_t len) {
    (void) msg_id;
    (void) body;
    (void) len;
    req_in_flight = 0;
    if (status != ESP_RPC_OK) {
        printf("wifi_sta: coprocessor not responding\n");
        restart_cp();
    }
}

// Rpc_Resp_WifiStaGetApInfo { resp = 1; wifi_ap_record ap_record = 2 { ... rssi = 5 ... } }
static void on_ap_info(esp_rpc_status_t status, uint16_t msg_id, const uint8_t *body, size_t len) {
    (void) msg_id;
    req_in_flight = 0;
    if (status != ESP_RPC_OK) {
        printf("wifi_sta: coprocessor not responding\n");
        restart_cp();
        return;
    }
    pb_reader_t r;
    pb_field_t f;
    pb_reader_init(&r, body, len);
    while (pb_read_field(&r, &f) == 1) {
        if (f.field == 2 && f.wire_type == PB_WT_LEN) {
            pb_reader_t r2;
            pb_field_t f2;
            pb_reader_init(&r2, f.data, f.len);
            while (pb_read_field(&r2, &f2) == 1) {
                if (f2.field == 5) {
                    rssi = pb_as_int32(&f2);
                }
            }
        }
    }
}

// Send a request; the callback clears req_in_flight.
static void send(uint16_t msg_id, const uint8_t *body, size_t len, esp_rpc_resp_cb_t cb) {
    if (esp_rpc_request(msg_id, body, len, cb) == 0) {
        req_in_flight = 1;
    }
}

// RPC events from the coprocessor (registered with esp_rpc_init()).
void wifi_sta_rpc_event(uint16_t msg_id, const uint8_t *body, size_t len) {
    pb_reader_t r;
    pb_field_t f;

    switch (msg_id) {
    case RPC_EVENT_ESP_INIT:
        printf("wifi_sta: coprocessor RPC ready\n");
        cp_up = 1;
        if (state != WIFI_STA_WAIT_CP) {
            // It restarted under us: run the whole sequence again.
            req_in_flight = 0;
            scan_done = 0;
            set_state(WIFI_STA_GET_FW);
        }
        break;

    case RPC_EVENT_WIFI_NO_ARGS: {
        int32_t id = resp_code(body, len, 2);
        printf("wifi_sta: event %s\n", wifi_event_name(id));
        break;
    }

    case RPC_EVENT_STA_SCAN_DONE:
        printf("wifi_sta: scan done\n");
        scan_done = 1;
        break;

    case RPC_EVENT_STA_CONNECTED: {
        char ssid[33] = { 0 };
        uint32_t channel = 0;
        pb_reader_init(&r, body, len);
        while (pb_read_field(&r, &f) == 1) {
            if (f.field == 2 && f.wire_type == PB_WT_LEN) {
                pb_reader_t r2;
                pb_field_t f2;
                pb_reader_init(&r2, f.data, f.len);
                while (pb_read_field(&r2, &f2) == 1) {
                    if (f2.field == 1) {
                        memcpy(ssid, f2.data, f2.len < 32 ? f2.len : 32);
                    } else if (f2.field == 4) {
                        channel = (uint32_t) f2.varint;
                    }
                }
            }
        }
        printf("wifi_sta: CONNECTED to \"%s\" on channel %lu\n", ssid, channel);
        last_rssi_tick = HAL_GetTick() - RSSI_PERIOD_MS;  // ask right away
        set_state(WIFI_STA_CONNECTED);
        break;
    }

    case RPC_EVENT_STA_DISCONNECTED: {
        uint32_t reason = 0;
        pb_reader_init(&r, body, len);
        while (pb_read_field(&r, &f) == 1) {
            if (f.field == 2 && f.wire_type == PB_WT_LEN) {
                pb_reader_t r2;
                pb_field_t f2;
                pb_reader_init(&r2, f.data, f.len);
                while (pb_read_field(&r2, &f2) == 1) {
                    if (f2.field == 4) {
                        reason = (uint32_t) f2.varint;
                    }
                }
            }
        }
        printf("wifi_sta: disconnected, reason %lu - retrying in %u s\n", reason, RETRY_DELAY_MS / 1000);
        rssi = 0;
        retry_later(WIFI_STA_CONNECT, RETRY_DELAY_MS);
        break;
    }

    default:
        printf("wifi_sta: event 0x%03x (%u bytes)\n", msg_id, (unsigned) len);
        break;
    }
}

// The network to join. The strings must stay valid (they're kept, not copied).
void wifi_sta_init(const char *ssid, const char *password) {
    sta_ssid = ssid;
    sta_password = password;
    cp_up = 0;
    scan_done = 0;
    req_in_flight = 0;
    set_state(WIFI_STA_WAIT_CP);
}

// Main loop: liveness and RSSI checks, and the next request of the sequence.
void wifi_sta_poll(void) {
    if (req_in_flight || esp_rpc_busy()) {
        return;
    }

    uint8_t buf[128];
    pb_writer_t w;

    // Liveness: a quiet link might be a dead coprocessor. Ask it something cheap.
    uint32_t now = HAL_GetTick();
    if (state != WIFI_STA_WAIT_CP && state != WIFI_STA_ERROR
            && now - esp_host_last_rx() > LIVENESS_IDLE_MS && now - last_probe_tick > LIVENESS_IDLE_MS) {
        last_probe_tick = now;
        send(RPC_ID_GET_CP_FW_VERSION, NULL, 0, on_probe);
        return;
    }

    switch (state) {
    case WIFI_STA_WAIT_CP:
        if (cp_up) {
            set_state(WIFI_STA_GET_FW);
        }
        break;

    case WIFI_STA_GET_FW:
        send(RPC_ID_GET_CP_FW_VERSION, NULL, 0, on_fw_version);
        break;

    case WIFI_STA_SET_MODE:
        pb_writer_init(&w, buf, sizeof(buf));
        pb_write_int32_field(&w, 1, WIFI_MODE_STA);
        send(RPC_ID_SET_WIFI_MODE, buf, w.pos, on_simple_resp);
        break;

    case WIFI_STA_GET_MAC:
        pb_writer_init(&w, buf, sizeof(buf));
        pb_write_int32_field(&w, 1, WIFI_IF_STA);
        send(RPC_ID_GET_MAC_ADDRESS, buf, w.pos, on_mac);
        break;

    case WIFI_STA_START:
        send(RPC_ID_WIFI_START, NULL, 0, on_simple_resp);
        break;

    case WIFI_STA_SET_PS:
        pb_writer_init(&w, buf, sizeof(buf));
        pb_write_int32_field(&w, 1, WIFI_PS_NONE);
        send(RPC_ID_WIFI_SET_PS, buf, w.pos, on_simple_resp);
        break;

    case WIFI_STA_SCAN:
        scan_done = 0;
        send(RPC_ID_WIFI_SCAN_START, NULL, 0, on_simple_resp);   // default scan config, non-blocking
        break;

    case WIFI_STA_SCAN_WAIT:
        if (scan_done) {
            set_state(WIFI_STA_SCAN_RESULTS);
        }
        break;

    case WIFI_STA_SCAN_RESULTS:
        pb_writer_init(&w, buf, sizeof(buf));
        pb_write_int32_field(&w, 1, SCAN_MAX_RECORDS);
        send(RPC_ID_WIFI_SCAN_GET_AP_RECORDS, buf, w.pos, on_ap_records);
        break;

    case WIFI_STA_SET_CONFIG: {
        // Rpc_Req_WifiSetConfig { iface = 1; wifi_config cfg = 2 { wifi_sta_config sta = 2 { ssid = 1; password = 2; } } }
        pb_writer_init(&w, buf, sizeof(buf));
        pb_write_int32_field(&w, 1, WIFI_IF_STA);
        size_t cfg = pb_write_nested_start(&w, 2);
        size_t sta = pb_write_nested_start(&w, 2);
        pb_write_bytes_field(&w, 1, sta_ssid, strlen(sta_ssid));
        pb_write_bytes_field(&w, 2, sta_password, strlen(sta_password));
        pb_write_nested_end(&w, sta);
        pb_write_nested_end(&w, cfg);
        if (w.error) {
            printf("wifi_sta: config too big\n");
            set_state(WIFI_STA_ERROR);
            break;
        }
        printf("wifi_sta: connecting to \"%s\"\n", sta_ssid);
        send(RPC_ID_WIFI_SET_CONFIG, buf, w.pos, on_simple_resp);
        break;
    }

    case WIFI_STA_CONNECT:
        send(RPC_ID_WIFI_CONNECT, NULL, 0, on_simple_resp);
        break;

    case WIFI_STA_RETRY_WAIT:
        if (HAL_GetTick() - retry_tick > retry_delay) {
            set_state(retry_state);
        }
        break;

    case WIFI_STA_CONNECTED:
        if (now - last_rssi_tick > RSSI_PERIOD_MS) {
            last_rssi_tick = now;
            send(RPC_ID_WIFI_STA_GET_AP_INFO, NULL, 0, on_ap_info);
        }
        break;

    case WIFI_STA_CONNECT_WAIT:
    case WIFI_STA_ERROR:
    default:
        break;
    }
}

wifi_sta_state_t wifi_sta_get_state(void) {
    return state;
}

const uint8_t* wifi_sta_get_mac(void) {
    return mac;
}

const char* wifi_sta_get_ssid(void) {
    return sta_ssid;
}

int wifi_sta_get_rssi(void) {
    return rssi;
}
