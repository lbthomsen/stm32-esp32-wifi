/*
 * at_spi.c - ESP-AT over SPI, host side transport (no RTOS).
 *
 * Follows esp_at/examples/at_spi_master (the reference SPI AT master):
 *  - SPI mode 0, half duplex. Every transaction: [cmd][addr][dummy][data...].
 *  - HS rising edge: the ESP wants the host to look. The host reads the slave
 *    status (cmd 0x02, addr 0x04, 4 bytes):
 *      direction 1: the ESP has `len` bytes, read them (cmd 0x04) and say
 *                   so (cmd 0x08).
 *      direction 2: the ESP is ready for the bytes the host asked to send,
 *                   write them (cmd 0x03) and say so (cmd 0x07).
 *  - To send, the host first requests a write (cmd 0x01, addr 0x00) with
 *    {0xFE, seq, len16} and waits for the ESP to come back with direction 2.
 *  - Sequence numbers count up per direction; seq 1 after a mismatch means
 *    the ESP restarted.
 */

#include <string.h>
#include "at_spi.h"

#define CMD_WRBUF       0x01    // write the slave's shared buffer (write request)
#define CMD_RDBUF       0x02    // read the slave's shared buffer (status)
#define CMD_WRDMA       0x03    // write data
#define CMD_RDDMA       0x04    // read data
#define CMD_WR_DONE     0x07
#define CMD_RD_DONE     0x08

#define WRBUF_ADDR      0x00
#define RDBUF_ADDR      0x04

#define DIR_NONE        0
#define DIR_READ        1       // ESP -> host
#define DIR_WRITE       2       // host -> ESP

#define WRITE_REQ_MAGIC 0xFE

#define SPI_TIMEOUT_MS  50
#define HS_STUCK_MS     100     // HS high this long without a transaction: treat as an edge
#define BOOT_HOLD_MS    100

static SPI_HandleTypeDef *spi;
static at_spi_stats_t stats;

static uint8_t rx_ring[AT_SPI_RX_RING_SIZE];
static size_t rx_head, rx_tail;     // head: write, tail: read
static uint8_t tx_ring[AT_SPI_TX_RING_SIZE];
static size_t tx_head, tx_tail;

static uint8_t chunk[AT_SPI_MAX_CHUNK] __attribute__((aligned(4)));

static volatile uint8_t hs_edge;
static uint8_t send_seq;            // last write request sequence number
static uint8_t recv_seq;            // last read sequence number
static uint16_t planned_len;        // bytes promised in the outstanding write request
static uint8_t write_requested;
static uint32_t last_xfer_tick;
static uint32_t reset_tick;

static inline size_t ring_used(size_t head, size_t tail, size_t size) {
    return (head + size - tail) % size;
}

static inline int hs_active(void) {
    return HAL_GPIO_ReadPin(SPI1_HS_GPIO_Port, SPI1_HS_Pin) == GPIO_PIN_SET;
}

void HAL_GPIO_EXTI_Callback(uint16_t gpio_pin) {
    if (gpio_pin == SPI1_HS_Pin) {
        hs_edge = 1;
    }
}

static inline void cs_low(void) {
    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_RESET);
}

static inline void cs_high(void) {
    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_SET);
}

// One half duplex transaction. Either tx or rx (or neither) carries data.
static int xfer(uint8_t cmd, uint8_t addr, const uint8_t *tx, uint8_t *rx, uint16_t len) {
    uint8_t hdr[3] = { cmd, addr, 0x00 };   // command, address, 8 dummy bits
    HAL_StatusTypeDef st;

    cs_low();
    st = HAL_SPI_Transmit(spi, hdr, sizeof(hdr), SPI_TIMEOUT_MS);
    if (st == HAL_OK && len) {
        if (tx) {
            st = HAL_SPI_Transmit(spi, (uint8_t*) tx, len, SPI_TIMEOUT_MS);
        } else if (rx) {
            st = HAL_SPI_Receive(spi, rx, len, SPI_TIMEOUT_MS);
        }
    }
    cs_high();

    stats.transactions++;
    last_xfer_tick = HAL_GetTick();
    if (st != HAL_OK) {
        stats.spi_errors++;
        return -1;
    }
    return 0;
}

static void request_write(void) {
    size_t pending = ring_used(tx_head, tx_tail, sizeof(tx_ring));
    if (pending == 0 || write_requested) {
        return;
    }
    planned_len = (uint16_t) (pending > AT_SPI_MAX_CHUNK ? AT_SPI_MAX_CHUNK : pending);
    send_seq++;
    uint8_t req[4] = { WRITE_REQ_MAGIC, send_seq, planned_len & 0xff, planned_len >> 8 };
    if (xfer(CMD_WRBUF, WRBUF_ADDR, req, NULL, sizeof(req)) == 0) {
        write_requested = 1;
    }
}

static void do_read(uint8_t seq, uint16_t len) {
    if (seq != (uint8_t) (recv_seq + 1)) {
        stats.seq_errors++;     // seq 1 after a mismatch: the ESP restarted, carry on
    }
    if (len == 0 || len > AT_SPI_MAX_CHUNK) {
        stats.len_errors++;
        return;
    }
    recv_seq = seq;

    if (xfer(CMD_RDDMA, 0, NULL, chunk, len) != 0) {
        return;
    }
    xfer(CMD_RD_DONE, 0, NULL, NULL, 0);

    for (uint16_t i = 0; i < len; ++i) {
        size_t next = (rx_head + 1) % sizeof(rx_ring);
        if (next == rx_tail) {
            stats.rx_overflows++;
            break;
        }
        rx_ring[rx_head] = chunk[i];
        rx_head = next;
    }
    stats.rx_bytes += len;
}

static void do_write(uint8_t seq) {
    if (!write_requested || planned_len == 0) {
        return;
    }
    if (seq != send_seq) {
        stats.seq_errors++;
        if (seq != 1) {
            return;
        }
        send_seq = seq;         // the ESP restarted and took our request as its first
    }

    // Copy the planned bytes out of the ring (it may wrap)
    for (uint16_t i = 0; i < planned_len; ++i) {
        chunk[i] = tx_ring[tx_tail];
        tx_tail = (tx_tail + 1) % sizeof(tx_ring);
    }
    if (xfer(CMD_WRDMA, 0, chunk, NULL, planned_len) == 0) {
        xfer(CMD_WR_DONE, 0, NULL, NULL, 0);
        stats.tx_bytes += planned_len;
    }
    write_requested = 0;
    planned_len = 0;

    // More queued meanwhile: ask again
    request_write();
}

void at_spi_init(SPI_HandleTypeDef *hspi) {
    spi = hspi;
    memset(&stats, 0, sizeof(stats));
    cs_high();
    // Enable SPI1 now so SCK sits at its idle level before CS is ever asserted.
    __HAL_SPI_ENABLE(spi);
    at_spi_reset_esp();
}

void at_spi_reset_esp(void) {
    cs_high();
    HAL_GPIO_WritePin(SPI1_RST_GPIO_Port, SPI1_RST_Pin, GPIO_PIN_RESET);
    HAL_Delay(50);
    HAL_GPIO_WritePin(SPI1_RST_GPIO_Port, SPI1_RST_Pin, GPIO_PIN_SET);

    rx_head = rx_tail = 0;
    tx_head = tx_tail = 0;
    send_seq = 0;
    recv_seq = 0;
    planned_len = 0;
    write_requested = 0;
    hs_edge = 0;
    reset_tick = HAL_GetTick();
    last_xfer_tick = reset_tick;
}

void at_spi_poll(void) {
    uint32_t now = HAL_GetTick();
    if (now - reset_tick < BOOT_HOLD_MS) {
        return;
    }

    // Missed edge safety net: HS has been high for a while with nothing done.
    if (!hs_edge && hs_active() && now - last_xfer_tick > HS_STUCK_MS) {
        hs_edge = 1;
    }

    if (hs_edge) {
        hs_edge = 0;
        uint8_t status[4];
        if (xfer(CMD_RDBUF, RDBUF_ADDR, NULL, status, sizeof(status)) == 0) {
            uint8_t direction = status[0];
            uint8_t seq = status[1];
            uint16_t len = (uint16_t) (status[2] | (status[3] << 8));
            if (direction == DIR_READ) {
                do_read(seq, len);
            } else if (direction == DIR_WRITE) {
                do_write(seq);
            }
        }
    }

    request_write();
}

size_t at_spi_write(const void *data, size_t len) {
    const uint8_t *p = data;
    size_t n = 0;
    while (n < len) {
        size_t next = (tx_head + 1) % sizeof(tx_ring);
        if (next == tx_tail) {
            break;
        }
        tx_ring[tx_head] = p[n++];
        tx_head = next;
    }
    return n;
}

size_t at_spi_tx_free(void) {
    return sizeof(tx_ring) - 1 - ring_used(tx_head, tx_tail, sizeof(tx_ring));
}

size_t at_spi_read(void *data, size_t max) {
    uint8_t *p = data;
    size_t n = 0;
    while (n < max && rx_tail != rx_head) {
        p[n++] = rx_ring[rx_tail];
        rx_tail = (rx_tail + 1) % sizeof(rx_ring);
    }
    return n;
}

const at_spi_stats_t* at_spi_get_stats(void) {
    return &stats;
}
