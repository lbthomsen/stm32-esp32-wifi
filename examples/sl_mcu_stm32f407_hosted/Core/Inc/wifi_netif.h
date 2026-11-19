/*
 * wifi_netif.h - lwIP network interface on the ESP-Hosted STA data path (no RTOS).
 *
 * The coprocessor passes Ethernet frames between its Wi-Fi station and the
 * host; this module makes that an lwIP netif with DHCP.
 */

#ifndef WIFI_NETIF_H_
#define WIFI_NETIF_H_

#include <stdint.h>

// Initialise lwIP and hook the station data path (call once at start-up).
void wifi_netif_init(void);

// Main loop: received frames into lwIP, link state, lwIP timers.
void wifi_netif_poll(void);

// IPv4 address in network order, 0 if none.
uint32_t wifi_netif_get_ip(void);

// DHCP hostname, "stm32-" plus 6 hex digits from the STM32 unique ID.
const char* wifi_netif_get_hostname(void);

#endif /* WIFI_NETIF_H_ */
