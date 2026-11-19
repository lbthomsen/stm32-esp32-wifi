/*
 * esp_host.c - ESP-Hosted host protocol layer (no RTOS).
 *
 * Init sequence (esp_hosted v3, MCU host):
 *  1. The coprocessor sends a PRIV frame (if_type 5, pkt_type 0x33 = event)
 *     with event 0x22 (INIT) and a list of TLVs: chip id, capabilities,
 *     firmware version, RPC wire version, transfer size, ...
 *  2. The host answers with a PRIV frame carrying event 0x22 and its own
 *     TLVs: host capabilities, echoed chip id, raw throughput test mode,
 *     throttle thresholds and (if advertised) the RPC wire version.
 *  3. The coprocessor then sends the RPC "ESP init" event and accepts RPC
 *     requests (esp_rpc) and network frames (wifi_netif).
 *
 * Received frames are routed by interface type: PRIV (init) is handled
 * here, SERIAL goes to esp_rpc, STA (Ethernet frames) to the netif.
 * If no init event arrives, the host first asks for one (wake flag) and
 * then resets the coprocessor through EN.
 *
 * Runs in main loop context (frames arrive through esp_spi_poll()).
 */

#include <stdio.h>
#include <string.h>
#include "esp_host.h"
#include "esp_rpc.h"

#define PRIV_PKT_TYPE_EVENT             0x33
#define PRIV_EVENT_INIT                 0x22

/* CP -> host TLVs */
#define TLV_CAPABILITY                  0x11
#define TLV_FIRMWARE_CHIP_ID            0x12
#define TLV_TEST_RAW_TP                 0x13
#define TLV_RX_Q_SIZE                   0x14
#define TLV_TX_Q_SIZE                   0x15
#define TLV_CAP_EXT                     0x16
#define TLV_FIRMWARE_VERSION            0x17
#define TLV_RPC_VERSION                 0x1a
#define TLV_TRANSFER_SIZE               0x1c

/* host -> CP TLVs */
#define TLV_HOST_CAPABILITIES           0x44
#define TLV_HOST_RCVD_CHIP_ID           0x45
#define TLV_HOST_TEST_RAW_TP            0x46
#define TLV_HOST_THROTTLE_HIGH          0x47
#define TLV_HOST_THROTTLE_LOW           0x48

#define RPC_VERSION_V2                  0x02

/* Header flag: host woke up. The coprocessor answers it by reopening the
 * data path and sending a fresh init event, so a host restart works even
 * without resetting the coprocessor. */
#define FLAG_POWER_SAVE_STOPPED         (1 << 3)
#define TEST_RAW_TP_NONE                0x00

static esp_host_info_t info;           // what the coprocessor announced
static uint8_t init_reply_pending;      // init event seen, reply not queued yet
static esp_host_data_rx_cb_t sta_rx_callback;
static uint32_t init_wait_start;

#define ESP_HOST_INIT_TIMEOUT_MS 4000   // no init event this long after a reset: reset again
#define ESP_HOST_WAKE_AFTER_MS 2000     // no init event this long: ask for one

static uint8_t wake_sent;
static uint32_t last_rx_tick;           // for the liveness check in wifi_sta

// A little endian TLV value of 1 to 4 bytes.
static uint32_t tlv_u32(const uint8_t *v, uint8_t len) {
    uint32_t r = 0;
    for (uint8_t i = 0; i < len && i < 4; ++i) {
        r |= (uint32_t) v[i] << (8 * i);
    }
    return r;
}

// The coprocessor's init event: record what it announced and schedule the
// host's reply (sent from esp_host_poll once the transmit queue has room).
static void handle_init_event(const uint8_t *tlv, uint16_t len) {
    if (info.ready) {
        // The coprocessor restarted on its own (crash, watchdog, brownout).
        // Anything in flight went with it.
        printf("esp_host: coprocessor restarted\n");
        esp_rpc_reset();
    }
    memset(&info, 0, sizeof(info));

    while (len >= 2) {
        uint8_t tag = tlv[0];
        uint8_t tag_len = tlv[1];
        if (tag_len + 2u > len) {
            printf("esp_host: truncated init TLV 0x%02x\n", tag);
            break;
        }
        const uint8_t *v = tlv + 2;

        switch (tag) {
        case TLV_CAPABILITY:
            info.caps = v[0];
            break;
        case TLV_FIRMWARE_CHIP_ID:
            info.chip_id = v[0];
            break;
        case TLV_RX_Q_SIZE:
            info.rx_q_size = v[0];
            break;
        case TLV_TX_Q_SIZE:
            info.tx_q_size = v[0];
            break;
        case TLV_CAP_EXT:
            info.ext_caps = tlv_u32(v, tag_len);
            break;
        case TLV_FIRMWARE_VERSION:
            info.fw_version = tlv_u32(v, tag_len);
            break;
        case TLV_RPC_VERSION:
            info.rpc_version = v[0];
            break;
        case TLV_TRANSFER_SIZE:
            info.transfer_size = tlv_u32(v, tag_len);
            break;
        default:
            break;
        }

        tlv += tag_len + 2;
        len -= tag_len + 2;
    }

    printf("esp_host: coprocessor up - chip_id=%u fw=%lu.%lu.%lu caps=0x%02x ext_caps=0x%08lx rpc=v%u xfer=%lu q=%u/%u\n",
            info.chip_id,
            (info.fw_version >> 16) & 0xff, (info.fw_version >> 8) & 0xff, info.fw_version & 0xff,
            info.caps, info.ext_caps, info.rpc_version, info.transfer_size, info.rx_q_size, info.tx_q_size);

    if (info.transfer_size && info.transfer_size != ESP_SPI_BUF_SIZE) {
        printf("esp_host: WARNING coprocessor transfer size %lu, host uses %u\n", info.transfer_size, ESP_SPI_BUF_SIZE);
    }

    init_reply_pending = 1;
}

// Queue the host's answer to the init event:
//   [0x22][len] then TLVs [tag][1][value] for capabilities (none), the chip id
//   we received, raw throughput test off, flow control thresholds (off), and
//   RPC v2 if the coprocessor offered a version.
static int send_init_reply(void) {
    uint8_t buf[32];
    uint8_t *p = buf;

    *p++ = PRIV_EVENT_INIT;
    uint8_t *evt_len = p++;

    *p++ = TLV_HOST_CAPABILITIES;
    *p++ = 1;
    *p++ = 0;

    *p++ = TLV_HOST_RCVD_CHIP_ID;
    *p++ = 1;
    *p++ = info.chip_id;

    *p++ = TLV_HOST_TEST_RAW_TP;
    *p++ = 1;
    *p++ = TEST_RAW_TP_NONE;

    *p++ = TLV_HOST_THROTTLE_HIGH;
    *p++ = 1;
    *p++ = 0;

    *p++ = TLV_HOST_THROTTLE_LOW;
    *p++ = 1;
    *p++ = 0;

    if (info.rpc_version) {
        *p++ = TLV_RPC_VERSION;
        *p++ = 1;
        *p++ = RPC_VERSION_V2;
    }

    *evt_len = (uint8_t) (p - buf - 2);

    return esp_spi_send(ESP_IF_PRIV, 0, 0, 0, buf, (uint16_t) (p - buf));
}

// An empty PRIV frame with the "power save stopped" flag: the coprocessor
// answers with a fresh init event.
static void send_wake(void) {
    esp_spi_send(ESP_IF_PRIV, 0, FLAG_POWER_SAVE_STOPPED, 0, NULL, 0);
}

// Every good frame from the transport lands here; route it by interface.
static void host_rx(const esp_frame_t *frame) {
    last_rx_tick = HAL_GetTick();

    if (frame->if_type == ESP_IF_PRIV && frame->len >= 2 && frame->payload[0] == PRIV_EVENT_INIT) {
        uint8_t evt_len = frame->payload[1];
        if (evt_len + 2u <= frame->len) {
            handle_init_event(frame->payload + 2, evt_len);
            return;
        }
    }

    if (frame->if_type == ESP_IF_SERIAL) {
        esp_rpc_rx(frame);
        return;
    }

    if (frame->if_type == ESP_IF_STA) {
        if (sta_rx_callback) {
            sta_rx_callback(frame->payload, frame->len);
        }
        return;
    }

    printf("esp_host: rx if_type=%u if_num=%u flags=0x%02x pkt_type=0x%02x len=%u\n",
            frame->if_type, frame->if_num, frame->flags, frame->pkt_type, frame->len);
}

// Set up the transport and reset the coprocessor (a clean start every time).
void esp_host_init(SPI_HandleTypeDef *hspi) {
    esp_spi_init(hspi, host_rx);
    esp_host_restart_cp();
}

// Main loop: received frames, RPC, the init handshake and its timeouts.
void esp_host_poll(void) {
    esp_spi_poll();

    // The init reply goes out before any RPC traffic.
    if (!init_reply_pending) {
        esp_rpc_poll();
    }

    // A freshly reset coprocessor sends its init event by itself. If none
    // has arrived (it wasn't reset, e.g. no reset line), ask for one.
    if (!info.ready && !init_reply_pending && !wake_sent && (HAL_GetTick() - init_wait_start) > ESP_HOST_WAKE_AFTER_MS
            && !esp_spi_tx_busy()) {
        send_wake();
        wake_sent = 1;
    }

    // No init event from the coprocessor: reset it and try again.
    if (!info.ready && !init_reply_pending && (HAL_GetTick() - init_wait_start) > ESP_HOST_INIT_TIMEOUT_MS) {
        printf("esp_host: no init event from coprocessor, resetting it\n");
        esp_spi_reset_cp();
        wake_sent = 0;
        init_wait_start = HAL_GetTick();
    }

    if (init_reply_pending && !esp_spi_tx_busy()) {
        if (send_init_reply() == 0) {
            init_reply_pending = 0;
            info.ready = 1;
            printf("esp_host: init reply queued\n");
        }
    }
}

// Reset the coprocessor through EN and start the handshake over.
void esp_host_restart_cp(void) {
    printf("esp_host: resetting coprocessor\n");
    memset(&info, 0, sizeof(info));
    init_reply_pending = 0;
    esp_rpc_reset();
    esp_spi_reset_cp();
    wake_sent = 0;
    init_wait_start = HAL_GetTick();
}

// HAL tick of the last frame received.
uint32_t esp_host_last_rx(void) {
    return last_rx_tick;
}

// Where Ethernet frames from the station interface go (wifi_netif).
void esp_host_set_sta_rx(esp_host_data_rx_cb_t cb) {
    sta_rx_callback = cb;
}

// What the coprocessor announced in its init event.
const esp_host_info_t* esp_host_get_info(void) {
    return &info;
}
