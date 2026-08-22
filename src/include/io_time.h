/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 系统时间 (RTC + epoch 缓存) — Zephyr 版 src/sys/time.c 的 FreeRTOS
 * 移植。io_hooks.h 声明的 set_timestamp/io_now_epoch 在 src/sys/time.c
 * 提供强符号实现。
 */

#ifndef IO_TIME_H
#define IO_TIME_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 合法时间戳范围门 (纯函数, host 测试): [2000-01-01, 2100-01-01) 半开
 * 区间 UTC (上端点 2100-01-01 00:00:00 本身拒绝, 对齐 Zephyr 版)。
 * 越界值 (主站只写低 16 位、高字为 0 -> 1970 年等) 直接拒绝 */
bool ts_in_range(time_t t);

/* 设置 RTC + epoch 缓存 (Modbus 0x0E/0x0F 写低位或 UDP SET_TIME 调用)。
 * 返回 true=成功; false=RTC 未就绪 (LSE/RTC 初始化失败)/参数越界/
 * RTC 写失败 (不动缓存, Zephyr 版 rtc_set_time+clock_settime 的合并
 * 语义) */
bool set_timestamp(time_t t);

/* 当前 Unix epoch (u32 截断, 对齐 Zephyr (uint32_t)time(NULL) 寄存器
 * 用法; 回绕 2106 年, 范围门本身限 2100): boot 从 RTC 载入缓存,
 * 之后 1Hz 软件定时器递增 (RTC 本体只在设时间时写) */
uint32_t io_now_epoch(void);

/* 当前秒内毫秒 0-999 (与 io_now_epoch 的 1Hz 递增同相位, tick 快照
 * 推算): 供日志时间戳; host 测试不可用 (依赖 FreeRTOS tick) */
uint32_t io_now_ms(void);

/* LSE + RTC 初始化 (main 早期调用, 需在 log_init 之后 -- 内部有日志):
 * 读 RTC 恢复缓存, 无效日期 -> 0; 备份域无标志 (首次上电) 时先写入
 * 默认 2020-01-01 00:00:00, 之后由 VBAT 维持。LSE/RTC 初始化失败不
 * 阻断启动: 1Hz 定时器照常启动 (epoch 从 0 自走), set_timestamp 被
 * rtc_ready 守卫拒绝 */
void io_time_init(void);

#ifdef __cplusplus
}
#endif

#endif /* IO_TIME_H */
