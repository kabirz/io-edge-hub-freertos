/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 数字/模拟 IO 模块 (src/io/dio.c + adc.c) 公共接口。
 *   - dio: 16 路 DI 采样任务 + 8 路 DO 输出 (mb_set_do, 声明在 io_hooks.h)
 *     + 8 路 LED 跟随
 *   - adc: 4 路 AI 采样任务 + 工程量换算
 *
 * mb_set_do 的声明收口在 io_hooks.h (regmap.c 的调用面); 本头只放
 * io 模块自身的启动入口与纯换算函数 (host 可测, adc.c 的 HAL/任务
 * 部分以 HOST_TEST 分离)。
 */

#ifndef IO_H
#define IO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== DI/DO (dio.c) ==================== */
/* GPIO 初始化 (DI 下拉输入 / DO+LED 推挽输出低) + DI 采样任务 (prio 6,
 * 栈 256 字)。main 初始化序列调用 (对齐 Zephyr SYS_INIT dio_init +
 * K_THREAD_DEFINE 的合并) */
void dio_start(void);

/* ==================== ADC (adc.c) ==================== */
/* ADC1 + PC0-PC3 模拟引脚初始化 + AI 采样任务 (prio 6, 栈 256 字) */
void adc_start(void);

/* 12-bit raw -> 工程量 (AI0/1: 0.01mA, AI2/3: 0.01V), 纯函数 (host 测试
 * tests/test_adc_math.c 锁定整除语义):
 *   voltage_mv = raw * 3300 / 4096        -- 32 位整除先行截断
 *   val        = coeff[ch] * mv / 10000   -- 64 位中间量再整除 */
uint16_t ai_convert(uint8_t ch, int32_t raw);

#ifdef __cplusplus
}
#endif

#endif /* IO_H */
