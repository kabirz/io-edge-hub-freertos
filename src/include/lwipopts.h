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

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

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

/* ---- Tcpip thread ---- */
#define TCPIP_THREAD_STACKSIZE  1024  /* 4096 bytes (in words); UDP 回调+发送链
                                           * (udp_input -> app -> udp_sendto ->
                                           * ip4_route) 深栈, 2KB 会溢出 */
#define TCPIP_THREAD_PRIO       4
#define TCPIP_MBOX_SIZE         8     /* tcpip thread mailbox slots */

/* ---- Memory ---- */
#define MEM_SIZE                (24u * 1024)   /* 24 KB LwIP heap (CCM) */

#define MEMP_NUM_PBUF           16
#define MEMP_NUM_RAW_PCB        0
/* mbtcp 2 并发 + httpd 2 并发 + 余量 (TCP_PCB_LISTEN 独立池) */
#define MEMP_NUM_TCP_PCB        8
#define MEMP_NUM_TCP_PCB_LISTEN 2
/* httpd 流式发送多段在途: 2 连接 x ~8 段 + 重传余量 */
#define MEMP_NUM_TCP_SEG        32
#define MEMP_NUM_UDP_PCB        3   /* cfg + discover + margin */
#define MEMP_NUM_NETBUF         0
#define MEMP_NUM_NETCONN        0
#define MEMP_NUM_SYS_TIMEOUT    6

/* ---- Pbuf ---- */
/* pbuf pool in CCM (64KB, no DMA limitation).
 * PBUF_POOL_BUFSIZE must >= 1500 + PBUF_LINK_HLEN (14) for full Ethernet frame. */
#define PBUF_POOL_SIZE          16  /* 16 × 1540B ≈ 24.6KB, 在 CCM (64KB) 内 */
#define PBUF_POOL_BUFSIZE       1540
#define PBUF_LINK_HLEN          14
#define PBUF_ETH_HLEN           14
/* PBUF_IP_HLEN: use LwIP default (20 for IPv4) */

/* ---- TCP ---- */
#define TCP_MSS                 1460  /* 1500 - 40 (IP+TCP headers) */
#define TCP_SND_BUF             (4 * TCP_MSS)
#define TCP_SND_QUEUELEN        16
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

/* ---- Debug ---- */
/* 调试期曾开启 (printf 诊断在 tcpip 线程同步执行, 每包数十 ms 延迟),
 * 已完成 UDP/MACRAW bring-up, 关闭。需要时改回 1 + 打开模块开关。 */
#define LWIP_DEBUG              0
#define LWIP_DBG_TYPES_ON       LWIP_DBG_OFF
#define LWIP_DBG_MIN_LEVEL      0
#define UDP_DEBUG               LWIP_DBG_OFF
#define IP_DEBUG                LWIP_DBG_OFF

/* ---- Statistics ---- */
#define LWIP_STATS              0

/* ---- threading / sys ---- */
#define LWIP_PROVIDE_ERRNO      1

/* ---- dhcp / arp ---- */
#define LWIP_ARP                1
#define ARP_TABLE_SIZE          10
#define ARP_QUEUEING            0

#endif /* LWIPOPTS_H */
