/*
 * wifi_sta.h - Wi-Fi station control over ESP-Hosted RPC (no RTOS).
 *
 * Brings the coprocessor's Wi-Fi up as a station and keeps it connected,
 * one RPC request at a time (see wifi_sta.c for the sequence).
 */

#ifndef WIFI_STA_H_
#define WIFI_STA_H_

#include <stdint.h>

// Bring-up states, in order (RETRY_WAIT and ERROR are side tracks)
typedef enum {
    WIFI_STA_WAIT_CP = 0,       // waiting for the coprocessor's RPC init event
    WIFI_STA_GET_FW,
    WIFI_STA_SET_MODE,
    WIFI_STA_GET_MAC,
    WIFI_STA_START,
    WIFI_STA_SET_PS,
    WIFI_STA_SCAN,
    WIFI_STA_SCAN_WAIT,
    WIFI_STA_SCAN_RESULTS,
    WIFI_STA_SET_CONFIG,
    WIFI_STA_CONNECT,
    WIFI_STA_CONNECT_WAIT,
    WIFI_STA_CONNECTED,
    WIFI_STA_RETRY_WAIT,
    WIFI_STA_ERROR
} wifi_sta_state_t;

// The network to join (strings are kept, not copied).
void wifi_sta_init(const char *ssid, const char *password);

// Main loop: the next request of the sequence, liveness and RSSI checks.
void wifi_sta_poll(void);

wifi_sta_state_t wifi_sta_get_state(void);

// Station MAC address (valid once the sequence is past GET_MAC).
const uint8_t* wifi_sta_get_mac(void);
const char* wifi_sta_get_ssid(void);
int wifi_sta_get_rssi(void);     // dBm of the AP we're connected to, 0 if unknown

// Hook for esp_host: RPC events from the coprocessor
void wifi_sta_rpc_event(uint16_t msg_id, const uint8_t *body, size_t len);

#endif /* WIFI_STA_H_ */
