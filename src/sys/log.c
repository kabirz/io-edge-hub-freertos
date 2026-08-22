/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 日志输出 (target-only): USART1 115200 8N1, PA9(TX)/PA10(RX) AF7。
 * 异步延迟打印 -- 调用任务只做格式化+入队 (微秒级), 不再被串口阻塞:
 *   - 4KB 环形缓冲 (单生产者串行, 单消费者), 专职 logger 任务按序刷出;
 *     日志与 shell (log_line/log_raw)、printf (_write) 同一队列 FIFO,
 *     行级次序与原子性保持
 *   - 环满丢弃新行并计数, 追平后补一行 "[W] N log lines dropped"
 *     (突发流量 > 11.5KB/s 串口带宽时的设计降级: 设备不卡, 日志裁剪)
 *   - log_write 前缀 [HH:MM:SS.mmm][L]: epoch+8h 一天内偏移 + tick 相位
 *     毫秒 (时间戳在入队时取 = 事件时间, 非刷出时间)
 *   - log_flush(timeout): 复位前排空队列 (fw 升级/延迟重启路径),
 *     否则 NVIC_SystemReset 会带走未刷出的尾部日志
 *   - log_init 前的调用丢弃 (对齐旧行为); 不可用于 ISR 上下文
 *     (入队互斥锁不可从中断获取; 现网 ISR 路径均只入队自身队列)
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "main.h"
#include "io_time.h"
#include "log.h"

#define LOG_UART_BAUD 115200u
#define LOG_LINE_MAX  160u  /* 单行缓冲 (溢出截断) */
#define LOG_TMO_MS    100u  /* 单次 HAL 阻塞发送超时 */

/* 环形缓冲: 2 的幂, head/tail 单调递增 (u32, 回绕 ~102h @11.5KB/s) */
#define LOG_RING_SIZE 4096u
#define LOG_RING_MASK (LOG_RING_SIZE - 1u)

#define LOG_TASK_PRIO  2   /* 高于 shell/hb: 刷出延迟低; 低于网络任务 */
#define LOG_TASK_STACK 256 /* 字 = 1KB: 仅发送循环, 无格式化深度 */

static uint8_t log_ring[LOG_RING_SIZE];
static volatile uint32_t ring_head; /* 多生产者写 (log_push_lock 串行) */
static volatile uint32_t ring_tail; /* 仅 logger 任务写 */

static SemaphoreHandle_t log_push_lock; /* 多生产者入队互斥 (仅 memcpy) */
static StaticSemaphore_t log_push_lock_cb;

static TaskHandle_t log_task_handle;
static StackType_t log_stack[LOG_TASK_STACK];
static StaticTask_t log_tcb;

static volatile uint32_t log_dropped; /* 环满丢弃行计数 */

/* 行级发送: 仅 logger 任务调用 (串口唯一持有者, 无需互斥) */
static void log_emit(const char *buf, uint16_t len)
{
	if (huart1.Instance == NULL) {
		return;
	}
	(void)HAL_UART_Transmit(&huart1, (const uint8_t *)buf, len, LOG_TMO_MS);
}

/* 入队 (任务上下文): 满则整行丢弃并计数 */
static void log_push(const char *buf, uint16_t len)
{
	uint32_t head;
	uint32_t space;
	uint32_t off;
	uint32_t first;

	if (len == 0 || log_push_lock == NULL) {
		return; /* log_init 前: 丢弃 */
	}
	if (xSemaphoreTake(log_push_lock, portMAX_DELAY) != pdTRUE) {
		return;
	}

	head = ring_head;
	space = LOG_RING_SIZE - (head - ring_tail);
	if ((uint32_t)len > space) {
		log_dropped++;
	} else {
		off = head & LOG_RING_MASK;
		first = LOG_RING_SIZE - off;
		if (first > len) {
			first = len;
		}
		memcpy(&log_ring[off], buf, first);
		if ((uint32_t)len > first) {
			memcpy(log_ring, buf + first, (size_t)len - first);
		}
		ring_head = head + len;
	}

	xSemaphoreGive(log_push_lock);
	if (log_task_handle != NULL) {
		xTaskNotifyGive(log_task_handle);
	}
}

/* 行输出核心: 可选前缀 + 用户格式化 + CRLF -> 入队 */
static void log_vline(const char *prefix, const char *fmt, va_list ap)
{
	char buf[LOG_LINE_MAX];
	int n = 0;
	int m;

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
	log_push(buf, (uint16_t)n);
}

/* ==================== 对外接口 ==================== */

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

	va_start(ap, fmt);
	log_vline(prefix, fmt, ap);
	va_end(ap);
}

void log_line(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	log_vline(NULL, fmt, ap);
	va_end(ap);
}

void log_raw(const char *buf, uint16_t len)
{
	log_push(buf, len);
}

void log_flush(uint32_t timeout_ms)
{
	TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

	/* 等队列排空 (丢弃补报行由 logger 排空后自打, 多留一拍) */
	while (ring_head != ring_tail || log_dropped != 0u) {
		if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
			return;
		}
		vTaskDelay(pdMS_TO_TICKS(2));
	}
	vTaskDelay(pdMS_TO_TICKS(5));
}

/* ==================== logger 任务 ==================== */

static void log_task_fn(void *arg)
{
	(void)arg;

	for (;;) {
		(void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		for (;;) {
			uint32_t tail = ring_tail;
			uint32_t used = ring_head - tail;

			if (used == 0) {
				break;
			}
			{
				/* 连续段: 到环尾或到 head, 单次发送 */
				uint32_t off = tail & LOG_RING_MASK;
				uint32_t chunk = LOG_RING_SIZE - off;

				if (chunk > used) {
					chunk = used;
				}
				log_emit((const char *)&log_ring[off],
					 (uint16_t)chunk);
				ring_tail = tail + chunk;
			}
		}

		/* 追平后补报丢弃 (先清零: 期间新增丢弃归下一轮) */
		{
			uint32_t d = log_dropped;

			if (d != 0u) {
				char msg[48];
				int n;

				log_dropped = 0;
				n = snprintf(msg, sizeof(msg),
					     "[W] %lu log lines dropped\r\n",
					     (unsigned long)d);
				if (n > 0) {
					log_emit(msg, (uint16_t)n);
				}
			}
		}
	}
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

	log_push_lock = xSemaphoreCreateMutexStatic(&log_push_lock_cb);
	if (log_push_lock == NULL) {
		Error_Handler();
	}
	log_task_handle = xTaskCreateStatic(log_task_fn, "logger",
					     LOG_TASK_STACK, NULL,
					     LOG_TASK_PRIO, log_stack, &log_tcb);
	if (log_task_handle == NULL) {
		Error_Handler();
	}
}

/* newlib _write (printf/puts 出口): 同队列 FIFO, 与日志行不交织 */
int _write(int fd, const char *buf, int len)
{
	(void)fd;
	log_push(buf, (uint16_t)len);
	return len;
}
