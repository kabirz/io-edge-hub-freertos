/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modbus RTU 传输层 (target-only): USART2 PA2(TX)/PA3(RX) 8N1 +
 * RS485 DE PA1 (推挽输出, 空闲低)。
 *
 *   - RX: HAL_UARTEx_ReceiveToIdle_IT —— IDLE 事件(帧内 >=1 字符静默,
 *     < t3.5)回调里 rtu_rx_feed() 喂帧状态机并重启接收; 状态机内部经
 *     rtu_t35_kick() (本文件强符号, ISR 上下文 xTimerResetFromISR)
 *     重启 t3.5 单次软件定时器。
 *   - t3.5 到期: 定时器服务任务(prio 4)回调里 xTaskNotifyGive 唤醒
 *     RTU 任务(prio 5) -> rtu_t35_expired() 解帧。软件定时器回调运行
 *     于任务上下文而非 ISR, 须用非 FromISR 版本。
 *   - TX (rtu_tx_frame, RTU 任务上下文): 先 HAL_UART_AbortReceive
 *     抑制自发回环 (RS485 收发器 RE 常使能时会收到自己发出的帧),
 *     DE 拉高 + ~20us 建立时间后 HAL_UART_Transmit_IT; TxCplt 回调
 *     (USART2 判定)里 DE 拉低并重启接收。
 *   - 帧处理期间 (frame_pending) 新到的 RX 事件丢弃 (处理中字节不入帧)。
 *   - USART2 IRQ 优先级 6 >= configLIBRARY_MAX_SYSCALL_INTERRUPT_
 *     PRIORITY(5): 回调内 FromISR 调用合法。
 *   - 波特率/从站号启动时读 reg 0x08/0x09 固定, 运行期写只存不生效
 *     (重启后经 config_store 生效)。
 */

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

#include "main.h"

#include "rtu_frame.h"
#include "init.h"

#include "log.h"

/* ==================== 板级定义 ==================== */

#define RTU_UART           USART2
#define RTU_UART_IRQ       USART2_IRQn
#define RTU_IRQ_PRIO       6u   /* >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY */
#define RTU_DE_PORT        GPIOA
#define RTU_DE_PIN         GPIO_PIN_1
#define RTU_DE_SETUP_LOOPS 2000u /* DE 建立时间 (~30us @168MHz, 先于起始位) */

/* 栈 1024 字 = 4096B: FC06 写 reg 0x10/0x11 在本任务内执行
 * config_store -> SPI 保存 (+history_sync), 调用链与 Modbus TCP
 * 任务一致 (该任务同为 1024 字), 原 512 字余量不足 */
#define RTU_TASK_STACK     1024u /* 字 */
#define RTU_TASK_PRIO      5u

/* 帧状态机缓冲同为 256B, 此处只做字节搬运 */
#define RTU_RX_BUF_SIZE    256u

/* ==================== 状态 ==================== */

UART_HandleTypeDef huart2;

static uint8_t rtu_rx_buf[RTU_RX_BUF_SIZE];

static StackType_t rtu_stack[RTU_TASK_STACK];
static StaticTask_t rtu_tcb;
static TaskHandle_t rtu_task_handle;

static StaticTimer_t rtu_t35_tcb;
static TimerHandle_t rtu_t35_timer;

static volatile bool tx_busy;       /* DE 置高 -> TxCplt 之间 */
static volatile bool frame_pending; /* t3.5 已到期, 处理期间丢弃 RX 事件 */
static bool started;

/* ==================== t3.5 定时钩子 (ISR 上下文) ==================== */

/* rtu_rx_feed 内部调用 (USART2 ISR); 强符号覆盖 rtu_frame.c 弱默认 */
void rtu_t35_kick(void)
{
	BaseType_t woken = pdFALSE;

	if (rtu_t35_timer != NULL) {
		xTimerResetFromISR(rtu_t35_timer, &woken);
		portYIELD_FROM_ISR(woken);
	}
}

/* ==================== 帧处理任务 ==================== */

static void rtu_task(void *arg)
{
	(void)arg;

	for (;;) {
		(void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		rtu_t35_expired();
		if (!tx_busy) {
		/* 无应答: 立即恢复接收; 有应答时由 TxCplt 收尾清除 */
			frame_pending = false;
		}
	}
}

static void rtu_t35_timer_cb(TimerHandle_t timer)
{
	(void)timer;

	if (rtu_task_handle != NULL) {
		/* 软件定时器回调运行于定时器服务任务 (非 ISR):
		 * xTaskNotifyGive 而非 FromISR 版本 */
		frame_pending = true;
		xTaskNotifyGive(rtu_task_handle);
	}
}

/* ==================== 发送 (RTU 任务上下文) ==================== */

static void rtu_tx_restart_rx(void)
{
	(void)HAL_UARTEx_ReceiveToIdle_IT(&huart2, rtu_rx_buf,
					  RTU_RX_BUF_SIZE);
}

static void rtu_tx_finish(void)
{
	/* TC = 移位寄存器已吐完最后一位, 此时释放 DE 安全 */
	HAL_GPIO_WritePin(RTU_DE_PORT, RTU_DE_PIN, GPIO_PIN_RESET);
	tx_busy = false;
	rtu_tx_restart_rx();
	frame_pending = false;
}

/* rtu_frame_bind 注册的 tx 回调: 输出完整应答帧 */
static void rtu_tx_frame(const uint8_t *frame, uint16_t len)
{
	tx_busy = true;
	/* 抑制自发回环: 应答期间不收 */
	(void)HAL_UART_AbortReceive(&huart2);

	HAL_GPIO_WritePin(RTU_DE_PORT, RTU_DE_PIN, GPIO_PIN_SET);
	for (volatile uint32_t i = 0; i < RTU_DE_SETUP_LOOPS; i++) {
		__NOP(); /* DE 建立 (收发器使能最大 us 级) 先于起始位 */
	}

	if (HAL_UART_Transmit_IT(&huart2, (uint8_t *)frame, len) != HAL_OK) {
		LOG_WRN("rtu tx start failed");
		rtu_tx_finish(); /* 启动失败: 释放总线, 恢复接收 */
	}
}

/* ==================== HAL 回调 (USART2 判定) ==================== */

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
	if (huart->Instance != RTU_UART) {
		return;
	}
	rtu_tx_finish();
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
	if (huart->Instance != RTU_UART) {
		return;
	}
	if (!frame_pending && Size > 0) {
		/* 内部经 rtu_t35_kick 重启 t3.5 (FromISR) */
		rtu_rx_feed(rtu_rx_buf, Size);
	}
	rtu_tx_restart_rx();
}

/* ReceiveToIdle_IT 的 IT 接收遇错 (噪声/帧错/溢出) 即终止: 不重启则
 * RTU 静默死亡。此处重启接收让残帧走 CRC/长度丢弃路径自恢复; TX 出错
 * (TxCplt 不会再来) 同样走收尾释放 DE, 避免总线被钳死在发送态。 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
	if (huart->Instance != RTU_UART || started == false) {
		return;
	}
	if (tx_busy) {
		rtu_tx_finish();
	} else {
		rtu_tx_restart_rx();
	}
}

/* ==================== USART2 中断向量 ==================== */

void USART2_IRQHandler(void)
{
	HAL_UART_IRQHandler(&huart2);
}

/* ==================== 初始化 ==================== */

/* USART2 引脚/时钟/中断 (HAL_UART_Init 回调; 仅管 USART2) */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
	GPIO_InitTypeDef io = {0};

	if (huart->Instance != RTU_UART) {
		return;
	}

	__HAL_RCC_USART2_CLK_ENABLE();
	__HAL_RCC_GPIOA_CLK_ENABLE();

	/* PA2 TX / PA3 RX, AF7; RX 上拉保证总线空闲为 mark 电平 */
	io.Pin = GPIO_PIN_2 | GPIO_PIN_3;
	io.Mode = GPIO_MODE_AF_PP;
	io.Pull = GPIO_PULLUP;
	io.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	io.Alternate = GPIO_AF7_USART2;
	HAL_GPIO_Init(GPIOA, &io);

	HAL_NVIC_SetPriority(RTU_UART_IRQ, RTU_IRQ_PRIO, 0);
	HAL_NVIC_EnableIRQ(RTU_UART_IRQ);
}

static void rtu_de_init(void)
{
	GPIO_InitTypeDef io = {0};

	io.Pin = RTU_DE_PIN;
	io.Mode = GPIO_MODE_OUTPUT_PP;
	io.Pull = GPIO_NOPULL;
	io.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(RTU_DE_PORT, &io);
	HAL_GPIO_WritePin(RTU_DE_PORT, RTU_DE_PIN, GPIO_PIN_RESET); /* 空闲低 */
}

void mb_rtu_start(void)
{
	uint32_t baud;
	uint8_t srv_unit;

	if (started) {
		return; /* 幂等 */
	}
	started = true;

	/* 启动时固定 (reg 0x08 默认 9600 / reg 0x09 默认 1); 运行期写
	 * 只存不生效 */
	baud = get_holding_reg(HOLDING_RS485_BAUDRATE_IDX);
	/* 波特率兜底: 0 或超出 1200..115200 回落 9600 —— HAL 对 baud 0
	 * 初始化失败, 持久化损坏值不得阻断 UART 启动 */
	if (baud < 1200u || baud > 115200u) {
		LOG_WRN("rtu baud %u invalid, fallback 9600", (unsigned)baud);
		baud = 9600;
	}
	srv_unit = (uint8_t)get_holding_reg(HOLDING_SLAVE_ID_IDX);

	rtu_de_init();

	huart2.Instance = RTU_UART;
	huart2.Init.BaudRate = baud;
	huart2.Init.WordLength = UART_WORDLENGTH_8B;
	huart2.Init.StopBits = UART_STOPBITS_1;
	huart2.Init.Parity = UART_PARITY_NONE;
	huart2.Init.Mode = UART_MODE_TX_RX;
	huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	huart2.Init.OverSampling = UART_OVERSAMPLING_16;
	if (HAL_UART_Init(&huart2) != HAL_OK) {
		Error_Handler();
	}

	rtu_frame_bind(srv_unit, baud, rtu_tx_frame);

	/* t3.5 单次软件定时器: 每次喂数据重启 (rtu_t35_kick) */
	rtu_t35_timer = xTimerCreateStatic("rtu_t35",
					   pdMS_TO_TICKS(rtu_t35_ms(baud)),
					   pdFALSE, NULL, rtu_t35_timer_cb,
					   &rtu_t35_tcb);
	if (rtu_t35_timer == NULL) {
		Error_Handler();
	}

	rtu_task_handle = xTaskCreateStatic(rtu_task, "rtu",
					    RTU_TASK_STACK, NULL,
					    RTU_TASK_PRIO,
					    rtu_stack, &rtu_tcb);
	if (rtu_task_handle == NULL) {
		Error_Handler();
	}

	rtu_tx_restart_rx();
	LOG_INF("rtu slave up");
}
