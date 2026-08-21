/*
 * UDP 配置服务传输层: LwIP UDP PCB 监听 8600, recv 回调处理命令
 * (替代 ioLibrary socket API, MACRAW 接管 socket 0 后不可用)。
 * 跨网段过滤逻辑保持不变。
 */

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lwip/udp.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"

#include "w5500.h"
#include "udp_cfg.h"
#include "init.h"
#include "io_hooks.h"

#include "log.h"

#define UDP_CFG_PORT       8600u
#define UDP_CFG_BCAST_PORT (UDP_CFG_PORT + 1u)

/* 诊断探针: 回调计数 (RAM 探针, ST-LINK 可读) */
volatile uint32_t udp_recv_calls;
volatile uint32_t udp_recv_bytes;

static struct udp_pcb *cfg_pcb;

/* FACTORY_RESET 两步确认计时源: tick -> ms */
static uint32_t udp_now_ms_target(void)
{
    return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}

/* 发送方与本机是否同 /24 网段 */
static bool same_subnet24(const ip_addr_t *addr)
{
    uint32_t a = ip4_addr_get_u32(addr);
    uint32_t local = ((uint32_t)get_holding_reg(HOLDING_IP_OCTET1_IDX) |
                      ((uint32_t)get_holding_reg(HOLDING_IP_OCTET2_IDX) << 8) |
                      ((uint32_t)get_holding_reg(HOLDING_IP_OCTET3_IDX) << 16));
    return (a & 0x00FFFFFF) == (local & 0x00FFFFFF);
}

/* LwIP UDP recv 回调 */
static void udp_cfg_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                          const ip_addr_t *src_addr, u16_t src_port)
{
    udp_recv_calls++;
    (void)arg;
    (void)pcb;
    (void)src_addr;
    (void)src_port;

    if (p == NULL || p->tot_len == 0) {
        return;
    }
    udp_recv_bytes += p->tot_len;

    /* 提取命令字节 (第一个字节) */
    uint8_t cmd = 0;
    pbuf_copy_partial(p, &cmd, 1, 0);

    /* 跨网段白名单外的命令: 静默丢弃 */
    bool same = same_subnet24(src_addr);
    if (!same && !udp_cmd_bcast_allowed(cmd)) {
        LOG_WRN("udpcfg: drop cross-subnet cmd 0x%02x", cmd);
        pbuf_free(p);
        return;
    }

    /* 提取完整 payload */
    uint8_t rx[256];
    uint16_t copy_len = (p->tot_len < sizeof(rx)) ? p->tot_len : (uint16_t)sizeof(rx);
    pbuf_copy_partial(p, rx, copy_len, 0);
    pbuf_free(p);

    /* 处理命令 */
    uint8_t rep[64];
    uint16_t rlen = udp_app_cmd(rx[0], &rx[1], (uint16_t)(copy_len - 1),
                                 rep, sizeof(rep));
    if (rlen == 0) {
        return; /* 未知命令静默 */
    }

    /* 发送应答 */
    struct pbuf *rp = pbuf_alloc(PBUF_TRANSPORT, rlen, PBUF_RAM);
    if (rp == NULL) {
        return;
    }
    pbuf_take(rp, rep, rlen);

    if (same) {
        /* 同网段: 单播回源地址 */
        udp_sendto(pcb, rp, src_addr, src_port);
    } else {
        /* 跨网段: 定向广播应答 (config+1) */
        ip_addr_t bcast;
        IP_ADDR4(&bcast, 255, 255, 255, 255);
        udp_sendto(pcb, rp, &bcast, UDP_CFG_BCAST_PORT);
    }
    pbuf_free(rp);

    /* FACTORY_RESET / REBOOT 确认步: 应答已发送, 现在重启 */
    if (udp_cfg_reboot_pending()) {
        history_sync();
        io_reboot_cold();
    }
}

/* RAW API 须在 tcpip 线程执行 (避免与 udp_input 遍历 udp_pcbs 竞态) */
static void udp_cfg_init_cb(void *arg)
{
    (void)arg;

    cfg_pcb = udp_new();
    if (cfg_pcb == NULL) {
        LOG_ERR("udpcfg: udp_new failed");
        return;
    }

    if (udp_bind(cfg_pcb, IP_ADDR_ANY, UDP_CFG_PORT) != ERR_OK) {
        LOG_ERR("udpcfg: bind port %u failed", UDP_CFG_PORT);
        udp_remove(cfg_pcb);
        cfg_pcb = NULL;
        return;
    }

    udp_recv(cfg_pcb, udp_cfg_recv, NULL);
    LOG_INF("udpcfg: port %u listening (LwIP) pcb=%p recv=%p",
            UDP_CFG_PORT, (void *)cfg_pcb, (void *)cfg_pcb->recv);
}

void udp_cfg_start(void)
{
    udp_now_ms = udp_now_ms_target;

    tcpip_callback(udp_cfg_init_cb, NULL);
}
