/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * regmap.c (寄存器模型) 向其余模块的全部调用点。
 * Zephyr 版这些符号散落在 init.h 声明 + 各模块直链; FreeRTOS 版统一收口:
 *   - host 测试: 测试文件内提供假件 (tests/test_regmap.c)
 *   - 固件: src/sys/app_stubs.c 提供 weak 空实现占位,
 *     真实实现 (dio/history/time/reboot/os, 后续任务) 同名强符号覆盖
 */

#ifndef IO_HOOKS_H
#define IO_HOOKS_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== DO 控制 (dio.c) ==================== */
/* 设置 DO 输出 + LED 联动, val bit0-7 对应 DO1-DO8 */
void mb_set_do(uint16_t val);

/* ==================== 历史记录 (history.c) ==================== */
/* 开关历史写入 (holding_reg[0x05] 写回调调用) */
void history_enable_write(bool en);
/* 刷出缓存: 重启前 / FTP/HTTP 读文件前调用, 确保数据落盘 */
void history_sync(void);

/* ==================== 时间管理 (sys/time.c) ==================== */
/* 设置 RTC + 系统时钟 (Modbus 0x0E/0x0F 或 UDP 命令调用).
 * 范围门 946684800..4102444800; 返回 true=成功, false=参数越界或
 * RTC 写入失败 (Zephyr 版为 void, UDP 路径需判 ok 故改 bool) */
bool set_timestamp(time_t t);
/* 当前 Unix epoch (0x0E/0x0F 实时读) */
uint32_t io_now_epoch(void);

/* ==================== 重启 (sys/reboot.c) ==================== */
/* 冷重启 (Zephyr 版 regmap 里 reboot 前的 k_msleep(100) 延时移到本实现内) */
void io_reboot_cold(void);

/* ==================== 寄存器并发保护 (sys/os.c) ==================== */
/* FreeRTOS 互斥锁; host 测试为空实现。仅 io_write_do_bit 的读-改-写与
 * holding_reg_save 的全量导出持锁 (单字读写本身原子, 不加锁) */
void io_lock(void);
void io_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* IO_HOOKS_H */
