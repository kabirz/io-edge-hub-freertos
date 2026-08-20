/*
 * W5500 MACRAW netif driver for LwIP.
 *
 * Socket 0 in MACRAW mode (Sn_MR_MACRAW). Full 16KB RX + 16KB TX buffers.
 * EXTI interrupt on INT pin (PD1) -> counting semaphore -> RX task.
 * 2-byte length header handling with byte-swap and sanity check.
 */

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "main.h"
#include "spi.h"
#include "wizchip_conf.h"
#include "w5500_macraw.h"

#include "init.h"      /* get_holding_reg, HOLDING_IP_OCTET*_IDX */

#include "lwip/init.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"

#include "log.h"

/* W5500 MACRAW socket number (fixed to 0) */
#define MACRAW_SN  0

/* EXTI interrupt pin: PD1 (W5500 INT) */
#define W5500_INT_PORT  GPIOD
#define W5500_INT_PIN   GPIO_PIN_1

/* RX task config */
#define MACRAW_RX_PRIO   5
#define MACRAW_RX_STACK  512  /* 2048 bytes */

/* ==================== Forward declarations ==================== */
static err_t low_level_output(struct netif *netif, struct pbuf *p);

/* ==================== Static state ==================== */
static struct netif w5500_netif;
static SemaphoreHandle_t rx_sem;
static StaticSemaphore_t rx_sem_cb;
static StackType_t rx_stack[MACRAW_RX_STACK];
static StaticTask_t rx_tcb;

/* Debug counters */
volatile uint32_t macraw_rx_total;
volatile uint32_t macraw_rx_ipv4;
volatile uint32_t macraw_rx_arp;
volatile uint32_t macraw_rx_other;

/* ==================== Low-level: init / output / input ==================== */

static err_t macraw_netif_init(struct netif *netif)
{
    netif->output = etharp_output;
    netif->linkoutput = low_level_output;
    netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
    return ERR_OK;
}

static void low_level_init(struct netif *netif)
{
    /* Socket 0 buffers already configured by w5500_net_init (8KB RX/TX).
     * Just set MAC address and open the MACRAW socket. */
    netif->hwaddr[0] = 0x00;
    netif->hwaddr[1] = 0x08;
    netif->hwaddr[2] = 0xDC;
    netif->hwaddr_len = ETHARP_HWADDR_LEN;

    close(MACRAW_SN);
    socket(MACRAW_SN, Sn_MR_MACRAW, 0, 0x00);

    uint8_t sr = getSn_SR(MACRAW_SN);
    uint8_t mr = getSn_MR(MACRAW_SN);
    LOG_INF("MACRAW: SR=0x%02x MR=0x%02x (expect SR=0x42 MR=0x02)", sr, mr);
    if (sr != SOCK_MACRAW) {
        LOG_ERR("MACRAW: socket open failed (SR=0x%02x)", getSn_SR(MACRAW_SN));
        return;
    }
    LOG_INF("MACRAW: socket 0 opened, 16KB RX/TX");
}

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
    struct pbuf *q;
    (void)netif;

    /* Wait for socket ready */
    uint32_t t0 = (uint32_t)xTaskGetTickCount();
    while (getSn_SR(MACRAW_SN) != SOCK_MACRAW) {
        if (((uint32_t)xTaskGetTickCount() - t0) > pdMS_TO_TICKS(100)) {
            return ERR_IF;
        }
        vTaskDelay(1);
    }

    for (q = p; q != NULL; q = q->next) {
        wiz_send_data(MACRAW_SN, (uint8_t *)q->payload, q->len);
    }
    setSn_CR(MACRAW_SN, Sn_CR_SEND);

    uint32_t t1 = (uint32_t)xTaskGetTickCount();
    while (getSn_CR(MACRAW_SN)) {
        if (((uint32_t)xTaskGetTickCount() - t1) > pdMS_TO_TICKS(100)) {
            return ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }

    uint8_t ir = getSn_IR(MACRAW_SN);
    if (ir & Sn_IR_SENDOK) {
        setSn_IR(MACRAW_SN, Sn_IR_SENDOK);
    } else if (ir & Sn_IR_TIMEOUT) {
        setSn_IR(MACRAW_SN, Sn_IR_TIMEOUT);
        LOG_WRN("MACRAW TX: timeout");
        return ERR_TIMEOUT;
    }
    return ERR_OK;
}

static struct pbuf *low_level_input(void)
{
    uint16_t rsr = getSn_RX_RSR(MACRAW_SN);
    if (rsr < 2u) {
        return NULL;
    }

    /* 一次性读取整个帧 (2-byte header + payload) 到临时缓冲区。
     * MACRAW 帧最大 1514B (不含 header), 加 2B header = 1516B。
     * 用栈上缓冲区一次性读取避免 wiz_recv_data 分次读导致指针错位。 */
    uint8_t buf[1520];
    uint16_t toread = (rsr > sizeof(buf)) ? (uint16_t)sizeof(buf) : rsr;
    wiz_recv_data(MACRAW_SN, buf, toread);
    setSn_CR(MACRAW_SN, Sn_CR_RECV);

    /* 解析 2-byte big-endian length header */
    uint16_t hdr = (uint16_t)((buf[0] << 8) | buf[1]);
    uint16_t framelen = hdr - 2;

    if (framelen == 0 || framelen > 32000 || (2 + framelen) > toread) {
        LOG_WRN("MACRAW RX: bad hdr=0x%04x rsr=%u read=%u", hdr, rsr, toread);
        return NULL;
    }

    struct pbuf *p = pbuf_alloc(PBUF_RAW, framelen, PBUF_POOL);
    if (p == NULL) {
        LOG_WRN("MACRAW RX: pbuf alloc fail (%u)", framelen);
        return NULL;
    }

    struct pbuf *q;
    uint16_t offset = 2; /* skip header */
    for (q = p; q != NULL; q = q->next) {
        memcpy(q->payload, &buf[offset], q->len);
        offset += q->len;
    }
    return p;
}

/* ==================== EXTI interrupt handler ==================== */

void EXTI1_IRQHandler(void)
{
    if (__HAL_GPIO_EXTI_GET_IT(W5500_INT_PIN) != RESET) {
        __HAL_GPIO_EXTI_CLEAR_IT(W5500_INT_PIN);
        BaseType_t wake = pdFALSE;
        if (rx_sem != NULL) {
            xSemaphoreGiveFromISR(rx_sem, &wake);
            portYIELD_FROM_ISR(wake);
        }
    }
}

/* ==================== RX task ==================== */

static void macraw_rx_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* 轮询模式: 每 10ms 检查 W5500 INT 引脚 (PD1, 低电平有效)
         * 和 socket RX 缓冲区。比 EXTI 更可靠 (避免中断优先级问题)。 */
        vTaskDelay(pdMS_TO_TICKS(10));

        /* 检查是否有数据在 MACRAW socket 缓冲区 */
        uint16_t rsr = getSn_RX_RSR(MACRAW_SN);
        if (rsr == 0) {
            continue;
        }

        for (;;) {
            struct pbuf *p = low_level_input();
            if (p == NULL) {
                break;
            }
            LOG_INF("MACRAW RX: frame %u bytes", p->tot_len);
            macraw_rx_total++;
            if (p->tot_len >= 14) {
                uint16_t ethertype = ((uint8_t *)p->payload)[12] << 8 |
                                     ((uint8_t *)p->payload)[13];
                if (ethertype == 0x0800) macraw_rx_ipv4++;
                else if (ethertype == 0x0806) macraw_rx_arp++;
                else macraw_rx_other++;
            }
            if (w5500_netif.input(p, &w5500_netif) != ERR_OK) {
                pbuf_free(p);
            }
        }
    }
}

/* ==================== Public API ==================== */

bool w5500_macraw_init(const uint8_t mac[6])
{
    ip4_addr_t ip_addr, netmask, gw;
    (void)mac;

    rx_sem = xSemaphoreCreateCountingStatic(0xFFFF, 0, &rx_sem_cb);
    if (rx_sem == NULL) {
        LOG_ERR("MACRAW: semaphore failed");
        return false;
    }

    /* EXTI on PD1 (falling edge = W5500 INT) */
    GPIO_InitTypeDef io = {0};
    io.Pin = W5500_INT_PIN;
    io.Mode = GPIO_MODE_IT_FALLING;
    io.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(W5500_INT_PORT, &io);
    HAL_NVIC_SetPriority(EXTI1_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(EXTI1_IRQn);

    IP4_ADDR(&ip_addr,
             (uint8_t)get_holding_reg(HOLDING_IP_OCTET1_IDX),
             (uint8_t)get_holding_reg(HOLDING_IP_OCTET2_IDX),
             (uint8_t)get_holding_reg(HOLDING_IP_OCTET3_IDX),
             (uint8_t)get_holding_reg(HOLDING_IP_OCTET4_IDX));
    {
        uint32_t ip32 = ip4_addr_get_u32(&ip_addr);
        gw.addr = (ip32 & 0x00FFFFFF) | (1u << 24);
    }
    IP4_ADDR(&netmask, 255, 255, 255, 0);

    low_level_init(&w5500_netif);

    if (!netif_add(&w5500_netif, &ip_addr, &netmask, &gw,
                    NULL, macraw_netif_init, tcpip_input)) {
        LOG_ERR("MACRAW: netif_add failed");
        return false;
    }
    netif_set_default(&w5500_netif);
    netif_set_up(&w5500_netif);
    LOG_INF("MACRAW: netif up, IP %s", ip4addr_ntoa(&ip_addr));

    xTaskCreateStatic(macraw_rx_task, "macraw_rx", MACRAW_RX_STACK,
                      NULL, MACRAW_RX_PRIO, rx_stack, &rx_tcb);
    return true;
}

bool w5500_macraw_link_up(void)
{
    return netif_is_link_up(&w5500_netif);
}
