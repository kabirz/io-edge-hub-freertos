/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN1 业务通道 (PA11 RX / PA12 TX, AF9)
 *
 * Zephyr 版 src/can.c + libs/can_fw_upgrade/can_fw_upgrade.c 初始化部分的
 * FreeRTOS 移植:
 *   - 波特率 = holding_reg[0x07] (reg 值即 kbps, 默认 250) 查位时序表
 *     (CAN 时钟 = PCLK1 = 42MHz), SJW=1; 非法值 (含 800k) 回落 250k
 *   - RX: 过滤器组 0 / FIFO0 / 16 位标识符掩码模式, 两组半槽:
 *     半槽 A 精确匹配业务 ID (holding_reg[0x06], 默认 0x0111),
 *     半槽 B 接收 0x100-0x1FF 段 (固件升级协议 0x101-0x107);
 *     命中帧经 can_set_rx_hook 注入消费者 (fw_can_frame_isr -> 队列
 *     -> fw 任务, 对齐 Zephyr can_fw_upgrade 库 msgq + RX 线程),
 *     未注入时静默丢弃 + 计数
 *   - TX: mod_can_send() 发送 API (现无调用者, 现版固件无周期推送)
 *   - 波特率/ID 启动快照: 运行期写寄存器只存不生效, 重启后经
 *     config_store 应用 (与 RS485/Modbus 从站号同语义)
 *   - NART=0 自动重传 (HAL AutoRetransmission=ENABLE, 对齐 Zephyr app
 *     域默认; ONE_SHOT 仅 Zephyr boot 域使用); HAL 默认不开错误类中断
 *     (IER 仅 FMPIE0), 无 Zephyr 侧需手动屏蔽的错误中断风暴问题
 *
 * 偏差记录 (设计文档已确认):
 *   - 800kbps 不可实现: 42MHz/800k = 52.5 tq 非整数, 无整数分频组合,
 *     reg 0x07=800 视为非法回落 250k
 */

#include <string.h>

#include "main.h"

#include "io_can.h"
#include "init.h"
#include "log.h"

/* ==================== 板级定义 ==================== */

#define CAN_IRQ_PRIO 6u /* >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY;
			 * 本 ISR 无 FreeRTOS 调用, 取值仅对齐 USART2 */

/* ==================== 位时序表 ==================== */

/* CAN 时钟 = APB1 = 42MHz; 波特率 = 42MHz / (PSC x (1 + BS1 + BS2)),
 * 每行总位数整数整除 (采样点 85.7%-90%) */
static const struct {
    uint16_t kbps;
    uint16_t presc;
    uint32_t bs1; /* CAN_BS1_xTQ 宏值 */
    uint32_t bs2; /* CAN_BS2_xTQ 宏值 */
} can_timing_table[] = {
    {50, 105, CAN_BS1_6TQ, CAN_BS2_1TQ}, /* 42M/(105*8) = 50k,  87.5% */
    {100, 42, CAN_BS1_8TQ, CAN_BS2_1TQ}, /* 42M/(42*10)  = 100k, 90.0% */
    {125, 42, CAN_BS1_6TQ, CAN_BS2_1TQ}, /* 42M/(42*8)   = 125k, 87.5% */
    {250, 21, CAN_BS1_6TQ, CAN_BS2_1TQ}, /* 42M/(21*8)   = 250k, 87.5% */
    {500, 12, CAN_BS1_5TQ, CAN_BS2_1TQ}, /* 42M/(12*7)   = 500k, 85.7% */
    {1000, 6, CAN_BS1_5TQ, CAN_BS2_1TQ}, /* 42M/(6*7)    = 1M,   85.7% */
};

#define CAN_BAUDRATE_FALLBACK 250u /* 非法/不支持值的回落档 */

/* ==================== 状态 ==================== */

CAN_HandleTypeDef hcan1;

static bool started;       /* can_start 成功完成 */
static uint32_t can_rx_frames; /* 静默消费计数 (仅 ISR 写) */

/* RX 帧消费者注入 (fw_can_frame_isr); 须在 can_start() 前注册。
 * ISR 上下文调用, 消费者自行保证 ISR 安全 (入队)。 */
static void (*can_rx_hook)(uint32_t id, const uint8_t *data, uint8_t dlc);

void can_set_rx_hook(void (*fn)(uint32_t id, const uint8_t *data, uint8_t dlc))
{
    can_rx_hook = fn;
}

/* ==================== 发送 (任务上下文) ==================== */

int mod_can_send(uint32_t id, const uint8_t *data, uint8_t len)
{
    CAN_TxHeaderTypeDef hdr = {0};
    uint8_t buf[8] = {0}; /* HAL 固定读 8 字节, 不足补零 (对齐 Zephyr
			   * can_frame 零初始化语义) */
    uint32_t mailbox;
    uint32_t start;

    if (!started || len > 8 || (data == NULL && len > 0)) {
        return -1;
    }
    memcpy(buf, data, len);

    hdr.StdId = id & 0x7FFu; /* 11 位标准 ID (超出位硬件按位域截断) */
    hdr.IDE = CAN_ID_STD;
    hdr.RTR = CAN_RTR_DATA;
    hdr.DLC = len;
    hdr.TransmitGlobalTime = DISABLE;

    /* 空闲邮箱等待 <=100ms (对齐 Zephyr can_send K_MSEC(100)):
     * 忙等, 不假设调用上下文可睡眠 */
    start = HAL_GetTick();
    while (HAL_CAN_GetTxMailboxesFreeLevel(&hcan1) == 0U) {
        if (HAL_GetTick() - start > 100U) {
            LOG_WRN("can tx: no free mailbox (100ms)");
            return -1;
        }
    }

    return HAL_CAN_AddTxMessage(&hcan1, &hdr, buf, &mailbox) == HAL_OK ? 0 : -1;
}

/* ==================== RX 静默消费 (ISR 上下文) ==================== */

/* HAL 弱回调重载: FIFO0 消息挂起 (CAN1_RX0_IRQ)。读出注入消费者
 * (无消费者时丢弃+计数); FMP0>0 期间中断持续重入直至排空 */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    static CAN_RxHeaderTypeDef rx_hdr;
    static uint8_t rx_data[8];

    if (hcan->Instance != CAN1) {
        return;
    }
    (void)HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &rx_hdr, rx_data);
    if (can_rx_hook != NULL && rx_hdr.IDE == CAN_ID_STD) {
        can_rx_hook(rx_hdr.StdId, rx_data, rx_hdr.DLC);
    } else {
        can_rx_frames++;
    }
}

/* ==================== 中断向量 ==================== */

void CAN1_RX0_IRQHandler(void)
{
    HAL_CAN_IRQHandler(&hcan1);
}

/* ==================== 初始化 ==================== */

/* CAN1 引脚/时钟/中断 (HAL_CAN_Init 回调; 仅管 CAN1) */
void HAL_CAN_MspInit(CAN_HandleTypeDef *hcan)
{
    GPIO_InitTypeDef io = {0};

    if (hcan->Instance != CAN1) {
        return;
    }

    __HAL_RCC_CAN1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* PA11 RX / PA12 TX, AF9; CAN 收发器驱动电平, 无上下拉 */
    io.Pin = GPIO_PIN_11 | GPIO_PIN_12;
    io.Mode = GPIO_MODE_AF_PP;
    io.Pull = GPIO_NOPULL;
    io.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    io.Alternate = GPIO_AF9_CAN1;
    HAL_GPIO_Init(GPIOA, &io);

    HAL_NVIC_SetPriority(CAN1_RX0_IRQn, CAN_IRQ_PRIO, 0);
    HAL_NVIC_EnableIRQ(CAN1_RX0_IRQn);
}

void can_start(void)
{
    uint32_t kbps = get_holding_reg(HOLDING_CAN_BAUDRATE_IDX);
    uint16_t id = get_holding_reg(HOLDING_CAN_ID_IDX) & 0x7FFu;
    const CAN_FilterTypeDef filt = {
        /* 16 位标识符掩码模式: FR1 = (MaskLow<<16)|IdLow,
         * FR2 = (MaskHigh<<16)|IdHigh, 两组各为独立的 16 位 id/mask
         * 过滤器, 帧命中任一组即入 FIFO0 (HAL 源码
         * stm32f4xx_hal_can.c 16BIT 分支实证)。标准 ID 半字布局
         * = STID[10:0]<<5:
         *   半槽 A (FR1) = (id<<5, 0x7FF<<5) 精确匹配业务 ID;
         *   半槽 B (FR2) = (0x100<<5, 0x700<<5) 接收 0x100-0x1FF 段
         *     (固件升级协议 0x101-0x107; 业务 ID 可配到段外)。
         * 掩码 RTR/IDE 位为 0 = don't-care (16 位刻度下半字
         * bit4=RTR/bit3=IDE), 远端帧按 STID 同判通过 */
        .FilterIdHigh = 0x100u << 5,
        .FilterIdLow = (uint32_t)id << 5,
        .FilterMaskIdHigh = 0x700u << 5,
        .FilterMaskIdLow = 0x7FFu << 5,
        .FilterFIFOAssignment = CAN_FILTER_FIFO0,
        .FilterBank = 0,
        .FilterMode = CAN_FILTERMODE_IDMASK,
        .FilterScale = CAN_FILTERSCALE_16BIT,
        .FilterActivation = ENABLE,
        .SlaveStartFilterBank = 14, /* F407 双 CAN: 0-13 归 CAN1,
					     * 仅用组 0, 值取 CubeMX 惯例 */
    };
    size_t i;
    const uint8_t n = sizeof(can_timing_table) / sizeof(can_timing_table[0]);
    const uint32_t bs1_default = CAN_BS1_6TQ;
    uint32_t presc = 21; /* 250k 档 */
    uint32_t bs1 = bs1_default;
    uint32_t bs2 = CAN_BS2_1TQ;
    bool found = false;

    /* 波特率查表: reg 值即 kbps (x1000); 未命中 (0/非法/800k) 回落 250k */
    for (i = 0; i < n; i++) {
        if (can_timing_table[i].kbps == kbps) {
            presc = can_timing_table[i].presc;
            bs1 = can_timing_table[i].bs1;
            bs2 = can_timing_table[i].bs2;
            found = true;
            break;
        }
    }
    if (!found) {
        LOG_WRN("can baud %uk unsupported, fallback %uk",
                (unsigned)kbps, CAN_BAUDRATE_FALLBACK);
        kbps = CAN_BAUDRATE_FALLBACK;
    }

    hcan1.Instance = CAN1;
    hcan1.Init.Prescaler = presc;
    hcan1.Init.Mode = CAN_MODE_NORMAL;
    hcan1.Init.SyncJumpWidth = CAN_SJW_1TQ;
    hcan1.Init.TimeSeg1 = bs1;
    hcan1.Init.TimeSeg2 = bs2;
    hcan1.Init.TimeTriggeredMode = DISABLE;
    hcan1.Init.AutoBusOff = ENABLE;      /* ABOM: bus-off 自动恢复 */
    hcan1.Init.AutoWakeUp = DISABLE;
    hcan1.Init.AutoRetransmission = ENABLE; /* NART=0, 对齐 Zephyr app 域 */
    hcan1.Init.ReceiveFifoLocked = DISABLE; /* FIFO 满时新帧覆盖旧帧 */
    hcan1.Init.TransmitFifoPriority = DISABLE;
    if (HAL_CAN_Init(&hcan1) != HAL_OK) {
        LOG_ERR("can init failed");
        return;
    }

    if (HAL_CAN_ConfigFilter(&hcan1, &filt) != HAL_OK) {
        LOG_ERR("can filter config failed");
        return;
    }
    if (HAL_CAN_Start(&hcan1) != HAL_OK) {
        /* 总线被显性电平占据 (典型: 上位机波特率失配) 时启动失败,
         * 不致命 (对齐 Zephyr can_fw_upgrade: 仅损失 CAN 功能) */
        LOG_ERR("can start failed (bus busy?)");
        return;
    }
    if (HAL_CAN_ActivateNotification(&hcan1, CAN_IT_RX_FIFO0_MSG_PENDING)
        != HAL_OK) {
        LOG_ERR("can rx notification failed");
        return;
    }

    started = true;
    LOG_INF("can up: %ukbps, bus id=0x%03x", (unsigned)kbps, (unsigned)id);
}
