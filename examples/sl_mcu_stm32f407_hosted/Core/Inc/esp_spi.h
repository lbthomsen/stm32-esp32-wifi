/*
 * esp_spi.h - ESP-Hosted SPI full duplex transport, host side (no RTOS).
 *
 * Talks to an ESP32 running the ESP-Hosted coprocessor firmware over SPI
 * mode 3 with handshake and data ready lines. The transfers run from
 * interrupts (HS/DR EXTI and SPI DMA completion); received frames are
 * queued and handed to the rx callback from esp_spi_poll() in the main
 * loop, never from an interrupt.
 */

#ifndef ESP_SPI_H_
#define ESP_SPI_H_

#include <stdint.h>
#include "main.h"

#define ESP_SPI_BUF_SIZE        1600
#define ESP_SPI_HDR_SIZE        12
#define ESP_SPI_TX_SLOTS        4       // frames queued for the coprocessor
#define ESP_SPI_RX_SLOTS        4       // received frames waiting for the main loop

/* Interface types on the wire (eh_common_interface.h) */
#define ESP_IF_INVALID          0
#define ESP_IF_STA              1
#define ESP_IF_AP               2
#define ESP_IF_SERIAL           3
#define ESP_IF_HCI              4
#define ESP_IF_PRIV             5
#define ESP_IF_TEST             6
#define ESP_IF_AUX              7
#define ESP_IF_DUMMY            8

// A received frame, as passed to the rx callback. payload points into the
// transport's buffer and is only valid during the callback.
typedef struct {
    uint8_t if_type;
    uint8_t if_num;
    uint8_t flags;
    uint8_t pkt_type;
    uint16_t seq_num;
    uint16_t len;
    const uint8_t *payload;
} esp_frame_t;

typedef void (*esp_spi_rx_cb_t)(const esp_frame_t *frame);

typedef struct {
    uint32_t transactions;      // SPI transactions clocked
    uint32_t rx_frames;         // good frames received (passed up)
    uint32_t rx_dummy;          // transactions where the coprocessor had nothing to send
    uint32_t rx_bad_checksum;
    uint32_t rx_bad_header;
    uint32_t tx_frames;         // frames sent
    uint32_t spi_errors;        // HAL errors and transfers that timed out
} esp_spi_stats_t;

// Set up the DMA streams and SPI1. rx_cb gets every good received frame.
void esp_spi_init(SPI_HandleTypeDef *hspi, esp_spi_rx_cb_t rx_cb);

// Pulse the coprocessor's EN pin (PA3, open drain) and drop all queued frames.
void esp_spi_reset_cp(void);

// Main loop: hands received frames to the rx callback, starts transfers no
// interrupt will start (boot, idle poll, after the RX queue was full).
void esp_spi_poll(void);

// Queue a frame for the coprocessor. Returns 0, -1 if the queue is full,
// -2 if the payload doesn't fit in one transaction.
int esp_spi_send(uint8_t if_type, uint8_t if_num, uint8_t flags, uint8_t pkt_type,
                 const uint8_t *payload, uint16_t len);
int esp_spi_tx_busy(void);     // 1 when the transmit queue is full
int esp_spi_tx_queued(void);   // frames waiting to be sent
const esp_spi_stats_t* esp_spi_get_stats(void);

#endif /* ESP_SPI_H_ */
