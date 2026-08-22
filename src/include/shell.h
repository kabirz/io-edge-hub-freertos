/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 调试 shell (USART1, 与日志同一串口) — Zephyr 版 SHELL 的最小对应物:
 *   - RX: 寄存器级中断 (绕开 HAL UART 锁, 避开与日志阻塞 TX 的
 *     竞态) -> 环形缓冲 -> shell 任务行组装 (回显/退格)
 *   - 输出经 log_line/log_raw (与日志同一把锁, 行级不交织)
 *   - 命令: help / tasks (任务表) / reboot / io <子命令>
 *     (io 子命令集对齐 Zephyr 版 src/shell.c)
 */

#ifndef SHELL_H
#define SHELL_H

#ifdef __cplusplus
extern "C" {
#endif

/* 启动 shell (boot 任务在 log_init 之后调用): 创建任务 + 打开
 * USART1 RX 中断。target-only */
void shell_start(void);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_H */
