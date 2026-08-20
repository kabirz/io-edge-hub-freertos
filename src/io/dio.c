/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 数字 IO: 16 路 DI 采集 + 8 路 DO 输出 + 8 路 LED 指示
 *   - Zephyr 版 src/io/dio.c 的 FreeRTOS 移植 (行为权威源):
 *     GPIO 表按 overlay /zephyr,user 硬编码 (顺序即通道号),
 *     DI 由独立任务周期采样 (周期 = holding_reg[0x03] 钳 [10,5000]ms),
 *     单次瞬时读、无消抖, 仅使能通道 (holding_reg[0x01] bitmap) 参与,
 *     结果写 input_reg[INPUT_DI_IDX]; 禁用通道读 0。
 *   - DO 由 mb_set_do() 驱动 (holding_reg[0x00] 写回调 / 网络断连安全
 *     清零), LED 无条件跟随 DO 状态 (对齐 Zephyr i<DO_NUM 循环 +
 *     i<ARRAY_SIZE(led_gpios) 镜像)。
 *   - DI 使能 (!=0) 时, 采样数据异步送历史记录 (send_history_data
 *     内部按历史开关决定是否落盘)。
 */

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "main.h"

#include "init.h"
#include "io.h"
#include "io_hooks.h" /* mb_set_do 声明 + io_now_epoch */
#include "history.h"
#include "log.h"

/* 采样间隔上限 5s: 业务合理性约束 (远程调大时钳制, 防止采样响应过慢) */
#define SAMPLE_INTERVAL_MAX 5000U

#define DIO_TASK_STACK 256u /* 字 */
#define DIO_TASK_PRIO  6u

/* 引脚描述 (overlay di-gpios/do-gpios/led-gpios, 顺序即通道号) */
struct dio_pin {
	GPIO_TypeDef *port;
	uint16_t pin;
};

/* DI1-16 (光耦输入, 下拉, 高有效) */
static const struct dio_pin di_pins[DI_NUM] = {
	{GPIOD, GPIO_PIN_3},  {GPIOD, GPIO_PIN_4},  {GPIOD, GPIO_PIN_5},
	{GPIOD, GPIO_PIN_6},  {GPIOB, GPIO_PIN_5},  {GPIOB, GPIO_PIN_6},
	{GPIOB, GPIO_PIN_7},  {GPIOB, GPIO_PIN_8},  {GPIOB, GPIO_PIN_9},
	{GPIOB, GPIO_PIN_10}, {GPIOB, GPIO_PIN_11}, {GPIOD, GPIO_PIN_2},
	{GPIOB, GPIO_PIN_0},  {GPIOB, GPIO_PIN_1},  {GPIOB, GPIO_PIN_3},
	{GPIOB, GPIO_PIN_4},
};

/* DO1-8 (推挽输出) + LED1-8 (推挽, 跟随 DO) */
static const struct dio_pin do_pins[DO_NUM] = {
	{GPIOD, GPIO_PIN_7},  {GPIOD, GPIO_PIN_8},  {GPIOD, GPIO_PIN_9},
	{GPIOD, GPIO_PIN_10}, {GPIOD, GPIO_PIN_11}, {GPIOD, GPIO_PIN_12},
	{GPIOD, GPIO_PIN_13}, {GPIOD, GPIO_PIN_14},
};
static const struct dio_pin led_pins[DO_NUM] = {
	{GPIOE, GPIO_PIN_8},  {GPIOE, GPIO_PIN_9},  {GPIOE, GPIO_PIN_10},
	{GPIOE, GPIO_PIN_11}, {GPIOE, GPIO_PIN_12}, {GPIOE, GPIO_PIN_13},
	{GPIOE, GPIO_PIN_14}, {GPIOE, GPIO_PIN_15},
};

static StackType_t di_stack[DIO_TASK_STACK];
static StaticTask_t di_tcb;

/* ================================================================
 * DO 输出 + LED 联动 (io_hooks.h 的强符号, 覆盖关系自本文件起取代
 * app_stubs.c 的 weak 占位)
 * ================================================================ */
int mb_set_do(uint16_t val)
{
	for (int i = 0; i < DO_NUM; i++) {
		GPIO_PinState on = (val & (1u << i)) ? GPIO_PIN_SET : GPIO_PIN_RESET;

		HAL_GPIO_WritePin(do_pins[i].port, do_pins[i].pin, on);
		HAL_GPIO_WritePin(led_pins[i].port, led_pins[i].pin, on);
	}
	return 0;
}

/* ================================================================
 * DI 采样线程
 * ================================================================ */
static void di_task(void *arg)
{
	(void)arg;

	while (1) {
		uint32_t si = get_holding_reg(HOLDING_DI_SAMPLE_MS_IDX);
		uint16_t en = get_holding_reg(HOLDING_DI_ENABLE_IDX);
		uint16_t val = 0;

		if (si < 10) {
			si = 10;
		} else if (si > SAMPLE_INTERVAL_MAX) {
			si = SAMPLE_INTERVAL_MAX;
		}

		for (int i = 0; i < DI_NUM; i++) {
			if ((en & (1u << i)) &&
			    HAL_GPIO_ReadPin(di_pins[i].port, di_pins[i].pin) == GPIO_PIN_SET) {
				val |= (1u << i);
			}
		}
		update_input_reg(INPUT_DI_IDX, val);

		/* 历史记录: send_history_data 内部按使能状态决定是否入队 */
		if (en) {
			struct his_data d = {0};

			d.type = DI_TYPE;
			d.timestamps = io_now_epoch();
			d.di.di_en_status = en;
			d.di.di_value = val;
			send_history_data(&d);
		}

		vTaskDelay(pdMS_TO_TICKS(si));
	}
}

/* ================================================================
 * GPIO 初始化 + 任务启动 (对齐 Zephyr SYS_INIT dio_init 与
 * K_THREAD_DEFINE 的合并; main 初始化序列调用)
 * ================================================================ */
void dio_start(void)
{
	GPIO_InitTypeDef io = {0};

	/* DI: 输入下拉 (光耦 idle 拉低, 高有效) */
	io.Mode = GPIO_MODE_INPUT;
	io.Pull = GPIO_PULLDOWN;
	for (int i = 0; i < DI_NUM; i++) {
		io.Pin = di_pins[i].pin;
		HAL_GPIO_Init(di_pins[i].port, &io);
	}

	/* DO/LED: 推挽输出, 初始低 (Zephyr GPIO_OUTPUT_INACTIVE) */
	io.Mode = GPIO_MODE_OUTPUT_PP;
	io.Pull = GPIO_NOPULL;
	io.Speed = GPIO_SPEED_FREQ_LOW;
	for (int i = 0; i < DO_NUM; i++) {
		io.Pin = do_pins[i].pin;
		HAL_GPIO_Init(do_pins[i].port, &io);
		HAL_GPIO_WritePin(do_pins[i].port, do_pins[i].pin, GPIO_PIN_RESET);
		io.Pin = led_pins[i].pin;
		HAL_GPIO_Init(led_pins[i].port, &io);
		HAL_GPIO_WritePin(led_pins[i].port, led_pins[i].pin, GPIO_PIN_RESET);
	}

	xTaskCreateStatic(di_task, "di", DIO_TASK_STACK, NULL,
			  DIO_TASK_PRIO, di_stack, &di_tcb);
	LOG_INF("DIO ready: %d DI, %d DO, %d LED", DI_NUM, DO_NUM, DO_NUM);
}
