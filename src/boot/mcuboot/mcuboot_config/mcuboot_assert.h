/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * MCUboot assert 替身: 默认 assert 走 newlib abort() -> _exit 无声死循环
 * (首次 SWAP 即因此变砖且无诊断)。改为调用域实现 mcuboot_assert_fail
 * (boot: UART 报行号停机; app: LOG 后复位), 行号定位 bootutil 源文件。
 */

#ifndef MCUBOOT_ASSERT_H
#define MCUBOOT_ASSERT_H

void mcuboot_assert_fail(int line);

/* newlib <assert.h> 若已被引入 (nano 头链), 先撤掉其定义避免重定义告警 */
#undef assert

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr)                     \
	do {                             \
		if (!(expr)) {           \
			mcuboot_assert_fail(__LINE__); \
		}                      \
	} while (0)
#endif

#endif /* MCUBOOT_ASSERT_H */
