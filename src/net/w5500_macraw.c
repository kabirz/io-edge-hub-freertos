/*
 * W5500 MACRAW netif driver for LwIP.
 *
 * Socket 0 MACRAW, 16KB RX/TX。收包时序与 ioLibrary recvfrom()
 * SOCK_MACRAW 分支一致 (读 2B 长度头 -> RECV+等待 -> 读数据 ->
 * RECV+等待); 坏头 (>1514) close 重开恢复同步。
 * Sn_MR = MACRAW|MFEN|MMB: 只收广播+自身单播 (屏蔽多播风暴),
 * netif->hwaddr 必须与 SHAR 同源。socket 0 命令序列由驱动级互斥
 * 保护 -- tcpip 线程 (SEND) 与 rx 任务 (RECV) 并发写 Sn_CR 会互相覆盖。
 */

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "main.h"
#include "spi.h"
#include "wizchip_conf.h"
#include "socket.h"     /* socket, close, SF_ETHER_OWN, SF_MULTI_BLOCK */
#include "w5500_macraw.h"

#include "init.h"      /* get_holding_reg, HOLDING_IP_OCTET*_IDX */

#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"

#include "log.h"

/* W5500 MACRAW socket number (fixed to 0) */
#define MACRAW_SN  0

/* EXTI interrupt pin: PD1 (W5500 INT) */
#define W5500_INT_PORT  GPIOD
#define W5500_INT_PIN   GPIO_PIN_1

/* 合法以太网帧长上限 (ioLibrary SOCKFATAL_PACKLEN 同值) */
#define MACRAW_MAX_FRAME  1514

/* RX task config */
#define MACRAW_RX_PRIO    5
#define MACRAW_RX_STACK   512  /* 2048 bytes */
/* 每轮轮询处理的帧数上限: 防止持续流量下高优先级收包饿死 tcpip 线程 */
#define MACRAW_RX_BURST   32

/* ==================== Forward declarations ==================== */
static err_t low_level_output(struct netif *netif, struct pbuf *p);

/* ==================== Static state ==================== */
static struct netif w5500_netif;
static bool netif_added;   /* netif_add 成功后才允许 set_link */
static SemaphoreHandle_t rx_sem;
static StaticSemaphore_t rx_sem_cb;
/* socket 0 命令序列互斥 (TX/RX 两线程) */
static SemaphoreHandle_t spi_lock;
static StaticSemaphore_t spi_lock_cb;
static StackType_t rx_stack[MACRAW_RX_STACK];
static StaticTask_t rx_tcb;

/* Debug counters (RAM 探针, ST-LINK 可读) */
volatile uint32_t macraw_rx_total;
volatile uint32_t macraw_rx_ipv4;
volatile uint32_t macraw_rx_arp;
volatile uint32_t macraw_rx_other;
volatile uint32_t macraw_rx_badhdr;
volatile uint32_t macraw_rx_nobuf;

/* ==================== Socket lock helpers ==================== */

static void macraw_lock(void)
{
    if (spi_lock != NULL) {
        xSemaphoreTake(spi_lock, portMAX_DELAY);
    }
}

static void macraw_unlock(void)
{
    if (spi_lock != NULL) {
        xSemaphoreGive(spi_lock);
    }
}

/* RECV 命令 + 等待处理完成 (不等待会与芯片内部指针更新竞争) */
static void macraw_recv_commit(void)
{
    setSn_CR(MACRAW_SN, Sn_CR_RECV);
    while (getSn_CR(MACRAW_SN) != 0) {
    }
}

/* 重开 MACRAW socket (须持有 spi_lock); 用于初始化和失步恢复 */
static void macraw_open_socket(void)
{
    close(MACRAW_SN);
    socket(MACRAW_SN, Sn_MR_MACRAW, 0, SF_ETHER_OWN | SF_MULTI_BLOCK);
}

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
    netif->hwaddr_len = ETHARP_HWADDR_LEN;

    macraw_lock();
    macraw_open_socket();
    uint8_t sr = getSn_SR(MACRAW_SN);
    uint8_t mr = getSn_MR(MACRAW_SN);
    macraw_unlock();

    LOG_INF("MACRAW: SR=0x%02x MR=0x%02x (expect 0x42 / 0xA4)", sr, mr);
    if (sr != SOCK_MACRAW) {
        LOG_ERR("MACRAW: socket open failed (SR=0x%02x)", sr);
        return;
    }
    LOG_INF("MACRAW: socket 0 open, 16KB RX/TX, filter=own+bcast");
}

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
    struct pbuf *q;
    (void)netif;

    macraw_lock();

    /* Wait for socket ready */
    uint32_t t0 = (uint32_t)xTaskGetTickCount();
    while (getSn_SR(MACRAW_SN) != SOCK_MACRAW) {
        if (((uint32_t)xTaskGetTickCount() - t0) > pdMS_TO_TICKS(100)) {
            macraw_unlock();
            return ERR_IF;
        }
        macraw_unlock();
        vTaskDelay(1);
        macraw_lock();
    }

    for (q = p; q != NULL; q = q->next) {
        wiz_send_data(MACRAW_SN, (uint8_t *)q->payload, q->len);
    }
    setSn_CR(MACRAW_SN, Sn_CR_SEND);

    uint32_t t1 = (uint32_t)xTaskGetTickCount();
    while (getSn_CR(MACRAW_SN) != 0) {
        if (((uint32_t)xTaskGetTickCount() - t1) > pdMS_TO_TICKS(100)) {
            macraw_unlock();
            return ERR_TIMEOUT;
        }
    }

    uint8_t ir = getSn_IR(MACRAW_SN);
    if (ir & Sn_IR_SENDOK) {
        setSn_IR(MACRAW_SN, Sn_IR_SENDOK);
    } else if (ir & Sn_IR_TIMEOUT) {
        setSn_IR(MACRAW_SN, Sn_IR_TIMEOUT);
        macraw_unlock();
        LOG_WRN("MACRAW TX: timeout");
        return ERR_TIMEOUT;
    }
    macraw_unlock();
    return ERR_OK;
}

/*
 * 读取一帧。返回 pbuf (交给 netif->input), 无数据或异常恢复后返回 NULL。
 * 时序与 ioLibrary recvfrom() SOCK_MACRAW 分支一致。
 */
static struct pbuf *low_level_input(void)
{
    uint16_t rsr = getSn_RX_RSR(MACRAW_SN);
    if (rsr < 2u) {
        return NULL;
    }

    macraw_lock();

    /* Step 1: 读 2 字节 MACRAW 长度头, 立即 RECV 提交并等待 */
    uint8_t hdr_buf[2];
    wiz_recv_data(MACRAW_SN, hdr_buf, 2);
    macraw_recv_commit();

    uint16_t hdr = (uint16_t)((hdr_buf[0] << 8) | hdr_buf[1]);
    uint16_t framelen = hdr - 2; /* 长度头包含自身 2 字节 */

    if (framelen == 0 || framelen > MACRAW_MAX_FRAME) {
        macraw_rx_badhdr++; /* 指针失步: close 重开恢复 */
        LOG_WRN("MACRAW RX: bad hdr=0x%04x rsr=%u, reopen", hdr, rsr);
        macraw_open_socket();
        macraw_unlock();
        return NULL;
    }

    /* Step 2: 读帧数据 (pbuf 不足则丢帧) 再 RECV 提交 */
    struct pbuf *p = pbuf_alloc(PBUF_RAW, framelen, PBUF_POOL);
    if (p == NULL) {
        macraw_rx_nobuf++;
        wiz_recv_ignore(MACRAW_SN, framelen);
        macraw_recv_commit();
        macraw_unlock();
        return NULL;
    }

    struct pbuf *q;
    for (q = p; q != NULL; q = q->next) {
        wiz_recv_data(MACRAW_SN, (uint8_t *)q->payload, q->len);
    }
    macraw_recv_commit();

    macraw_unlock();

    macraw_rx_total++;
    if (p->tot_len >= 14) {
        uint16_t ethertype = ((uint8_t *)p->payload)[12] << 8 |
                             ((uint8_t *)p->payload)[13];
        if (ethertype == 0x0800) {
            macraw_rx_ipv4++;
        } else if (ethertype == 0x0806) {
            macraw_rx_arp++;
        } else {
            macraw_rx_other++;
        }
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
        vTaskDelay(pdMS_TO_TICKS(10)); /* 轮询 (EXTI 在当前板上不可靠) */

        if (getSn_RX_RSR(MACRAW_SN) == 0) {
            continue;
        }

        uint32_t budget = MACRAW_RX_BURST;
        while (budget-- > 0) {
            struct pbuf *p = low_level_input();
            if (p == NULL) {
                break;
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

    /* 互斥先建: 后续所有 socket 0 访问都在保护下 */
    spi_lock = xSemaphoreCreateMutexStatic(&spi_lock_cb);

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

    /* hwaddr 与 w5500_net_init 写入 SHAR 的 mac 同源 (MFEN 按 SHAR 过滤) */
    memcpy(w5500_netif.hwaddr, mac, 6);

    low_level_init(&w5500_netif);

    /* netif RAW API 须持 core lock; ip4_route 要求 link-up 才路由,
     * 之后由 net_mon_task 边沿调用 set_link 更新 */
    LOCK_TCPIP_CORE();
    bool link_now = ((getPHYCFGR() & PHYCFGR_LNK_ON) != 0);
    if (!netif_add(&w5500_netif, &ip_addr, &netmask, &gw,
                    NULL, macraw_netif_init, tcpip_input)) {
        UNLOCK_TCPIP_CORE();
        LOG_ERR("MACRAW: netif_add failed");
        return false;
    }
    netif_set_default(&w5500_netif);
    netif_set_up(&w5500_netif);
    netif_added = true;
    if (link_now) {
        netif_set_link_up(&w5500_netif);
    }
    UNLOCK_TCPIP_CORE();
    LOG_INF("MACRAW: netif up, IP %s link=%s", ip4addr_ntoa(&ip_addr),
            link_now ? "up" : "down");

    xTaskCreateStatic(macraw_rx_task, "macraw_rx", MACRAW_RX_STACK,
                      NULL, MACRAW_RX_PRIO, rx_stack, &rx_tcb);
    return true;
}

bool w5500_macraw_link_up(void)
{
    return netif_is_link_up(&w5500_netif);
}

bool w5500_macraw_get_mac(uint8_t mac[6])
{
    if (!netif_added) {
        return false;
    }
    memcpy(mac, w5500_netif.hwaddr, 6);
    return true;
}

/* netif_set_link_* 须持 core lock: net_mon_task 经 tcpip_thread 执行 */
static void macraw_set_link_cb(void *arg)
{
    bool up = ((uintptr_t)arg != 0);

    if (up && !netif_is_link_up(&w5500_netif)) {
        netif_set_link_up(&w5500_netif);
    } else if (!up && netif_is_link_up(&w5500_netif)) {
        netif_set_link_down(&w5500_netif);
    }
}

void w5500_macraw_set_link(bool up)
{
    if (!netif_added) {
        return;
    }
    tcpip_callback(macraw_set_link_cb, (void *)(uintptr_t)(up ? 1u : 0u));
}
