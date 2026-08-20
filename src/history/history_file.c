/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * IO 历史记录纯文件核心: Zephyr 版 src/history/history.c 文件逻辑的
 * littlefs 原生 API 移植 (命名/轮转/清理/续写逐行保留)。
 *
 *   - DI/AI 采样经 history.c 壳投递, 本层逐条追加写当前文件
 *   - 单文件 HIST_FILE_MAX 上限: 超过则关闭 + 新建 data_MMDD_HHMMSS.raw
 *   - 开机首次写入复用最近文件 (找最新的 data_*.raw, < 上限追加);
 *     运行期 disable→enable 续写关闭前的文件, 不新建
 *   - 保留至多 HIST_MAX_FILES 个文件, 超限删最旧 (按名序 = 时间序)
 *   - DI 10B / AI 16B, 与 RT-Thread / PC 解析工具兼容
 *
 * 移植映射: fs_open(FS_O_APPEND|FS_O_CREATE|FS_O_WRONLY) →
 * lfs_file_open(LFS_O_APPEND|LFS_O_CREAT|LFS_O_WRONLY);
 * fs_seek(END)+fs_tell → lfs_file_size (打开句柄);
 * fs_unlink → lfs_remove; fs_opendir/readdir → lfs_dir_open/read;
 * fs_sync → lfs_file_sync。Zephyr 的 HIST_DIR "/lfs1" 对应 lfs 根
 * ("/"), 路径即文件名本身。
 *
 * host 可测: 仅 libc + littlefs, 无任何 OS/FreeRTOS 调用; 时钟经
 * hist_file_set_clock 注入。littlefs 无内部锁 — 所有 hist_file_* 必须
 * 由调用方 (history.c 的 hist_lock) 串行化。
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "io_compat.h" /* MSVC gmtime_r 垫片 (主机测试) */
#include "history_file.h"

#include "log.h" /* LOG_* (host 测试经 LOG_ENABLE=0 编译为空) */

#define HIST_MAX_FILES 10
#ifndef HIST_FILE_MAX
#define HIST_FILE_MAX (1024 * 1024)
#endif

#ifndef CLAMP
#define CLAMP(v, lo, hi) (((v) < (lo)) ? (lo) : (((v) > (hi)) ? (hi) : (v)))
#endif

static lfs_t *his_lfs;
static lfs_file_t his_fp;
static bool his_fp_open;
static uint32_t his_cur_size;
/* 最近使用的历史文件名: disable 关闭后 re-enable 续写同一文件;
 * 空 = 尚未打开过 (开机走目录扫描复用最新文件) */
static char his_cur_name[24];

/* 命名用时钟 (host 测试注入固定时间; target 用 time(NULL), Zephyr 版同源) */
static time_t default_clock(void)
{
	return time(NULL);
}
static time_t (*hist_clock)(void) = default_clock;

/* 删除最旧历史文件, 维持 <= HIST_MAX_FILES 个 */
static void cleanup_old_files(void)
{
	lfs_dir_t dir;
	struct lfs_info info;
	char names[HIST_MAX_FILES + 2][24];
	int n = 0;

	if (lfs_dir_open(his_lfs, &dir, "/") != 0) {
		return;
	}
	while (lfs_dir_read(his_lfs, &dir, &info) > 0) {
		if (info.type != LFS_TYPE_REG || strncmp(info.name, "data_", 5) != 0) {
			continue;
		}
		if (n < HIST_MAX_FILES + 2) {
			/* 限长拷贝 (容量 24B 截断为本意, names[12] 上限与
			 * Zephyr 版一致; strncpy 在 -O3 触发误报
			 * -Wstringop-truncation, 改显式钳位) */
			size_t len = strlen(info.name);

			if (len > sizeof(names[0]) - 1) {
				len = sizeof(names[0]) - 1;
			}
			memcpy(names[n], info.name, len);
			names[n][len] = '\0';
			n++;
		}
	}
	lfs_dir_close(his_lfs, &dir);

	while (n > HIST_MAX_FILES) {
		int min_i = 0;

		for (int i = 1; i < n; i++) {
			if (strcmp(names[i], names[min_i]) < 0) {
				min_i = i;
			}
		}
		lfs_remove(his_lfs, names[min_i]);
		LOG_INF("rotated out %s", names[min_i]);
		if (min_i != n - 1) {
			strcpy(names[min_i], names[n - 1]);
		}
		n--;
	}
}

static void make_hist_name(char *buf, size_t len)
{
	/* UTC+8: 手动加 8 小时 (Zephyr 版注释同: picolibc 不做时区偏移) */
	time_t t = hist_clock() + 8 * 3600;
	struct tm tmp;
	struct tm *lt = gmtime_r(&t, &tmp);
	int mon, mday, hour, min, sec;

	/* RTC 未同步时 gmtime_r 可能返回 NULL, 用全零填充文件名兜底 */
	if (lt == NULL) {
		snprintf(buf, len, "data_0101_000000.raw");
		return;
	}
	/* 钳位到合法范围: 既保证 %02d 定宽 2 位, 也避免 RTC 异常数据
	 * 生成非法文件名 */
	mon = CLAMP(lt->tm_mon + 1, 1, 12);
	mday = CLAMP(lt->tm_mday, 1, 31);
	hour = CLAMP(lt->tm_hour, 0, 23);
	min = CLAMP(lt->tm_min, 0, 59);
	sec = CLAMP(lt->tm_sec, 0, 59);
	snprintf(buf, len, "data_%02d%02d_%02d%02d%02d.raw", mon, mday, hour, min, sec);
}

/* 打开 name 追加写入: 记录当前大小, 更新 his_fp_open/his_cur_size.
 * create=true 允许新建 (轮转/无文件时), false 要求文件已存在 (续写). */
static int open_append(const char *name, bool create)
{
	int flags = LFS_O_WRONLY | LFS_O_APPEND | (create ? LFS_O_CREAT : 0);
	lfs_soff_t size;

	if (lfs_file_open(his_lfs, &his_fp, name, flags) != 0) {
		return -1;
	}
	/* Zephyr 版 fs_seek(END)+fs_tell 取大小; LFS_O_APPEND 保证写总在末尾 */
	size = lfs_file_size(his_lfs, &his_fp);
	if (size < 0) {
		LOG_ERR("history file size failed: %ld", (long)size);
		lfs_file_close(his_lfs, &his_fp);
		return -1;
	}
	his_cur_size = (uint32_t)size;
	his_fp_open = true;
	return 0;
}

/* 关闭已打开的 his_fp (不改 his_cur_name, 供下次续写) */
static void close_cur_file(void)
{
	if (his_fp_open) {
		lfs_file_close(his_lfs, &his_fp);
		his_fp_open = false;
	}
}

/* 确保当前文件可写:
 * 1) 续写上次关闭的文件 (运行期 disable→enable, 文件需仍在且 < 上限)
 * 2) 开机首次写入: 扫描目录复用最新的 data_*.raw (< 上限追加)
 * 3) 都不行则新建 data_MMDD_HHMMSS.raw (空文件触发保留数清理) */
static int ensure_file(void)
{
	char name[24];

	if (his_fp_open && his_cur_size < HIST_FILE_MAX) {
		return 0;
	}
	close_cur_file();

	/* 续写上次会话的文件: 频繁 disable/enable 不应产生碎片小文件,
	 * 也不应借新建触发 cleanup_old_files 挤掉真正的历史文件 */
	if (his_cur_name[0] != '\0' && his_cur_size < HIST_FILE_MAX) {
		if (open_append(his_cur_name, false) == 0 && his_cur_size < HIST_FILE_MAX) {
			LOG_INF("history file: %s (%u bytes, appending)",
				his_cur_name, his_cur_size);
			return 0;
		}
		close_cur_file();
		his_cur_name[0] = '\0'; /* 已满或被删 (如 FTP 清理), 重新选择 */
	}

	/* 本轮首次写入: 尝试复用目录里最新的文件 */
	if (his_cur_name[0] == '\0') {
		lfs_dir_t dir;
		struct lfs_info info;
		char latest[24] = {0};

		if (lfs_dir_open(his_lfs, &dir, "/") == 0) {
			while (lfs_dir_read(his_lfs, &dir, &info) > 0) {
				if (info.type == LFS_TYPE_REG &&
				    strncmp(info.name, "data_", 5) == 0 &&
				    strcmp(info.name, latest) > 0) {
					/* 同上: 显式钳位拷贝, 避免
					 * -Wstringop-truncation 误报 */
					size_t len = strlen(info.name);

					if (len > sizeof(latest) - 1) {
						len = sizeof(latest) - 1;
					}
					memcpy(latest, info.name, len);
					latest[len] = '\0';
				}
			}
			lfs_dir_close(his_lfs, &dir);
		}
		if (latest[0] != '\0') {
			if (open_append(latest, false) == 0 && his_cur_size < HIST_FILE_MAX) {
				strcpy(his_cur_name, latest);
				LOG_INF("history file: %s (%u bytes, appending)",
					latest, his_cur_size);
				return 0;
			}
			close_cur_file();
		}
		/* 没找到可用文件, 走下面新建逻辑 */
	}

	make_hist_name(name, sizeof(name));
	if (open_append(name, true) != 0) {
		return -1;
	}
	strcpy(his_cur_name, name);
	if (his_cur_size == 0) {
		cleanup_old_files();
	}
	LOG_INF("history file: %s", name);
	return 0;
}

int hist_file_write(const struct his_data *d)
{
	size_t len;
	lfs_ssize_t wr;

	if (his_lfs == NULL) {
		return -1;
	}
	if (ensure_file() != 0) {
		return -1;
	}
	/* DI 记录 10B / AI 记录 16B: packed struct 写盘长度按 type 截断
	 * (DI union 成员止于偏移 10), 与 RT-Thread / PC 解析工具兼容 */
	len = (d->type == DI_TYPE) ? 10U : 16U;
	wr = lfs_file_write(his_lfs, &his_fp, d, len);
	if (wr == (lfs_ssize_t)len) {
		his_cur_size += len;
		return 0;
	}
	/* 部分写入: 文件可能超限, 强制下次轮转新文件 */
	LOG_WRN("history write short (%ld/%u)", (long)wr, (unsigned)len);
	his_cur_size = HIST_FILE_MAX;
	return -1;
}

int hist_file_sync(void)
{
	if (his_fp_open) {
		return lfs_file_sync(his_lfs, &his_fp);
	}
	return 0;
}

void hist_file_close(void)
{
	close_cur_file();
}

int hist_file_init(lfs_t *lfs)
{
	/* boot 语义: 静态状态归零 (Zephyr 版静态初始化即此), 不做任何
	 * lfs 调用 — 上一会话若异常掉电, 打开句柄本就不存在 */
	his_lfs = lfs;
	his_fp_open = false;
	his_cur_name[0] = '\0';
	his_cur_size = 0;
	return 0;
}

void hist_file_set_clock(time_t (*fn)(void))
{
	hist_clock = fn;
}
