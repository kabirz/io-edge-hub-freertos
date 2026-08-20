/*
 * LwIP configuration for io-edge-hub FreeRTOS + W5500 MACRAW.
 *
 * Memory budget (STM32F407VET6: 128KB SRAM + 64KB CCM):
 *   SRAM free: ~72KB (after .bss 56KB)
 *   CCM free: 64KB (unused, pbuf pool goes here)
 *
 *   LwIP heap (MEM_SIZE):        16 KB  SRAM
 *   pbuf pool (PBUF_POOL):  25 × 1540B  CCM  (~38.5 KB)
 *   MEMP pools:              ~8 KB  SRAM
 *   Total LwIP:             ~62.5 KB
 */

#ifndef LWIP_Hkıl_LWIPOPTS_H
#define LWIP_H_kil_LWIPOPTS_H

/* ---- Platform ---- */
#define NO_SYS                  0
#define LWIP_NETCONN            0
#define LWIP_SOCKET             0
#define LWIP_CALLBACK_API       1
#define LWIP_RAW                0
#define LWIP_DHCP               0
#define LWIP_AUTOIP             0
#define LWIP_DNS                0
#define LWIP_IGMP               0
#define LWIP_NETIF_STATUS_CALLBACK 0
#define LWIP_NETIF_LINK_CALLBACK   0

/* ---- Timing ---- */
#define LWIP_TIMERS             1
#define LWIP_TIMERS_CUSTOM      0
#define TCP_FAST_INTERVAL       500
#define TCP_SLOW_INTERVAL       500

/* ---- Memory ---- */
#define MEM_SIZE                (16u * 1024)   /* 16 KB LwIP heap (SRAM) */

#define MEMP_NUM_PBUF           16
#define MEMP_NUM_RAW_PCB        0
#define MEMP_NUM_TCP_PCB        4   /* Modbus TCP: max 2 concurrent + margin */
#define MEMP_NUM_TCP_PCB_LISTEN 2
#define MEMP_NUM_TCP_SEG        8
#define MEMP_NUM_UDP_PCB        3   /* cfg + discover + margin */
#define MEMP_NUM_NETBUF         0
#define MEMP_NUM_NETCONN        0
#define MEMP_NUM_SYS_TIMEOUT    6

/* ---- Pbuf ---- */
/* pbuf pool in CCM (64KB, no DMA limitation).
 * PBUF_POOL_BUFSIZE must >= 1500 + PBUF_LINK_HLEN (14) for full Ethernet frame. */
#define PBUF_POOL_SIZE          25
#define PBUF_POOL_BUFSIZE       1540
#define PBUF_LINK_HLEN          14
#define PBUF_ETH_HLEN           14
#define PBUF_IP_HLEN            40  /* max(IPv4, IPv6) header */

/* ---- TCP ---- */
#define TCP_MSS                 1460  /* 1500 - 40 (IP+TCP headers) */
#define TCP_SND_BUF             (4 * TCP_MSS)
#define TCP_SND_QUEUELEN        (2 * TCP_SND_BUF / TCP_MSS)
#define TCP_WND                 (4 * TCP_MSS)
#define TCP_OOSEQ_MAX           4
#define LWIP_TCP_KEEPALIVE      0
#define LWIP_NETIF_TX_SINGLE_PBUF 1  /* force single pbuf for TX (simplifies MACRAW send) */

/* ---- UDP ---- */
#define UDP_TTL                 64

/* ---- Checksums ---- */
/* STM32F407 has no hardware checksum for TCP/UDP; compute in software. */
#define CHECKSUM_GEN_IP         1
#define CHECKSUM_GEN_UDP        1
#define CHECKSUM_GEN_TCP        1
#define CHECKSUM_GEN_ICMP       1
#define CHECKSUM_CHECK_IP       1
#define CHECKSUM_CHECK_UDP      1
#define CHECKSUM_CHECK_TCP      1
#define CHECKSUM_CHECK_ICMP     1

/* ---- Debug (disabled in release) ---- */
#define LWIP_DEBUG              0

/* ---- Statistics ---- */
#define LWIP_STATS              0

/* ---- threading / sys ---- */
#define LWIP_PROVIDE_ERRNO      1

/* ---- dhcp / arp ---- */
#define LWIP_ARP                1
#define ARP_TABLE_SIZE          10
#define ARP_QUEUEING            0

#endif /* LWIP_H_kil_LWIPOPTS_H */
