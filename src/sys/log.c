/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 日志输出 (target-only): USART1 115200 8N1, PA9(TX)/PA10(RX) AF7。
 *   - log_write: [HH:MM:SS.mmm][L] msg\r\n。HH:MM:SS = (epoch+8h) 取一天内
 *     偏移再 gmtime (先取模免大 epoch 依赖 time_t 宽度; 天数略去,
 *     对齐 Zephyr +8 时区显示, 控制器决议 #3), mmm = 当前秒内毫秒
 *     (io_now_ms, tick 相位推算)
 *   - log_line/log_raw: shell 等非日志输出共用出口 (前者整行加 CRLF,
 *     后者裸字节供回显/prompt), 与 log_write 同一 UART、同一把锁,
 *     行级不交织
 *   - 线程安全: 静态互斥锁覆盖 "格式化 + 发送" 整段, 多任务行不交织;
 *     log_init 前锁句柄为 NULL, 直发 (仅 main 早期单线程窗口可达)
 *   - _write (newlib printf 重定向) 自 syscalls.c 移交至此, 与 log_write
 *     同一 UART 出口、同一把锁
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "FreeRTOS.h"
#include "semphr.h"

#include "main.h"
#include "io_time.h"
#include "log.h"

#define LOG_UART_BAUD 115200u
#define LOG_LINE_MAX  160u  /* 溢出截断; 160B @115200 约 14ms 阻塞 */
#define LOG_TMO_MS    100u  /* 单次 HAL 阻塞发送超时 */

static SemaphoreHandle_t log_mutex;
static StaticSemaphore_t log_mutex_cb;

/* 行级发送: huart1 未初始化 (log_init 前, 防御路径) 直接丢弃 */
static void log_emit(const char *buf, uint16_t len)
{
	if (huart1.Instance == NULL) {
		return;
	}
	(void)HAL_UART_Transmit(&huart1, (const uint8_t *)buf, len, LOG_TMO_MS);
}

void log_init(void)
{
	GPIO_InitTypeDef io = {0};

	__HAL_RCC_USART1_CLK_ENABLE();

	/* PA9 TX / PA10 RX, AF7 (仅 TX 实际使用; RX 一并初始化备用) */
	io.Pin = GPIO_PIN_9 | GPIO_PIN_10;
	io.Mode = GPIO_MODE_AF_PP;
	io.Pull = GPIO_PULLUP;
	io.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	io.Alternate = GPIO_AF7_USART1;
	HAL_GPIO_Init(GPIOA, &io);

	huart1.Instance = USART1;
	huart1.Init.BaudRate = LOG_UART_BAUD;
	huart1.Init.WordLength = UART_WORDLENGTH_8B;
	huart1.Init.StopBits = UART_STOPBITS_1;
	huart1.Init.Parity = UART_PARITY_NONE;
	huart1.Init.Mode = UART_MODE_TX_RX;
	huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	huart1.Init.OverSampling = UART_OVERSAMPLING_16;
	if (HAL_UART_Init(&huart1) != HAL_OK) {
		Error_Handler();
	}

	log_mutex = xSemaphoreCreateMutexStatic(&log_mutex_cb);
	if (log_mutex == NULL) {
		Error_Handler();
	}
}

/* 行输出核心: 可选前缀 + 用户格式化 + CRLF (调用方持锁) */
static void log_vline(const char *prefix, const char *fmt, va_list ap)
{
	char buf[LOG_LINE_MAX];
	int n = 0, m;

	if (prefix != NULL) {
		n = (int)strlen(prefix);
		memcpy(buf, prefix, (size_t)n);
	}

	m = vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
	if (m > 0) {
		n += m; /* 截断时 m 超余量, 下方统一钳位 */
	}
	if (n < 0) {
		n = 0; /* 防御: 格式化错误输出空行 */
	}
	if ((size_t)n > sizeof(buf) - 3) {
		n = (int)sizeof(buf) - 3; /* 尾部 CRLF + NUL */
	}

	buf[n++] = '\r';
	buf[n++] = '\n';
	log_emit(buf, (uint16_t)n);
}

void log_write(char level, const char *fmt, ...)
{
	char prefix[24]; /* "[HH:MM:SS.mmm][X] " + NUL */
	struct tm tm;
	va_list ap;
	time_t t;

	/* 时间戳: epoch+8h 的一天内偏移 (本地 +8, 对齐 Zephyr 显示习惯)
	 * + 当前秒内毫秒 */
	t = (time_t)(((uint32_t)io_now_epoch() + 8u * 3600u) % 86400u);
	(void)gmtime_r(&t, &tm);
	(void)snprintf(prefix, sizeof(prefix), "[%02d:%02d:%02d.%03u][%c] ",
		       tm.tm_hour, tm.tm_min, tm.tm_sec,
		       (unsigned)io_now_ms(), level);

	if (log_mutex != NULL) {
		(void)xSemaphoreTake(log_mutex, portMAX_DELAY);
	}
	va_start(ap, fmt);
	log_vline(prefix, fmt, ap);
	va_end(ap);
	if (log_mutex != NULL) {
		(void)xSemaphoreGive(log_mutex);
	}
}

void log_line(const char *fmt, ...)
{
	va_list ap;

	if (log_mutex != NULL) {
		(void)xSemaphoreTake(log_mutex, portMAX_DELAY);
	}
	va_start(ap, fmt);
	log_vline(NULL, fmt, ap);
	va_end(ap);
	if (log_mutex != NULL) {
		(void)xSemaphoreGive(log_mutex);
	}
}

void log_raw(const char *buf, uint16_t len)
{
	if (log_mutex != NULL) {
		(void)xSemaphoreTake(log_mutex, portMAX_DELAY);
	}
	log_emit(buf, len);
	if (log_mutex != NULL) {
		(void)xSemaphoreGive(log_mutex);
	}
}

/* newlib _write 重定向 (printf/puts 出口; 自 syscalls.c 移交) */
int _write(int fd, const char *buf, int len)
{
	(void)fd;

	if (log_mutex != NULL) {
		(void)xSemaphoreTake(log_mutex, portMAX_DELAY);
	}
	log_emit(buf, (uint16_t)len);
	if (log_mutex != NULL) {
		(void)xSemaphoreGive(log_mutex);
	}
	return len;
}
