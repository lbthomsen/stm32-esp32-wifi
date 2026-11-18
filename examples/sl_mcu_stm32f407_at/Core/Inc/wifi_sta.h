/*
 * wifi_sta.h - Wi-Fi station control over ESP-Hosted RPC (no RTOS).
 */

#ifndef WIFI_STA_H_
#define WIFI_STA_H_

#include <stdint.h>

typedef enum {
    WIFI_STA_WAIT_CP = 0,
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

void wifi_sta_init(const char *ssid, const char *password);
void wifi_sta_poll(void);
wifi_sta_state_t wifi_sta_get_state(void);
const uint8_t* wifi_sta_get_mac(void);

// Hook for esp_host: RPC events from the coprocessor
void wifi_sta_rpc_event(uint16_t msg_id, const uint8_t *body, size_t len);

#endif /* WIFI_STA_H_ */
