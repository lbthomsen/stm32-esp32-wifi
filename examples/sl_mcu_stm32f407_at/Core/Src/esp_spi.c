/*
 * esp_spi.c - ESP-Hosted SPI full duplex transport, host side (no RTOS).
 *
 * Protocol (matches esp_hosted coprocessor, SPI full duplex):
 *  - SPI mode 3, fixed size transactions of ESP_SPI_BUF_SIZE bytes.
 *  - Handshake (HS) high: the coprocessor has a transaction queued and is
 *    ready to be clocked. It drops HS when CS is asserted.
 *  - Data ready (DR) high: the coprocessor has a frame for the host.
 *  - Every transaction is full duplex. The host sends its pending frame (or
 *    zeros) and receives whatever the coprocessor had queued (or a dummy).
 *  - Frames start with a 12 byte v1 header, little endian, with a 16 bit
 *    byte-sum checksum computed with the checksum field zeroed.
 */

#include <string.h>
#include "esp_spi.h"

typedef struct __attribute__((packed)) {
    uint8_t if_type :4;
    uint8_t if_num :4;
    uint8_t flags;
    uint16_t len;
    uint16_t offset;
    uint16_t checksum;
    uint16_t seq_num;
    uint8_t throttle_cmd :2;
    uint8_t rx_credit :6;
    uint8_t pkt_type;
} esp_hdr_v1_t;

_Static_assert(sizeof(esp_hdr_v1_t) == ESP_SPI_HDR_SIZE, "v1 header must be 12 bytes");

#define CHECKSUM_OFFSET 6

static SPI_HandleTypeDef *spi;
static esp_spi_rx_cb_t rx_callback;
static esp_spi_stats_t stats;

static uint8_t rx_buf[ESP_SPI_BUF_SIZE] __attribute__((aligned(4)));
static uint8_t idle_buf[ESP_SPI_BUF_SIZE] __attribute__((aligned(4)));

// Transmit queue: a small ring of complete transaction buffers.
static uint8_t tx_ring[ESP_SPI_TX_SLOTS][ESP_SPI_BUF_SIZE] __attribute__((aligned(4)));
static uint8_t tx_head;     // next slot to fill
static uint8_t tx_tail;     // next slot to send
static uint8_t tx_count;
static uint16_t tx_seq_num;

#define ESP_SPI_IDLE_POLL_MS 100
#define ESP_SPI_BOOT_MS 1000
#define ESP_SPI_HS_SETTLE_US 50

static uint32_t last_xfer_tick;
static volatile uint8_t hs_edge;
static uint8_t cp_ready;
static uint8_t sync_pending;
static uint32_t reset_tick;
static volatile uint8_t dr_edge;

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

// Busy wait using the DWT cycle counter (enabled in esp_spi_init).
static void delay_us(uint32_t us) {
    uint32_t start = DWT->CYCCNT;
    uint32_t cycles = us * (SystemCoreClock / 1000000);
    while ((DWT->CYCCNT - start) < cycles) {
    }
}

static inline int dr_active(void) {
    return HAL_GPIO_ReadPin(SPI1_DR_GPIO_Port, SPI1_DR_Pin) == GPIO_PIN_SET;
}

void HAL_GPIO_EXTI_Callback(uint16_t gpio_pin) {
    if (gpio_pin == SPI1_HS_Pin) {
        hs_edge = 1;
    } else if (gpio_pin == SPI1_DR_Pin) {
        dr_edge = 1;
    }
}

void esp_spi_init(SPI_HandleTypeDef *hspi, esp_spi_rx_cb_t rx_cb) {
    spi = hspi;
    rx_callback = rx_cb;
    memset(&stats, 0, sizeof(stats));
    tx_head = tx_tail = tx_count = 0;
    sync_pending = 1;
    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_SET);

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    // Enable SPI1 now so SCK sits at its idle level (high, CPOL=1) before
    // CS is ever asserted. HAL would otherwise only enable it inside the
    // first transfer, with CS already low, and the coprocessor would see a
    // spurious clock edge that shifts the whole first frame by one bit.
    __HAL_SPI_ENABLE(spi);
}

void esp_spi_reset_cp(void) {
    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(SPI1_RST_GPIO_Port, SPI1_RST_Pin, GPIO_PIN_RESET);
    HAL_Delay(50);
    HAL_GPIO_WritePin(SPI1_RST_GPIO_Port, SPI1_RST_Pin, GPIO_PIN_SET);
    tx_head = tx_tail = tx_count = 0;
    cp_ready = 0;
    sync_pending = 1;
    reset_tick = HAL_GetTick();
    hs_edge = 0;
    dr_edge = 0;
}

int esp_spi_tx_busy(void) {
    return tx_count >= ESP_SPI_TX_SLOTS;
}

int esp_spi_tx_queued(void) {
    return tx_count;
}

int esp_spi_send(uint8_t if_type, uint8_t if_num, uint8_t flags, uint8_t pkt_type,
                 const uint8_t *payload, uint16_t len) {
    if (tx_count >= ESP_SPI_TX_SLOTS) {
        return -1;
    }
    if (len > ESP_SPI_BUF_SIZE - ESP_SPI_HDR_SIZE) {
        return -2;
    }

    uint8_t *tx_buf = tx_ring[tx_head];
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

    tx_head = (tx_head + 1) % ESP_SPI_TX_SLOTS;
    ++tx_count;
    return 0;
}

static void decode_rx(void) {
    const esp_hdr_v1_t *hdr = (const esp_hdr_v1_t*) rx_buf;


    if (hdr->if_type == ESP_IF_DUMMY || hdr->if_type == ESP_IF_INVALID || (hdr->len == 0 && hdr->flags == 0)) {
        ++stats.rx_dummy;
        return;
    }

    uint32_t total = (uint32_t) hdr->offset + hdr->len;
    if (hdr->offset < ESP_SPI_HDR_SIZE || hdr->offset > ESP_SPI_HDR_SIZE + 7 || total > ESP_SPI_BUF_SIZE) {
        ++stats.rx_bad_header;
        return;
    }

    if (frame_checksum(rx_buf, (uint16_t) total) != hdr->checksum) {
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
                .payload = rx_buf + hdr->offset
        };
        rx_callback(&frame);
    }
}

void esp_spi_poll(void) {
    // After a reset HS floats high while the coprocessor boots, so it can't
    // be trusted until the firmware is up. Hold off for ESP_SPI_BOOT_MS.
    if (!cp_ready) {
        if ((HAL_GetTick() - reset_tick) < ESP_SPI_BOOT_MS) {
            return;
        }
        cp_ready = 1;
    }

    if (!hs_active()) {
        return;
    }

    // Clock when there's something to move in either direction, or every
    // ESP_SPI_IDLE_POLL_MS while HS is up (same as the upstream host).
    uint32_t now = HAL_GetTick();
    if (!tx_count && !dr_active() && !dr_edge && (now - last_xfer_tick) < ESP_SPI_IDLE_POLL_MS) {
        return;
    }

    // The coprocessor raises HS from its post-setup callback, a moment
    // before its SPI slave is fully armed. Clocking right away loses the
    // first bit, so let HS settle before asserting CS.
    delay_us(ESP_SPI_HS_SETTLE_US);
    if (!hs_active()) {
        return;
    }

    dr_edge = 0;
    hs_edge = 0;
    last_xfer_tick = now;

    // The first transaction after a host start or coprocessor reset can be
    // off by one bit: the coprocessor counts a stray SCK edge from the host's
    // pin setup into it. Make that one a throwaway zero frame; CS realigns
    // the coprocessor for every transaction after it.
    const uint8_t *tx = idle_buf;   // all zero: a dummy frame
    uint8_t sending = 0;
    if (sync_pending) {
        sync_pending = 0;
    } else if (tx_count) {
        tx = tx_ring[tx_tail];
        sending = 1;
    }

    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_RESET);

    // The coprocessor drops HS once it has seen CS. Give it that moment
    // before clocking.
    uint32_t t_cs = HAL_GetTick();
    while (hs_active() && (HAL_GetTick() - t_cs) < 2) {
    }

    HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(spi, (uint8_t*) tx, rx_buf, ESP_SPI_BUF_SIZE, 100);
    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_SET);

    ++stats.transactions;

    if (status != HAL_OK) {
        ++stats.spi_errors;
        return;
    }

    if (sending) {
        tx_tail = (tx_tail + 1) % ESP_SPI_TX_SLOTS;
        --tx_count;
        ++stats.tx_frames;
    }

    decode_rx();
}

const esp_spi_stats_t* esp_spi_get_stats(void) {
    return &stats;
}
