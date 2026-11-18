/*
 * wifi_netif.h - lwIP network interface on the ESP-Hosted STA data path (no RTOS).
 */

#ifndef WIFI_NETIF_H_
#define WIFI_NETIF_H_

#include <stdint.h>

void wifi_netif_init(void);
void wifi_netif_poll(void);
uint32_t wifi_netif_get_ip(void);
const char* wifi_netif_get_hostname(void);   // IPv4 address in network order, 0 if none

#endif /* WIFI_NETIF_H_ */
