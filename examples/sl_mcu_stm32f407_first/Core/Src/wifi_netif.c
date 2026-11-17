/*
 * wifi_netif.c - lwIP network interface on the ESP-Hosted STA data path (no RTOS).
 *
 * Frames on the STA interface (if_type 1) are plain Ethernet frames. Received
 * frames are copied into pbufs and queued; lwIP only sees them from
 * wifi_netif_poll() in the main loop, so lwIP is never re-entered from inside
 * its own transmit path.
 */

#include <stdio.h>
#include <string.h>
#include "main.h"
#include "wifi_netif.h"
#include "wifi_sta.h"
#include "esp_host.h"
#include "esp_spi.h"

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

#define RX_QUEUE_LEN    8
#define TX_WAIT_MS      20

static struct netif netif;
static uint8_t netif_added;
static uint8_t link_up;
static uint32_t last_ip;

static struct pbuf *rx_queue[RX_QUEUE_LEN];
static uint8_t rx_head, rx_tail, rx_count;
static uint32_t rx_dropped;

static uint8_t tx_frame[ESP_SPI_BUF_SIZE - ESP_SPI_HDR_SIZE];
static char hostname[16];

/* lwIP platform hooks */

uint32_t sys_now(void) {
    return HAL_GetTick();
}

uint32_t lwip_rand(void) {
    static uint32_t x;
    if (!x) {
        x = HAL_GetTick() ^ 0x9e3779b9u ^ (wifi_sta_get_mac()[5] << 8) ^ wifi_sta_get_mac()[4];
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

/* Data path */

static void sta_rx(const uint8_t *frame, uint16_t len) {
    if (rx_count >= RX_QUEUE_LEN || !netif_added) {
        ++rx_dropped;
        return;
    }
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (!p) {
        ++rx_dropped;
        return;
    }
    pbuf_take(p, frame, len);
    rx_queue[rx_head] = p;
    rx_head = (rx_head + 1) % RX_QUEUE_LEN;
    ++rx_count;
}

static err_t linkoutput(struct netif *n, struct pbuf *p) {
    (void) n;
    if (p->tot_len > sizeof(tx_frame)) {
        return ERR_BUF;
    }
    uint16_t len = pbuf_copy_partial(p, tx_frame, p->tot_len, 0);

    // Queue full: pump the SPI link a little to make room.
    uint32_t start = HAL_GetTick();
    while (esp_spi_tx_busy()) {
        esp_spi_poll();
        if (HAL_GetTick() - start > TX_WAIT_MS) {
            return ERR_MEM;
        }
    }
    return esp_spi_send(ESP_IF_STA, 0, 0, 0, tx_frame, len) == 0 ? ERR_OK : ERR_IF;
}

static void status_callback(struct netif *n) {
    uint32_t ip = ip4_addr_get_u32(netif_ip4_addr(n));
    if (ip != last_ip) {
        last_ip = ip;
        if (ip) {
            printf("wifi_netif: IP %s", ip4addr_ntoa(netif_ip4_addr(n)));
            printf(" mask %s", ip4addr_ntoa(netif_ip4_netmask(n)));
            printf(" gw %s\n", ip4addr_ntoa(netif_ip4_gw(n)));
        } else {
            printf("wifi_netif: IP released\n");
        }
    }
}

static err_t netif_init_cb(struct netif *n) {
    n->name[0] = 'w';
    n->name[1] = 'l';
    n->mtu = 1500;
    n->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(n->hwaddr, wifi_sta_get_mac(), ETH_HWADDR_LEN);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    n->output = etharp_output;
    n->linkoutput = linkoutput;
#if LWIP_NETIF_HOSTNAME
    n->hostname = hostname;
#endif
    return ERR_OK;
}

// DHCP hostname: "stm32-" plus 6 hex digits folded from the 96 bit unique ID,
// so several boards on one network get different names.
static void make_hostname(void) {
    uint32_t id = HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2();
    id ^= id >> 24;
    snprintf(hostname, sizeof(hostname), "stm32-%06lx", id & 0xffffff);
}

void wifi_netif_init(void) {
    make_hostname();
    printf("wifi_netif: hostname %s\n", hostname);
    lwip_init();
    esp_host_set_sta_rx(sta_rx);
}

void wifi_netif_poll(void) {
    // Feed received frames to lwIP.
    while (rx_count) {
        struct pbuf *p = rx_queue[rx_tail];
        rx_tail = (rx_tail + 1) % RX_QUEUE_LEN;
        --rx_count;
        if (netif.input(p, &netif) != ERR_OK) {
            pbuf_free(p);
        }
    }

    // Follow the Wi-Fi connection state.
    int connected = wifi_sta_get_state() == WIFI_STA_CONNECTED;
    if (connected && !netif_added) {
        netif_add(&netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL, netif_init_cb, ethernet_input);
        netif_set_default(&netif);
        netif_set_status_callback(&netif, status_callback);
        netif_set_up(&netif);
        netif_added = 1;
        printf("wifi_netif: interface up, starting DHCP\n");
        netif_set_link_up(&netif);
        link_up = 1;
        dhcp_start(&netif);
    } else if (netif_added && connected != link_up) {
        link_up = connected;
        if (connected) {
            netif_set_link_up(&netif);      // lwIP restarts DHCP on link up
        } else {
            netif_set_link_down(&netif);
        }
    }

    sys_check_timeouts();
}

const char* wifi_netif_get_hostname(void) {
    return hostname;
}

uint32_t wifi_netif_get_ip(void) {
    return last_ip;
}
