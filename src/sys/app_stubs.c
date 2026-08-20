/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * io_hooks.h 钩子的 weak 空实现: Task 13 后仅剩 mb_set_do (dio.c,
 * Task 14 落地)。其余钩子已有强符号实现 (强符号自动覆盖本文件):
 *   history_enable_write/history_sync -> src/history/history.c (T8)
 *   set_timestamp/io_now_epoch        -> src/sys/time.c (T13)
 *   io_reboot_cold/set/get_reboot_status -> src/sys/reboot.c (T13)
 *   io_lock/io_unlock (+os_init)      -> src/sys/os.c (T13)
 *   watchdog_init/watchdog_feed       -> src/sys/watchdog.c (T13)
 *
 * host 测试不链接本文件 -- 测试自带假件 (见 tests/test_regmap.c)。
 */

#include "io_hooks.h"

__attribute__((weak)) void mb_set_do(uint16_t val)
{
	(void)val;
}
