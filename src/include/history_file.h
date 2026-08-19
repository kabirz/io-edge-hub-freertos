/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * IO 历史记录纯文件核心 (host 可测: 仅依赖 littlefs + libc, 零 OS 调用)。
 *
 * Zephyr 版 src/history/history.c 的文件逻辑逐行移植 (命名/轮转/清理/续写),
 * OS 壳 (队列/任务/互斥) 在 history.c。调用方必须串行化: littlefs 无内部
 * 锁, 对共享 lfs_t 的每次 hist_file_* 调用都应在同一把锁下 (history.c)。
 */

#ifndef HISTORY_FILE_H
#define HISTORY_FILE_H

#include <time.h>
#include "lfs.h"
#include "init.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 绑定已挂载的文件系统并复位会话状态 (boot 语义: 无当前文件, 首次写入走
 * 目录扫描复用最新文件)。lfs 必须已成功 lfs_port_mount; lfs==NULL 时
 * 后续写入返回 -1。返回 0。
 */
int hist_file_init(lfs_t *lfs);

/* 追加一条记录 (DI 10B / AI 16B), 必要时轮转新文件并触发保留数清理。
 * 返回 0 成功; -1 = 文件系统不可用 / 打开失败 / 短写 (短写会把当前文件
 * 标记为满, 下次写入强制轮转, 对齐 Zephyr 版)。 */
int hist_file_write(const struct his_data *d);

/* 刷出 littlefs 缓冲 (文件未打开时为空操作)。返回 lfs_file_sync 结果。 */
int hist_file_sync(void);

/* 关闭当前文件但记住文件名: 下次写入续写同一文件 (disable→enable 语义)。
 * 未打开时为空操作。 */
void hist_file_close(void);

/* 时间注入 (仅 host 测试使用, target 不调用): 命名用时钟, 默认 time(NULL) */
void hist_file_set_clock(time_t (*fn)(void));

#ifdef __cplusplus
}
#endif

#endif /* HISTORY_FILE_H */
