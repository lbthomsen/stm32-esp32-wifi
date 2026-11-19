/*
 * esp_host.h - ESP-Hosted host protocol layer (no RTOS).
 *
 * Sits on top of esp_spi: handles the coprocessor init event and the
 * host capability reply, and routes frames to the RPC and network layers.
 */

#ifndef ESP_HOST_H_
#define ESP_HOST_H_

#include <stdint.h>
#include "esp_spi.h"

// What the coprocessor announced in its init event
typedef struct {
    uint8_t ready;              // init event received and answered
    uint8_t chip_id;
    uint8_t caps;
    uint32_t ext_caps;
    uint32_t fw_version;
    uint8_t rpc_version;
    uint8_t rx_q_size;
    uint8_t tx_q_size;
    uint32_t transfer_size;
} esp_host_info_t;

typedef void (*esp_host_data_rx_cb_t)(const uint8_t *frame, uint16_t len);

// Set up the SPI transport and reset the coprocessor.
void esp_host_init(SPI_HandleTypeDef *hspi);
void esp_host_set_sta_rx(esp_host_data_rx_cb_t cb);   // Ethernet frames from the STA interface
// Main loop: received frames, RPC traffic and the init handshake.
void esp_host_poll(void);

const esp_host_info_t* esp_host_get_info(void);

// HAL tick of the last frame received from the coprocessor (any interface)
uint32_t esp_host_last_rx(void);

// Reset the coprocessor through its EN pin and wait for a fresh init event
void esp_host_restart_cp(void);

#endif /* ESP_HOST_H_ */
