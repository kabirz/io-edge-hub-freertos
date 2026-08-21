/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * boot 域轻量日志: USART1 115200 (与 app log 同口), 阻塞发送,
 * 无锁 (boot 单线程裸机)。app 侧 log.c 不编入 boot, 这里独占 huart1。
 */

#ifndef BOOT_UART_H
#define BOOT_UART_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

void boot_uart_init(void);

/* printf 风格单行输出, 自带 CRLF; 行长截断 160B */
void boot_log(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_UART_H */
