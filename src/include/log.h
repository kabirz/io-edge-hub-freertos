/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 日志 (Zephyr LOG_MODULE_REGISTER/LOG_INF 等的最小对应物, Task 13):
 *   - USART1 115200 阻塞发送 (src/sys/log.c, target-only)
 *   - 格式 [HH:MM:SS.mmm][L] msg\r\n, 时钟 = epoch+8h 的一天内偏移
 *     + 当前秒内毫秒 (对齐 Zephyr 日志 +8 本地时区显示习惯)
 *   - LOG_ENABLE=0 时全部宏编译为空 (host 测试统一如此编译; 固件默认开,
 *     也可编译期整体关闭省体积)
 *   - 文件/函数/行号按需写进消息文本, 宏不强制携带 (保持精简)
 */

#ifndef APP_LOG_H
#define APP_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LOG_ENABLE
#define LOG_ENABLE 1
#endif

#if LOG_ENABLE

/* level 字符: 'E'(err) / 'W'(wrn) / 'I'(inf) */
void log_write(char level, const char *fmt, ...);

#define LOG_ERR(...) log_write('E', __VA_ARGS__)
#define LOG_WRN(...) log_write('W', __VA_ARGS__)
#define LOG_INF(...) log_write('I', __VA_ARGS__)

#else /* LOG_ENABLE == 0: 编译为空 */

#define LOG_ERR(...) do {} while (0)
#define LOG_WRN(...) do {} while (0)
#define LOG_INF(...) do {} while (0)

#endif /* LOG_ENABLE */

/* USART1 + 日志互斥锁初始化 (main 最早调用, 先于一切 LOG 使用点) */
void log_init(void);

/* 整行输出 (自动补 CRLF, 无时间戳前缀): shell 命令应答等非日志文本,
 * 与 LOG_* 同一 UART、同一把锁 (行级不交织)。target-only */
void log_line(const char *fmt, ...);

/* 裸字节发送 (无 CRLF, 持同一把锁): shell 回显 / prompt。target-only */
void log_raw(const char *buf, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* APP_LOG_H */
