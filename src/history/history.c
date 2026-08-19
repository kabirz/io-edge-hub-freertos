/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * IO 历史记录 OS 壳 (Zephyr 版 src/history/history.c 的 FreeRTOS 移植):
 * msgq + k_work 专用工作队列 -> 静态队列 + 专属任务。文件逻辑 (命名/
 * 轮转/清理/续写) 在纯核心 history_file.c (host 已测)。
 *
 *   - DI/AI 采样线程 send_history_data() 入队; 历史任务批量取队写当前
 *     文件 (保持打开, 多条记录合并落盘), 减少 Flash 写次数
 *   - history_enable_write(false): 异步关文件 (任务内下次迭代处理),
 *     re-enable 续写同一文件, 不新建
 *   - littlefs 无内部锁: 共享 lfs_t 上的全部 hist_file_* 调用由本模块
 *     的静态互斥 hist_lock 串行化 (write 与 sync 互斥; 与 regmap 的
 *     io_lock 无关 — 那是 holding_reg[] 的锁)
 */

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "history.h"
#include "history_file.h"

/* LOG 占位 (Task 13 替换为真实日志) */
#define LOG_WRN(...) do {} while (0)
#define LOG_INF(...) do {} while (0)

#define HIST_QUEUE_DEPTH 16

static volatile bool history_enabled;
static volatile bool hist_close_req; /* disable 请求任务异步关文件 */
/* 非 NULL = 已就绪 (队列/锁/任务均建成); 同时是 "fs 未挂载则丢弃" 依据 */
static lfs_t *hist_lfs;

/* littlefs 串行化锁 (覆盖 hist_file_write/sync/close 的每次 lfs 调用) */
static StaticSemaphore_t hist_lock_cb;
static SemaphoreHandle_t hist_lock;

static uint8_t hist_queue_buf[HIST_QUEUE_DEPTH * sizeof(struct his_data)];
static StaticQueue_t hist_queue_cb;
static QueueHandle_t hist_queue;

static StackType_t hist_task_stack[1024]; /* 字 = 4096B, 对齐 Zephyr hist workq 栈 */
static StaticTask_t hist_task_tcb;

static void hist_task(void *arg)
{
	struct his_data d;

	(void)arg;
	for (;;) {
		if (xQueueReceive(hist_queue, &d, pdMS_TO_TICKS(100)) == pdTRUE) {
			xSemaphoreTake(hist_lock, portMAX_DELAY);
			/* 批量排空当前积压 (Zephyr his_flush: 取空 msgq 为止) */
			do {
				if (hist_file_write(&d) != 0) {
					/* 短写/打开失败: 本条丢弃 (与 Zephyr 同, 部分
					 * 写入的尾字节作废), 剩余记录留在队列, 下次
					 * 迭代已强制轮转到新文件再写 */
					break;
				}
			} while (xQueueReceive(hist_queue, &d, 0) == pdTRUE);
			if (!history_enabled) {
				/* disable: 关闭文件但记住名字, 下次 enable 续写同一文件 */
				hist_file_close();
				hist_close_req = false;
			}
			xSemaphoreGive(hist_lock);
		} else if (hist_close_req) {
			/* 禁用且队列空: 异步关闭 (不在调用线程做 Flash IO) */
			xSemaphoreTake(hist_lock, portMAX_DELAY);
			hist_file_close();
			hist_close_req = false;
			xSemaphoreGive(hist_lock);
		}
	}
}

void history_init(lfs_t *lfs)
{
	hist_file_init(lfs);
	hist_lock = xSemaphoreCreateMutexStatic(&hist_lock_cb);
	hist_queue = xQueueCreateStatic(HIST_QUEUE_DEPTH, sizeof(struct his_data),
					hist_queue_buf, &hist_queue_cb);
	xTaskCreateStatic(hist_task, "history", 1024, NULL, 2,
			  hist_task_stack, &hist_task_tcb);
	/* 发布顺序保证 hist_lfs != NULL 时队列/锁/任务均已就绪 */
	hist_lfs = lfs;
}

bool send_history_data(const struct his_data *d)
{
	static uint32_t drop_cnt;

	if (!history_enabled || hist_lfs == NULL) {
		return false; /* 关闭/未挂载: 静默丢弃 */
	}
	if (xQueueSend(hist_queue, d, 0) == pdTRUE) {
		return true;
	}
	/* 后台落盘慢时队列会满, 节流告警: 每 4 次一告 (对齐 Zephyr
	 * atomic_inc 返回旧值后再判 %4 的节奏) */
	if ((drop_cnt++ % 4) == 0) {
		LOG_WRN("history queue full, %u samples dropped", drop_cnt);
	}
	return false;
}

void history_enable_write(bool en)
{
	history_enabled = en;
	LOG_INF("history %s", en ? "enabled" : "disabled");
	if (!en) {
		/* 异步 flush + 关闭, 不在调用线程 (Modbus 写回调) 做 Flash IO;
		 * 任务在下次迭代排空队列后关闭 (Zephyr k_work 提交的对应物) */
		hist_close_req = true;
	}
}

void history_sync(void)
{
	if (hist_lfs == NULL) {
		return;
	}
	/* 重启前 / FTP/HTTP 读文件前调用: 与写任务互斥地刷盘 */
	xSemaphoreTake(hist_lock, portMAX_DELAY);
	hist_file_sync();
	xSemaphoreGive(hist_lock);
}
