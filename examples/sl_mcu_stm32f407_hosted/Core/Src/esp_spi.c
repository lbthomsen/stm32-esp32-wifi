/*
 * esp_spi.c - ESP-Hosted SPI full duplex transport, host side, interrupt
 * driven (no RTOS).
 *
 * Protocol (matches the esp_hosted coprocessor, SPI full duplex):
 *  - SPI mode 3, fixed size transactions of ESP_SPI_BUF_SIZE bytes.
 *  - Handshake (HS) high: the coprocessor has a transaction queued and is
 *    ready to be clocked. It drops HS when CS is asserted.
 *  - Data ready (DR) high: the coprocessor has a frame for the host.
 *  - Every transaction is full duplex. The host sends its pending frame (or
 *    zeros) and receives whatever the coprocessor had queued (or a dummy).
 *  - Frames start with a 12 byte v1 header, little endian, with a 16 bit
 *    byte-sum checksum computed with the checksum field zeroed.
 *
 * Who does what:
 *  - Interrupts run the wire. A rising edge on HS or DR (EXTI) starts a
 *    transaction if there is something to move; the SPI DMA completion
 *    interrupt finishes it (CS up, frame accounting) and starts the next
 *    one if the coprocessor is already ready again.
 *  - The DMA receives straight into a slot of a small RX queue. The main
 *    loop (esp_spi_poll) checks each received frame and hands it to the
 *    layers above, so lwIP and the RPC code never run in interrupt context.
 *  - Flow control: no transaction starts while every RX slot is full,
 *    because every transaction receives a frame. The main loop restarts the
 *    traffic once it has emptied a slot.
 *  - The coprocessor raises HS a little before its SPI slave is armed
 *    (more than 50 us, less than 200 us on the C3). Clocking too early
 *    gives corrupt frames, so the HS edge doesn't start the transaction
 *    itself: it starts a one-shot timer (TIM7), and the timer interrupt
 *    starts the transaction HS_SETTLE_US after the edge. No busy waiting.
 *  - esp_spi_poll() also covers what an edge can't: the boot hold-off, the
 *    100 ms idle poll the upstream host does, and a stuck transfer.
 *
 * TIM7 is driven through its registers (the HAL TIM module isn't enabled in
 * this project): a basic timer, 1 us ticks, one-pulse mode.
 *
 * The DMA streams (DMA2 stream 0 = SPI1_RX, stream 3 = SPI1_TX, channel 3)
 * are set up here rather than in CubeMX, so regenerating the project can't
 * undo them. Don't also enable SPI1 DMA in CubeMX.
 */

#include <string.h>
#include "esp_spi.h"

/* --- Wire format ------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    uint8_t if_type :4;     // ESP_IF_* (STA, SERIAL for RPC, PRIV for init, ...)
    uint8_t if_num :4;
    uint8_t flags;
    uint16_t len;           // payload length
    uint16_t offset;        // payload offset from the start of the frame (12)
    uint16_t checksum;      // byte sum of header + payload, this field zeroed
    uint16_t seq_num;
    uint8_t throttle_cmd :2;
    uint8_t rx_credit :6;
    uint8_t pkt_type;
} esp_hdr_v1_t;

_Static_assert(sizeof(esp_hdr_v1_t) == ESP_SPI_HDR_SIZE, "v1 header must be 12 bytes");

#define CHECKSUM_OFFSET 6   // byte offset of esp_hdr_v1_t.checksum

/* --- Timing ------------------------------------------------------------ */

#define IDLE_POLL_MS        100     // clock an idle link this often while HS is up (as upstream)
#define BOOT_MS             1000    // HS floats while the coprocessor boots: ignore it this long
#define HS_SETTLE_US        300     // HS rises before the slave is armed: measured >50, <200 us
#define HS_DROP_WAIT_US     2000    // after CS low, wait (at most) this long for HS to drop (~15 us)
#define XFER_TIMEOUT_MS     20      // 1600 bytes at 10.5 MHz take ~1.3 ms

#define SPI_IRQ_PRIORITY    0       // same as the EXTIs (CubeMX), so they never nest

/* --- State ------------------------------------------------------------- */

static SPI_HandleTypeDef *spi;
static DMA_HandleTypeDef dma_rx;
static DMA_HandleTypeDef dma_tx;
static esp_spi_rx_cb_t rx_callback;
static esp_spi_stats_t stats;

// All zeros: what the host clocks out when it has nothing to send (the
// coprocessor sees if_type 0 and ignores it).
static uint8_t idle_buf[ESP_SPI_BUF_SIZE] __attribute__((aligned(4)));

// Transmit queue: complete transaction buffers. The main loop fills slots
// and advances tx_wr; the completion interrupt advances tx_rd. Free running
// 8 bit counters: the number queued is (uint8_t) (tx_wr - tx_rd).
static uint8_t tx_ring[ESP_SPI_TX_SLOTS][ESP_SPI_BUF_SIZE] __attribute__((aligned(4)));
static volatile uint8_t tx_wr;
static volatile uint8_t tx_rd;
static uint16_t tx_seq_num;

// Receive queue: the DMA writes into slot rx_wr; the completion interrupt
// advances rx_wr for real frames, the main loop advances rx_rd.
static uint8_t rx_ring[ESP_SPI_RX_SLOTS][ESP_SPI_BUF_SIZE] __attribute__((aligned(4)));
static volatile uint8_t rx_wr;
static volatile uint8_t rx_rd;

static volatile uint8_t xfer_active;    // a DMA transaction is running
static volatile uint8_t xfer_sending;   // ...and it carries tx_ring[tx_rd]
static volatile uint32_t xfer_tick;
static volatile uint32_t last_xfer_tick;
static volatile uint8_t dr_edge;        // DR rose since the last transaction
static volatile uint32_t hs_rise_cycles; // DWT time of the last HS rising edge
static volatile uint8_t cp_ready;       // past the boot hold-off
static volatile uint8_t sync_pending;   // next transaction is a throwaway (see start_xfer)
static uint32_t reset_tick;

/* --- Helpers ----------------------------------------------------------- */

// The v1 header checksum: 16 bit sum of all bytes, skipping the checksum field.
static uint16_t frame_checksum(const uint8_t *buf, uint16_t len) {
    uint16_t sum = 0;
    for (uint16_t i = 0; i < len; ++i) {
        if (i == CHECKSUM_OFFSET || i == CHECKSUM_OFFSET + 1) {
            continue;
        }
        sum += buf[i];
    }
    return sum;
}

static inline int hs_active(void) {
    return HAL_GPIO_ReadPin(SPI1_HS_GPIO_Port, SPI1_HS_Pin) == GPIO_PIN_SET;
}

static inline int dr_active(void) {
    return HAL_GPIO_ReadPin(SPI1_DR_GPIO_Port, SPI1_DR_Pin) == GPIO_PIN_SET;
}

static inline void cs_low(void) {
    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_RESET);
}

static inline void cs_high(void) {
    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_SET);
}

static inline uint32_t us_to_cycles(uint32_t us) {
    return us * (SystemCoreClock / 1000000);
}

static inline uint8_t tx_queued(void) {
    return (uint8_t) (tx_wr - tx_rd);
}

static inline uint8_t rx_queued(void) {
    return (uint8_t) (rx_wr - rx_rd);
}

/* --- Settle timer (TIM7, one-pulse) -------------------------------------- */

static void settle_timer_init(void) {
    __HAL_RCC_TIM7_CLK_ENABLE();
    // APB1 timers run at twice PCLK1 when the APB1 prescaler isn't 1 (84 MHz here)
    uint32_t tim_clk = HAL_RCC_GetPCLK1Freq() * 2;
    TIM7->CR1 = TIM_CR1_OPM | TIM_CR1_URS;  // stop after one period; only overflow raises UIF
    TIM7->PSC = tim_clk / 1000000 - 1;      // 1 us per tick
    TIM7->ARR = 0xffff;
    TIM7->EGR = TIM_EGR_UG;                 // load PSC now (URS: no interrupt for this)
    TIM7->SR = 0;
    TIM7->DIER = TIM_DIER_UIE;
    HAL_NVIC_SetPriority(TIM7_IRQn, SPI_IRQ_PRIORITY, 0);
    HAL_NVIC_EnableIRQ(TIM7_IRQn);
}

// Fire TIM7_IRQHandler in about `us` microseconds (unless already armed).
static void settle_timer_start(uint32_t us) {
    if (TIM7->CR1 & TIM_CR1_CEN) {
        return;
    }
    TIM7->ARR = us ? us : 1;
    TIM7->CNT = 0;
    TIM7->CR1 |= TIM_CR1_CEN;
}

static void start_xfer(void);

void TIM7_IRQHandler(void) {
    TIM7->SR = 0;
    start_xfer();
}

/* --- Interrupt side ------------------------------------------------------ */

// Start a transaction if the coprocessor is ready and there's a reason to.
// Runs in interrupt context, or from the main loop with interrupts off.
static void start_xfer(void) {
    if (xfer_active || !cp_ready || !hs_active()) {
        return;
    }
    // Every transaction receives a frame; don't start one with nowhere to put it.
    if (rx_queued() >= ESP_SPI_RX_SLOTS) {
        return;
    }
    // Clock when there's something to move in either direction, or every
    // IDLE_POLL_MS while HS is up (same as the upstream host).
    uint32_t now = HAL_GetTick();
    if (!tx_queued() && !dr_active() && !dr_edge && !sync_pending && now - last_xfer_tick < IDLE_POLL_MS) {
        return;
    }

    // The coprocessor raises HS from its post-setup callback, before its
    // SPI slave is armed. If the edge was less than HS_SETTLE_US ago, come
    // back from the TIM7 interrupt when it's time.
    // An HS edge whose interrupt hasn't run yet (we're in another handler,
    // or the main loop has interrupts off) is a fresh edge too.
    if (__HAL_GPIO_EXTI_GET_IT(SPI1_HS_Pin)) {
        settle_timer_start(HS_SETTLE_US);
        return;
    }
    uint32_t since_us = (DWT->CYCCNT - hs_rise_cycles) / (SystemCoreClock / 1000000);
    if (since_us < HS_SETTLE_US) {
        settle_timer_start(HS_SETTLE_US - since_us);
        return;
    }

    // The first transaction after a host start or coprocessor reset can be
    // off by one bit: the coprocessor counts a stray SCK edge from the
    // host's pin setup into it. Make that one a throwaway zero frame; CS
    // realigns the coprocessor for every transaction after it.
    const uint8_t *tx = idle_buf;
    uint8_t sending = 0;
    if (sync_pending) {
        sync_pending = 0;
    } else if (tx_queued()) {
        tx = tx_ring[tx_rd % ESP_SPI_TX_SLOTS];
        sending = 1;
    }

    dr_edge = 0;
    last_xfer_tick = now;
    xfer_tick = now;
    xfer_sending = sending;
    xfer_active = 1;

    cs_low();

    // The coprocessor drops HS once it has seen CS. Give it that moment
    // before clocking.
    uint32_t t0 = DWT->CYCCNT;
    while (hs_active() && DWT->CYCCNT - t0 < us_to_cycles(HS_DROP_WAIT_US)) {
    }

    if (HAL_SPI_TransmitReceive_DMA(spi, (uint8_t*) tx, rx_ring[rx_wr % ESP_SPI_RX_SLOTS], ESP_SPI_BUF_SIZE) != HAL_OK) {
        cs_high();
        ++stats.spi_errors;
        xfer_active = 0;
    }
}

// start_xfer() from the main loop: with interrupts off, so it can't race
// the interrupt handlers that also call it.
static void start_xfer_from_main(void) {
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    start_xfer();
    __set_PRIMASK(primask);
}

// A dummy frame (the coprocessor had nothing to send) needs no further
// work. Anything else goes to the RX queue for the main loop to check.
static int is_dummy(const uint8_t *buf) {
    const esp_hdr_v1_t *hdr = (const esp_hdr_v1_t*) buf;
    return hdr->if_type == ESP_IF_DUMMY || hdr->if_type == ESP_IF_INVALID || (hdr->len == 0 && hdr->flags == 0);
}

// HAL (DMA interrupt): the transaction finished. CS up, account for the
// frame sent and the frame received, start the next one if possible.
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi) {
    if (hspi != spi) {
        return;
    }
    cs_high();
    ++stats.transactions;

    if (xfer_sending) {
        ++tx_rd;
        ++stats.tx_frames;
    }
    if (is_dummy(rx_ring[rx_wr % ESP_SPI_RX_SLOTS])) {
        ++stats.rx_dummy;
    } else {
        ++rx_wr;
    }
    xfer_active = 0;

    // Usually HS is low here and its next rising edge starts the next
    // transaction; this catches the case where it's already back up.
    start_xfer();
}

// HAL: the transfer failed (DMA or SPI error). Count it; the frame queued
// for sending stays queued and goes out with the next transaction.
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi) {
    if (hspi != spi) {
        return;
    }
    cs_high();
    ++stats.spi_errors;
    xfer_active = 0;
}

// HAL (EXTI interrupt): HS or DR rose.
void HAL_GPIO_EXTI_Callback(uint16_t gpio_pin) {
    if (gpio_pin == SPI1_HS_Pin) {
        hs_rise_cycles = DWT->CYCCNT;
        start_xfer();
    } else if (gpio_pin == SPI1_DR_Pin) {
        dr_edge = 1;
        start_xfer();
    }
}

// The DMA stream interrupts (vector names from the startup file)
void DMA2_Stream0_IRQHandler(void) {
    HAL_DMA_IRQHandler(&dma_rx);
}

void DMA2_Stream3_IRQHandler(void) {
    HAL_DMA_IRQHandler(&dma_tx);
}

/* --- Set-up ------------------------------------------------------------ */

// DMA for SPI1: both directions byte wide, memory incrementing, normal
// (not circular) mode, no FIFO.
static void dma_init(void) {
    __HAL_RCC_DMA2_CLK_ENABLE();

    dma_rx.Instance = DMA2_Stream0;
    dma_rx.Init.Channel = DMA_CHANNEL_3;
    dma_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
    dma_rx.Init.PeriphInc = DMA_PINC_DISABLE;
    dma_rx.Init.MemInc = DMA_MINC_ENABLE;
    dma_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    dma_rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    dma_rx.Init.Mode = DMA_NORMAL;
    dma_rx.Init.Priority = DMA_PRIORITY_HIGH;   // RX must never fall behind (overrun)
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

// Set up the cycle counter, DMA, the settle timer and SPI1.
void esp_spi_init(SPI_HandleTypeDef *hspi, esp_spi_rx_cb_t rx_cb) {
    spi = hspi;
    rx_callback = rx_cb;
    memset(&stats, 0, sizeof(stats));
    tx_wr = tx_rd = 0;
    rx_wr = rx_rd = 0;
    sync_pending = 1;
    cs_high();

    // The DWT cycle counter times the short waits around CS.
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    dma_init();
    settle_timer_init();

    // Enable SPI1 now so SCK sits at its idle level (high, CPOL=1) before
    // CS is ever asserted. HAL would otherwise only enable it inside the
    // first transfer, with CS already low, and the coprocessor would see a
    // spurious clock edge that shifts the whole first frame by one bit.
    __HAL_SPI_ENABLE(spi);
}

// Pulse EN for 50 ms and start from a clean slate: no transfer, empty
// queues, the throwaway sync frame next. Transfers wait BOOT_MS after this.
void esp_spi_reset_cp(void) {
    // Stop the interrupt side, then hold the coprocessor in reset.
    HAL_NVIC_DisableIRQ(EXTI4_IRQn);
    HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    if (xfer_active) {
        HAL_SPI_Abort(spi);
        xfer_active = 0;
    }
    cs_high();
    __set_PRIMASK(primask);

    HAL_GPIO_WritePin(SPI1_RST_GPIO_Port, SPI1_RST_Pin, GPIO_PIN_RESET);
    HAL_Delay(50);
    HAL_GPIO_WritePin(SPI1_RST_GPIO_Port, SPI1_RST_Pin, GPIO_PIN_SET);

    tx_wr = tx_rd = 0;
    rx_wr = rx_rd = 0;
    cp_ready = 0;
    sync_pending = 1;
    dr_edge = 0;
    reset_tick = HAL_GetTick();

    __HAL_GPIO_EXTI_CLEAR_IT(SPI1_HS_Pin | SPI1_DR_Pin);
    HAL_NVIC_EnableIRQ(EXTI4_IRQn);
    HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
}

/* --- Main loop side ------------------------------------------------------ */

int esp_spi_tx_busy(void) {
    return tx_queued() >= ESP_SPI_TX_SLOTS;
}

int esp_spi_tx_queued(void) {
    return tx_queued();
}

int esp_spi_send(uint8_t if_type, uint8_t if_num, uint8_t flags, uint8_t pkt_type,
                 const uint8_t *payload, uint16_t len) {
    if (tx_queued() >= ESP_SPI_TX_SLOTS) {
        return -1;
    }
    if (len > ESP_SPI_BUF_SIZE - ESP_SPI_HDR_SIZE) {
        return -2;
    }

    // Build the whole frame in the free slot, then publish it by advancing
    // tx_wr; the interrupt side only looks at slots below tx_wr.
    uint8_t *tx_buf = tx_ring[tx_wr % ESP_SPI_TX_SLOTS];
    memset(tx_buf, 0, ESP_SPI_HDR_SIZE);
    esp_hdr_v1_t *hdr = (esp_hdr_v1_t*) tx_buf;
    hdr->if_type = if_type & 0x0f;
    hdr->if_num = if_num & 0x0f;
    hdr->flags = flags;
    hdr->len = len;
    hdr->offset = ESP_SPI_HDR_SIZE;
    hdr->seq_num = tx_seq_num++;
    hdr->pkt_type = pkt_type;
    if (len) {
        memcpy(tx_buf + ESP_SPI_HDR_SIZE, payload, len);
    }
    hdr->checksum = frame_checksum(tx_buf, ESP_SPI_HDR_SIZE + len);

    __DMB();
    ++tx_wr;

    start_xfer_from_main();
    return 0;
}

// Check one received frame and pass it up.
static void decode_rx(const uint8_t *buf) {
    const esp_hdr_v1_t *hdr = (const esp_hdr_v1_t*) buf;

    uint32_t total = (uint32_t) hdr->offset + hdr->len;
    if (hdr->offset < ESP_SPI_HDR_SIZE || hdr->offset > ESP_SPI_HDR_SIZE + 7 || total > ESP_SPI_BUF_SIZE) {
        ++stats.rx_bad_header;
        return;
    }

    if (frame_checksum(buf, (uint16_t) total) != hdr->checksum) {
        ++stats.rx_bad_checksum;
        return;
    }

    ++stats.rx_frames;

    if (rx_callback) {
        esp_frame_t frame = {
                .if_type = hdr->if_type,
                .if_num = hdr->if_num,
                .flags = hdr->flags,
                .pkt_type = hdr->pkt_type,
                .seq_num = hdr->seq_num,
                .len = hdr->len,
                .payload = buf + hdr->offset
        };
        rx_callback(&frame);
    }
}

// Main loop: boot hold-off, stuck transfers, received frames to the layers
// above, and transfers no interrupt will start.
void esp_spi_poll(void) {
    uint32_t now = HAL_GetTick();

    // After a reset HS floats high while the coprocessor boots, so it can't
    // be trusted until the firmware is up. Hold off for BOOT_MS.
    if (!cp_ready) {
        if (now - reset_tick < BOOT_MS) {
            return;
        }
        cp_ready = 1;
    }

    // A transfer that never completed: abort it and carry on.
    if (xfer_active && now - xfer_tick > XFER_TIMEOUT_MS) {
        uint32_t primask = __get_PRIMASK();
        __disable_irq();
        if (xfer_active) {
            HAL_SPI_Abort(spi);
            cs_high();
            ++stats.spi_errors;
            xfer_active = 0;
        }
        __set_PRIMASK(primask);
    }

    // Hand received frames to the layers above (in main loop context).
    while (rx_queued()) {
        decode_rx(rx_ring[rx_rd % ESP_SPI_RX_SLOTS]);
        __DMB();
        ++rx_rd;
    }

    // Start what no edge will: the first transaction after boot, the idle
    // poll, traffic held back while the RX queue was full, or TX data
    // queued while HS was already up.
    if (!xfer_active && hs_active()) {
        start_xfer_from_main();
    }
}

// Counters for the status line and page.
const esp_spi_stats_t* esp_spi_get_stats(void) {
    return &stats;
}
