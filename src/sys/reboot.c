/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 重启 (Zephyr 版 regmap 的 k_msleep(100)+sys_reboot 与 main.c 的
 * 延迟重启标志收口于此):
 *   - io_reboot_cold(): history_sync 已由调用方先行完成, 此处 100ms
 *     延时 (等应答/日志上线, 对齐 Zephyr 重启路径) 后 NVIC_SystemReset
 *   - set_reboot_status/get_reboot_status: 延迟重启待办标志 (RAM
 *     volatile bool, 对齐 Zephyr main.c; Zephyr 版由 web/shell 置位、
 *     主循环轮询, 本版由心跳任务轮询)
 */

#include <stdbool.h>

#include "FreeRTOS.h"
#include "task.h"

#include "main.h"     /* NVIC_SystemReset (CMSIS) */
#include "io_hooks.h" /* set/get_reboot_status 声明 */

static volatile bool reboot_pending;

void set_reboot_status(bool en)
{
	reboot_pending = en;
}

bool get_reboot_status(void)
{
	return reboot_pending;
}

void io_reboot_cold(void)
{
	/* vTaskDelay 需调度器运行 -- 全部调用点 (Modbus/UDP 任务) 皆任务
	 * 上下文; 100ms 只为发送缓冲上线, 不复返回 */
	vTaskDelay(pdMS_TO_TICKS(100));
	NVIC_SystemReset();
}
