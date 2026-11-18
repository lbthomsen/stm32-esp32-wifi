/*
 * at_spi.h - ESP-AT over SPI, host side transport (no RTOS).
 *
 * ESP-AT's SPI interface is the ESP "slave HD" (half duplex) protocol:
 * every transaction is an 8 bit command, an 8 bit address and 8 dummy bits,
 * followed by the data phase. A handshake line (HS) from the ESP says
 * "ask me": the host then reads the slave status and either reads the data
 * the ESP has for it or writes the data it asked to send.
 *
 * The transport moves a plain byte stream in both directions; the AT layer
 * on top splits it into lines.
 */

#ifndef AT_SPI_H_
#define AT_SPI_H_

#include <stdint.h>
#include <stddef.h>
#include "main.h"

#define AT_SPI_MAX_CHUNK        4092    // ESP-AT's DMA limit per transfer
#define AT_SPI_RX_RING_SIZE     4096
#define AT_SPI_TX_RING_SIZE     4096    // must hold a whole AT_SEND_MAX payload plus the command

typedef struct {
    uint32_t transactions;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t seq_errors;
    uint32_t len_errors;
    uint32_t spi_errors;
    uint32_t rx_overflows;
} at_spi_stats_t;

void at_spi_init(SPI_HandleTypeDef *hspi);

// Pulse the ESP's EN pin (PA3, open drain) and drop all buffered data.
void at_spi_reset_esp(void);

void at_spi_poll(void);

// Queue bytes for the ESP. Returns the number accepted (may be less than len).
size_t at_spi_write(const void *data, size_t len);
size_t at_spi_tx_free(void);

// Take bytes received from the ESP. Returns the number copied.
size_t at_spi_read(void *data, size_t max);

const at_spi_stats_t* at_spi_get_stats(void);

#endif /* AT_SPI_H_ */
