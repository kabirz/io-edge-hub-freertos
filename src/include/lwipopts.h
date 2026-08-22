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
/* socket API (ftpd): netconn + select; httpd/mbtcp/udpcfg 仍走 raw 回调 */
#define LWIP_NETCONN            1
#define LWIP_SOCKET             1
#define LWIP_CALLBACK_API       1
#define LWIP_RAW                0
#define LWIP_DHCP               0
#define LWIP_AUTOIP             0
#define LWIP_DNS                0
#define LWIP_IGMP               0
#define LWIP_NETIF_STATUS_CALLBACK 0
#define LWIP_NETIF_LINK_CALLBACK   0

/* socket 选项: FTP 控制/数据连接收发超时 (select 循环依赖) */
#define LWIP_SO_RCVTIMEO        1
#define LWIP_SO_SNDTIMEO        1
/* netconn 收包邮箱深度: lwip 默认全 0 (期望移植层覆盖), 0 会造成
 * xQueueCreate(0) -> configASSERT 死循环 (曾在 FTP 首连冻结全系统) */
#define DEFAULT_TCP_RECVMBOX_SIZE 6
#define DEFAULT_ACCEPTMBOX_SIZE   6
#define DEFAULT_UDP_RECVMBOX_SIZE 6
/* struct timeval 用 newlib sys/time.h 的定义 (cc.h 已包含);
 * lwip 自带的私有定义会与 newlib 冲突 */
#define LWIP_TIMEVAL_PRIVATE    0
/* fd_set 容量: 最多 10 socket 并发 (ftpd serv+3ctrl+3listen+3data) */
#ifndef FD_SETSIZE
#define FD_SETSIZE              16
#endif

/* ---- Timing ---- */
#define LWIP_TIMERS             1
#define LWIP_TIMERS_CUSTOM      0
#define TCP_FAST_INTERVAL       500
#define TCP_SLOW_INTERVAL       500

/* ---- Tcpip thread ---- */
#define TCPIP_THREAD_STACKSIZE  1024  /* words; UDP 回调+发送链深栈, 2KB 溢出过 */
#define TCPIP_THREAD_PRIO       4
#define TCPIP_MBOX_SIZE         8     /* tcpip thread mailbox slots */

/* ---- Memory ---- */
#define MEM_SIZE                (24u * 1024)   /* 24 KB LwIP heap (CCM) */

#define MEMP_NUM_PBUF           16
#define MEMP_NUM_RAW_PCB        0
/* mbtcp 2 + httpd 2 + ftpd: 21 监听外的 3 控制 + 3 数据 + 余量
 * (LISTEN 为独立池) */
#define MEMP_NUM_TCP_PCB        14
/* mbtcp + httpd + ftp:21 永久 + 3 个 PASV 监听 */
#define MEMP_NUM_TCP_PCB_LISTEN 6
/* httpd 流式发送多段在途 */
#define MEMP_NUM_TCP_SEG        32
#define MEMP_NUM_UDP_PCB        3   /* cfg + discover + margin */
#define MEMP_NUM_NETBUF         16  /* socket recv netbuf (ftpd) */
#define MEMP_NUM_NETCONN        12  /* ftpd: serv + 3ctrl + 3listen + 3data */
#define MEMP_NUM_SYS_TIMEOUT    6

/* ---- Pbuf ---- */
/* pbuf pool in CCM (64KB, no DMA limitation). */
#define PBUF_POOL_SIZE          16  /* 16 × 1540B ≈ 24.6KB */
#define PBUF_POOL_BUFSIZE       1540
#define PBUF_LINK_HLEN          14
#define PBUF_ETH_HLEN           14

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
/* STM32F407 无硬件校验和, 全软件计算 */
#define CHECKSUM_GEN_IP         1
#define CHECKSUM_GEN_UDP        1
#define CHECKSUM_GEN_TCP        1
#define CHECKSUM_GEN_ICMP       1
#define CHECKSUM_CHECK_IP       1
#define CHECKSUM_CHECK_UDP      1
#define CHECKSUM_CHECK_TCP      1
#define CHECKSUM_CHECK_ICMP     1

/* ---- Debug ---- */
/* printf 诊断在 tcpip 线程同步执行 (每包数十 ms 延迟), 默认关闭 */
#define LWIP_DEBUG              0
#define LWIP_DBG_TYPES_ON       LWIP_DBG_OFF
#define LWIP_DBG_MIN_LEVEL      0
#define UDP_DEBUG               LWIP_DBG_OFF
#define IP_DEBUG                LWIP_DBG_OFF

/* ---- Statistics ---- */
#define LWIP_STATS              0

/* ---- threading / sys ---- */
/* errno 走 newlib (<errno.h>, __errno() 可重入): LWIP_PROVIDE_ERRNO
 * 须完全不定义 (errno.h 用 #ifdef 判定, 定义为 0 也算已定义);
 * LWIP_ERRNO_STDINCLUDE 让 lwip/errno.h 直接包含 <errno.h> */
#define LWIP_ERRNO_STDINCLUDE   1

/* ---- dhcp / arp ---- */
#define LWIP_ARP                1
#define ARP_TABLE_SIZE          10
#define ARP_QUEUEING            0

#endif /* LWIPOPTS_H */
