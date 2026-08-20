/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * OS 胶水: io_lock/io_unlock = 静态 FreeRTOS 互斥锁 (Zephyr 版
 * regmap 里 k_mutex 的对应物), 覆盖 DO 位读-改-写与 holding_reg_save
 * 全量导出两类临界区 (单字读写本身原子, 不加锁)。
 *
 * os_init() 在 main 建任务前调用, 其后 io_lock 可用; 锁句柄未建
 * (调用早于 os_init) 时直通 -- 该窗口只有 main 单线程路径, 无并发
 * 可言。持锁临界区均为寄存器 RAM 操作, 无阻塞调用, 不会久持。
 */

#include "FreeRTOS.h"
#include "semphr.h"

#include "main.h"     /* Error_Handler */
#include "io_hooks.h" /* io_lock/io_unlock/os_init 声明 */

static SemaphoreHandle_t io_mutex;
static StaticSemaphore_t io_mutex_cb;

void os_init(void)
{
	io_mutex = xSemaphoreCreateMutexStatic(&io_mutex_cb);
	if (io_mutex == NULL) {
		Error_Handler(); /* 静态创建仅参数错误会失败, 属编程错 */
	}
}

void io_lock(void)
{
	if (io_mutex != NULL) {
		(void)xSemaphoreTake(io_mutex, portMAX_DELAY);
	}
}

void io_unlock(void)
{
	if (io_mutex != NULL) {
		(void)xSemaphoreGive(io_mutex);
	}
}
