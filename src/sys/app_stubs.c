/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * io_hooks.h 全部钩子的 weak 空实现: 让固件在真实模块 (dio/history/
 * time/reboot/os, 后续任务) 落地前保持可链接。真实实现以同名强符号
 * 定义即可覆盖 (链接器自动优选强符号)。
 *
 * host 测试不链接本文件 -- 测试自带假件 (见 tests/test_regmap.c)。
 */

#include "io_hooks.h"
#include "io_watchdog.h"

__attribute__((weak)) void mb_set_do(uint16_t val)
{
	(void)val;
}

__attribute__((weak)) void history_enable_write(bool en)
{
	(void)en;
}

__attribute__((weak)) void history_sync(void)
{
}

__attribute__((weak)) bool set_timestamp(time_t t)
{
	(void)t;
	return false; /* 占位: 未接 RTC 前不谎报成功 */
}

__attribute__((weak)) void io_reboot_cold(void)
{
}

__attribute__((weak)) uint32_t io_now_epoch(void)
{
	return 0; /* RTC 未接: 上电未设时间, 对齐 Zephyr time(NULL)==0 语义 */
}

__attribute__((weak)) void io_lock(void)
{
}

__attribute__((weak)) void io_unlock(void)
{
}

/* watchdog: 真实 IWDG 实现在 sys/watchdog 任务 (Task 13) 落地,
 * 同名强符号自动覆盖; lfs_port/w25qxx 已按本接口喂狗 */
__attribute__((weak)) void watchdog_init(void)
{
}

__attribute__((weak)) void watchdog_feed(void)
{
}
