/*
 * lwipopts.h - lwIP configuration for the STM32 host (no RTOS, NO_SYS=1).
 *
 * Everything runs from the main loop: received frames are fed to lwIP from
 * wifi_netif_poll() and sys_check_timeouts() drives the timers.
 */

#ifndef LWIPOPTS_H_
#define LWIPOPTS_H_

/* No RTOS */
#define NO_SYS                          1
#define SYS_LIGHTWEIGHT_PROT            0
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0

/* Memory */
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (16 * 1024)
#define MEMP_NUM_PBUF                   16
#define MEMP_NUM_TCP_PCB                4
#define MEMP_NUM_TCP_SEG                16
#define MEMP_NUM_UDP_PCB                6
#define PBUF_POOL_SIZE                  16
#define PBUF_POOL_BUFSIZE               1536

/* Protocols */
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_RAW                        0
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_DHCP                       1
#define LWIP_DNS                        1
#define LWIP_IGMP                       0

#define TCP_MSS                         1460
#define TCP_WND                         (4 * TCP_MSS)
#define TCP_SND_BUF                     (4 * TCP_MSS)
#define TCP_SND_QUEUELEN                (2 * TCP_SND_BUF / TCP_MSS)

/* Netif */
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_NETIF_HOSTNAME             1
#define LWIP_SINGLE_NETIF               1

/* Checksums in software (the ESP passes frames through untouched) */
#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_UDP                1
#define CHECKSUM_GEN_TCP                1
#define CHECKSUM_GEN_ICMP               1
#define CHECKSUM_CHECK_IP               1
#define CHECKSUM_CHECK_UDP              1
#define CHECKSUM_CHECK_TCP              1
#define CHECKSUM_CHECK_ICMP             1

/* Debug / stats */
#define LWIP_STATS                      0
#define LWIP_DEBUG                      0

#endif /* LWIPOPTS_H_ */
