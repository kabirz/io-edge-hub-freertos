/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 硬件看门狗 (STM32 独立看门狗 IWDG) — Zephyr 版 src/sys/watchdog.c
 * (wdt 驱动) 的 HAL 直接移植。
 *
 * 30 秒超时, 心跳任务周期喂狗 (原 Zephyr main 主循环职责, 3s 周期);
 * fs_littlefs mkfs 擦整个 15MB NOR 期间 lfs_port/w25qxx 事件型喂狗
 * (30s 窗口需容纳外部 SPI NOR 全擦, 可达数十秒)。
 *
 * 计数: LSI 标称 32kHz / 预分频 256 = 125 Hz, 重装 3750 -> 30.0 s。
 * (LSI 出厂 17-47kHz, 实际窗口约 20-56s; reload 上限 0xFFF=4095)
 */

#include "main.h"
#include "io_watchdog.h"

static IWDG_HandleTypeDef hiwdg;

void watchdog_init(void)
{
	/* 调试器挂起时冻结 IWDG (对齐 Zephyr WDT_OPT_PAUSE_HALTED_BY_DBG) */
	__HAL_DBGMCU_FREEZE_IWDG();

	hiwdg.Instance = IWDG;
	hiwdg.Init.Prescaler = IWDG_PRESCALER_256;
	hiwdg.Init.Reload = 3750u; /* 125 Hz x 30 s */
	if (HAL_IWDG_Init(&hiwdg) != HAL_OK) {
		Error_Handler();
	}
}

void watchdog_feed(void)
{
	/* watchdog_init 之前的调用 (w25qxx 擦除轮询 / lfs_port 挂载失败格式
	 * 化路径) 为无操作: IWDG 尚未启动无需喂, Instance 判空防 NULL 解引用 */
	if (hiwdg.Instance != NULL) {
		HAL_IWDG_Refresh(&hiwdg);
	}
}
