/*
 * at_spi.c - ESP-AT over SPI, host side transport, interrupt driven (no RTOS).
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
 *
 * Everything on the wire runs from interrupts: the HS EXTI starts a status
 * read, and each SPI DMA completion starts the next transaction of the
 * sequence. Each transaction is one full duplex DMA transfer of the 3 header
 * bytes plus the data (the bytes clocked in during the header are ignored).
 * The main loop only moves bytes in and out of the two rings, and
 * at_spi_poll() is a safety net (missed edges, stuck transfers).
 *
 * The DMA streams are set up here rather than in CubeMX, so regenerating
 * the project can't undo them. Don't also enable SPI1 DMA in CubeMX.
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

#define DIR_READ        1       // ESP -> host
#define DIR_WRITE       2       // host -> ESP

#define WRITE_REQ_MAGIC 0xFE

#define HDR_LEN         3
#define XFER_TIMEOUT_MS 20      // a DMA transfer of 4 KB at 10.5 MHz takes ~3 ms
#define HS_STUCK_MS     100     // HS high this long without a transaction: treat as an edge
#define BOOT_HOLD_MS    100

#define SPI_IRQ_PRIORITY 0      // same as the HS EXTI (CubeMX), so they never nest

typedef enum {
    XS_IDLE = 0,
    XS_RD_STATUS,
    XS_RD_DATA,
    XS_RD_DONE,
    XS_WR_REQ,
    XS_WR_DATA,
    XS_WR_DONE
} xfer_state_t;

static SPI_HandleTypeDef *spi;
static DMA_HandleTypeDef dma_rx;
static DMA_HandleTypeDef dma_tx;
static at_spi_stats_t stats;

// Rings: one side is the main loop, the other the interrupts.
static uint8_t rx_ring[AT_SPI_RX_RING_SIZE];
static volatile size_t rx_head, rx_tail;    // head: written by IRQ, tail: by main
static uint8_t tx_ring[AT_SPI_TX_RING_SIZE];
static volatile size_t tx_head, tx_tail;    // head: written by main, tail: by IRQ

// DMA buffers: header (3 bytes) + data for one transaction. In SRAM, not
// CCM: the DMA can't reach CCM.
static uint8_t dma_tx_buf[HDR_LEN + AT_SPI_MAX_CHUNK] __attribute__((aligned(4)));
static uint8_t dma_rx_buf[HDR_LEN + AT_SPI_MAX_CHUNK] __attribute__((aligned(4)));

static volatile xfer_state_t xstate;    // which transaction is on the wire
static volatile uint8_t hs_edge;        // HS rose: read the status
static volatile uint32_t xfer_tick;     // start of the transfer in progress
static volatile uint32_t last_xfer_tick;
static uint8_t send_seq;                // last write request sequence number
static uint8_t recv_seq;                // last read sequence number
static uint16_t planned_len;            // bytes promised in the outstanding write request
static uint16_t read_len;               // bytes the ESP announced
static uint8_t write_requested;
static uint32_t reset_tick;

// Bytes in a ring buffer.
static inline size_t ring_used(size_t head, size_t tail, size_t size) {
    return (head + size - tail) % size;
}

static inline int hs_active(void) {
    return HAL_GPIO_ReadPin(SPI1_HS_GPIO_Port, SPI1_HS_Pin) == GPIO_PIN_SET;
}

static inline void cs_low(void) {
    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_RESET);
}

static inline void cs_high(void) {
    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_SET);
}

// Start one transaction. The data phase (if any) must already be in
// dma_tx_buf + HDR_LEN. Called from interrupt context or with interrupts off.
static void start(xfer_state_t s, uint8_t cmd, uint8_t addr, uint16_t data_len) {
    dma_tx_buf[0] = cmd;
    dma_tx_buf[1] = addr;
    dma_tx_buf[2] = 0x00;       // dummy
    xstate = s;
    xfer_tick = HAL_GetTick();
    stats.transactions++;
    cs_low();
    if (HAL_SPI_TransmitReceive_DMA(spi, dma_tx_buf, dma_rx_buf, HDR_LEN + data_len) != HAL_OK) {
        cs_high();
        stats.spi_errors++;
        xstate = XS_IDLE;
    }
}

// Bytes queued for the ESP.
static int tx_pending(void) {
    return ring_used(tx_head, tx_tail, sizeof(tx_ring)) != 0;
}

// Start whatever needs doing next, if the bus is free.
// Called from interrupt context or with interrupts off.
static void kick(void) {
    if (xstate != XS_IDLE || HAL_GetTick() - reset_tick < BOOT_HOLD_MS) {
        return;
    }
    if (hs_edge) {
        hs_edge = 0;
        memset(dma_tx_buf + HDR_LEN, 0, 4);
        start(XS_RD_STATUS, CMD_RDBUF, RDBUF_ADDR, 4);
        return;
    }
    if (tx_pending() && !write_requested) {
        size_t pending = ring_used(tx_head, tx_tail, sizeof(tx_ring));
        planned_len = (uint16_t) (pending > AT_SPI_MAX_CHUNK ? AT_SPI_MAX_CHUNK : pending);
        send_seq++;
        dma_tx_buf[HDR_LEN + 0] = WRITE_REQ_MAGIC;
        dma_tx_buf[HDR_LEN + 1] = send_seq;
        dma_tx_buf[HDR_LEN + 2] = planned_len & 0xff;
        dma_tx_buf[HDR_LEN + 3] = planned_len >> 8;
        start(XS_WR_REQ, CMD_WRBUF, WRBUF_ADDR, 4);
    }
}

// kick() from the main loop: with interrupts off, so it can't race the
// interrupt handlers that also call it.
static void kick_from_main(void) {
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    kick();
    __set_PRIMASK(primask);
}

// The status read finished: read the data the ESP has, or write ours.
static void on_status(void) {
    const uint8_t *st = dma_rx_buf + HDR_LEN;
    uint8_t direction = st[0];
    uint8_t seq = st[1];
    uint16_t len = (uint16_t) (st[2] | (st[3] << 8));

    if (direction == DIR_READ) {
        if (seq == recv_seq && seq != 1) {
            // The status of the packet we already read (an extra HS edge got
            // us here before the ESP moved on). Reading again gives junk.
            stats.stale_status++;
            return;
        }
        if (seq != (uint8_t) (recv_seq + 1)) {
            stats.seq_errors++;     // seq 1 after a mismatch: the ESP restarted, carry on
        }
        if (len == 0 || len > AT_SPI_MAX_CHUNK) {
            stats.len_errors++;
            return;
        }
        recv_seq = seq;
        read_len = len;
        memset(dma_tx_buf + HDR_LEN, 0, len);
        start(XS_RD_DATA, CMD_RDDMA, 0, len);
    } else if (direction == DIR_WRITE) {
        if (!write_requested || planned_len == 0) {
            return;
        }
        if (seq != send_seq) {
            stats.seq_errors++;
            if (seq != 1) {
                return;
            }
            send_seq = seq;     // the ESP restarted and took our request as its first
        }
        size_t tail = tx_tail;
        for (uint16_t i = 0; i < planned_len; ++i) {
            dma_tx_buf[HDR_LEN + i] = tx_ring[tail];
            tail = (tail + 1) % sizeof(tx_ring);
        }
        tx_tail = tail;
        start(XS_WR_DATA, CMD_WRDMA, 0, planned_len);
    }
}

// The data read finished: copy it into the RX ring, then say "read done".
static void on_read_data(void) {
    size_t head = rx_head;
    for (uint16_t i = 0; i < read_len; ++i) {
        size_t next = (head + 1) % sizeof(rx_ring);
        if (next == rx_tail) {
            stats.rx_overflows++;
            break;
        }
        rx_ring[head] = dma_rx_buf[HDR_LEN + i];
        head = next;
    }
    rx_head = head;
    stats.rx_bytes += read_len;
    start(XS_RD_DONE, CMD_RD_DONE, 0, 0);
}

// HAL (DMA interrupt): a transaction finished. Release CS, take the next
// step of the sequence, or start whatever else is waiting.
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi) {
    if (hspi != spi) {
        return;
    }
    cs_high();
    last_xfer_tick = HAL_GetTick();

    xfer_state_t done = xstate;
    xstate = XS_IDLE;

    switch (done) {
    case XS_RD_STATUS:
        on_status();
        break;
    case XS_RD_DATA:
        on_read_data();
        break;
    case XS_WR_REQ:
        write_requested = 1;
        break;
    case XS_WR_DATA:
        stats.tx_bytes += planned_len;
        start(XS_WR_DONE, CMD_WR_DONE, 0, 0);
        break;
    case XS_WR_DONE:
        write_requested = 0;
        planned_len = 0;
        break;
    default:
        break;
    }
    kick();
}

// HAL: the transfer failed (DMA or SPI error). Count it and carry on.
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi) {
    if (hspi != spi) {
        return;
    }
    cs_high();
    stats.spi_errors++;
    xstate = XS_IDLE;
    kick();
}

// HAL (EXTI interrupt): HS rose, the ESP wants attention.
void HAL_GPIO_EXTI_Callback(uint16_t gpio_pin) {
    if (gpio_pin == SPI1_HS_Pin) {
        hs_edge = 1;
        kick();
    }
}

// The DMA stream interrupts (vector names from the startup file)
void DMA2_Stream0_IRQHandler(void) {
    HAL_DMA_IRQHandler(&dma_rx);
}

void DMA2_Stream3_IRQHandler(void) {
    HAL_DMA_IRQHandler(&dma_tx);
}

// DMA for SPI1: both directions byte wide, memory incrementing, normal
// (not circular) mode, no FIFO.
static void dma_init(void) {
    __HAL_RCC_DMA2_CLK_ENABLE();

    // SPI1_RX: DMA2 stream 0 channel 3, SPI1_TX: DMA2 stream 3 channel 3
    dma_rx.Instance = DMA2_Stream0;
    dma_rx.Init.Channel = DMA_CHANNEL_3;
    dma_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
    dma_rx.Init.PeriphInc = DMA_PINC_DISABLE;
    dma_rx.Init.MemInc = DMA_MINC_ENABLE;
    dma_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    dma_rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    dma_rx.Init.Mode = DMA_NORMAL;
    dma_rx.Init.Priority = DMA_PRIORITY_HIGH;
    dma_rx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    HAL_DMA_Init(&dma_rx);
    __HAL_LINKDMA(spi, hdmarx, dma_rx);

    dma_tx.Instance = DMA2_Stream3;
    dma_tx.Init = dma_rx.Init;
    dma_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
    dma_tx.Init.Priority = DMA_PRIORITY_MEDIUM;
    HAL_DMA_Init(&dma_tx);
    __HAL_LINKDMA(spi, hdmatx, dma_tx);

    HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, SPI_IRQ_PRIORITY, 0);
    HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
    HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, SPI_IRQ_PRIORITY, 0);
    HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);
}

// Set up DMA and SPI1, then reset the ESP.
void at_spi_init(SPI_HandleTypeDef *hspi) {
    spi = hspi;
    memset(&stats, 0, sizeof(stats));
    cs_high();
    dma_init();
    // Enable SPI1 now so SCK sits at its idle level before CS is ever asserted.
    __HAL_SPI_ENABLE(spi);
    at_spi_reset_esp();
}

// Pulse EN for 50 ms and start from a clean slate: no transfer, empty
// rings, sequence numbers back to 0. Transfers wait BOOT_HOLD_MS after this.
void at_spi_reset_esp(void) {
    // Stop the interrupt side first, then hold the ESP in reset.
    HAL_NVIC_DisableIRQ(EXTI4_IRQn);
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    if (xstate != XS_IDLE) {
        HAL_SPI_Abort(spi);
        xstate = XS_IDLE;
    }
    cs_high();
    __set_PRIMASK(primask);

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
    __HAL_GPIO_EXTI_CLEAR_IT(SPI1_HS_Pin);
    HAL_NVIC_EnableIRQ(EXTI4_IRQn);
}

// Main loop safety net: abort a stuck transfer, catch a missed HS edge, and
// start work that no interrupt will (boot hold-off over, new TX data).
void at_spi_poll(void) {
    uint32_t now = HAL_GetTick();

    // A transfer that never completed: abort it and carry on.
    if (xstate != XS_IDLE && now - xfer_tick > XFER_TIMEOUT_MS) {
        uint32_t primask = __get_PRIMASK();
        __disable_irq();
        if (xstate != XS_IDLE) {
            HAL_SPI_Abort(spi);
            cs_high();
            stats.spi_errors++;
            xstate = XS_IDLE;
        }
        __set_PRIMASK(primask);
    }

    // Missed edge safety net: HS has been high for a while with nothing done.
    if (xstate == XS_IDLE && !hs_edge && hs_active() && now - last_xfer_tick > HS_STUCK_MS) {
        hs_edge = 1;
    }

    // Work that couldn't start from an interrupt (boot hold, new TX data, the above)
    if (xstate == XS_IDLE && (hs_edge || (tx_pending() && !write_requested))) {
        kick_from_main();
    }
}

// Queue bytes for the ESP (main loop). Returns how many fitted.
size_t at_spi_write(const void *data, size_t len) {
    const uint8_t *p = data;
    size_t n = 0;
    size_t head = tx_head;
    while (n < len) {
        size_t next = (head + 1) % sizeof(tx_ring);
        if (next == tx_tail) {
            break;
        }
        tx_ring[head] = p[n++];
        head = next;
    }
    tx_head = head;
    if (n) {
        kick_from_main();
    }
    return n;
}

// Room left in the TX ring.
size_t at_spi_tx_free(void) {
    return sizeof(tx_ring) - 1 - ring_used(tx_head, tx_tail, sizeof(tx_ring));
}

// Take received bytes (main loop). Returns how many were copied.
size_t at_spi_read(void *data, size_t max) {
    uint8_t *p = data;
    size_t n = 0;
    size_t tail = rx_tail;
    while (n < max && tail != rx_head) {
        p[n++] = rx_ring[tail];
        tail = (tail + 1) % sizeof(rx_ring);
    }
    rx_tail = tail;
    return n;
}

// Counters for the status line and page.
const at_spi_stats_t* at_spi_get_stats(void) {
    return &stats;
}
