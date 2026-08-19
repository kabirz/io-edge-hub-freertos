/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 硬件看门狗 (IWDG) 接口 — Zephyr 版 src/sys/watchdog.h 的对应物。
 * 真实实现 (30s 超时) 在 sys/watchdog 任务落地; lfs_port 擦除回调与
 * w25qxx 擦除轮询在长擦除期间调用 watchdog_feed() 防止中途复位。
 */

#ifndef IO_WATCHDOG_H
#define IO_WATCHDOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 IWDG */
void watchdog_init(void);

/* 喂狗: 主循环周期调用; 文件系统长擦除期间事件型调用 */
void watchdog_feed(void);

#ifdef __cplusplus
}
#endif

#endif /* IO_WATCHDOG_H */
