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
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "history.h"
#include "history_file.h"

#include "log.h"

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

/* ==================== Web/HTTP 文件访问 ==================== */

/* 下载会话 (httpd 单客户端串行使用; 每次 lfs 操作单独持锁) */
static lfs_file_t web_fp;
static bool web_fp_open;

bool history_web_name_valid(const char *name)
{
	size_t n = strlen(name);

	if (strncmp(name, "data_", 5) != 0) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		char c = name[i];
		bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
			  (c >= 'A' && c <= 'Z') || c == '_' || c == '.' || c == '-';

		if (!ok) {
			return false;
		}
	}
	return n > 5 && n < 32;
}

int history_web_list_json(char *buf, size_t bufsz)
{
	if (hist_lfs == NULL) {
		return -1;
	}

	lfs_dir_t dir;
	int n = 0;

	xSemaphoreTake(hist_lock, portMAX_DELAY);
	hist_file_sync();
	n += snprintf(buf + n, bufsz - n, "{\"files\":[");
	if (lfs_dir_open(hist_lfs, &dir, "/") == 0) {
		struct lfs_info info;
		bool first = true;

		while (lfs_dir_read(hist_lfs, &dir, &info) > 0) {
			if (info.type != LFS_TYPE_REG ||
			    !history_web_name_valid(info.name)) {
				continue;
			}
			n += snprintf(buf + n, bufsz - n,
				      "%s{\"name\":\"%s\",\"size\":%u}",
				      first ? "" : ",", info.name,
				      (unsigned)info.size);
			first = false;
			if (n >= (int)bufsz - 64) {
				break;
			}
		}
		lfs_dir_close(hist_lfs, &dir);
	}
	n += snprintf(buf + n, bufsz - n, "]}");
	xSemaphoreGive(hist_lock);
	return (n > (int)bufsz) ? (int)bufsz : n;
}

int history_web_open(const char *name)
{
	if (hist_lfs == NULL || !history_web_name_valid(name)) {
		return -1;
	}

	int rc = -1;

	xSemaphoreTake(hist_lock, portMAX_DELAY);
	/* 下载前刷采样缓存 (对齐 Zephyr /api/history/download 先
	 * history_sync 再打开): 当前文件末尾的最新采样可见 */
	hist_file_sync();
	if (!web_fp_open && lfs_file_open(hist_lfs, &web_fp, name,
					  LFS_O_RDONLY) == 0) {
		lfs_soff_t size = lfs_file_size(hist_lfs, &web_fp);

		if (size >= 0) {
			web_fp_open = true;
			rc = (int)size;
		} else {
			lfs_file_close(hist_lfs, &web_fp);
		}
	}
	xSemaphoreGive(hist_lock);
	return rc;
}

int history_web_read(uint8_t *buf, uint16_t len)
{
	if (!web_fp_open) {
		return -1;
	}

	lfs_ssize_t n;

	xSemaphoreTake(hist_lock, portMAX_DELAY);
	n = lfs_file_read(hist_lfs, &web_fp, buf, len);
	xSemaphoreGive(hist_lock);
	return (int)n;
}

void history_web_close(void)
{
	if (!web_fp_open) {
		return;
	}
	xSemaphoreTake(hist_lock, portMAX_DELAY);
	lfs_file_close(hist_lfs, &web_fp);
	xSemaphoreGive(hist_lock);
	web_fp_open = false;
}

int history_web_remove(const char *name)
{
	if (hist_lfs == NULL || !history_web_name_valid(name)) {
		return -1;
	}

	int rc;

	xSemaphoreTake(hist_lock, portMAX_DELAY);
	rc = lfs_remove(hist_lfs, name);
	xSemaphoreGive(hist_lock);
	return rc;
}

void history_web_usage(uint64_t *free_b, uint64_t *total_b)
{
	*free_b = 0;
	*total_b = 0;
	if (hist_lfs == NULL) {
		return;
	}

	xSemaphoreTake(hist_lock, portMAX_DELAY);
	struct lfs_fsinfo info;

	if (lfs_fs_stat(hist_lfs, &info) == 0) {
		lfs_ssize_t used = lfs_fs_size(hist_lfs);

		*total_b = (uint64_t)info.block_count * info.block_size;
		if (used >= 0) {
			uint64_t used_b = (uint64_t)used * info.block_size;

			*free_b = (used_b < *total_b) ? (*total_b - used_b) : 0;
		}
	}
	xSemaphoreGive(hist_lock);
}

/* ==================== FTP 文件访问 (ftpd.c) ==================== */

/* littlefs 挂载实例 (未挂载 NULL); ftpd 通用命令直接调 lfs API,
 * 每次 lfs 操作单独持锁 (长传输分块之间放锁, 不饿死采样落盘) */
lfs_t *history_fs(void)
{
	return hist_lfs;
}

void history_fs_lock(void)
{
	if (hist_lock != NULL) {
		xSemaphoreTake(hist_lock, portMAX_DELAY);
	}
}

void history_fs_unlock(void)
{
	if (hist_lock != NULL) {
		xSemaphoreGive(hist_lock);
	}
}
