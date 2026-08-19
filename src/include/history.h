/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * IO 历史记录 OS 壳 (FreeRTOS 队列 + 任务; 文件逻辑在 history_file.c)。
 * 函数名对齐 Zephyr 版 src/history/history.c 的公开接口。
 */

#ifndef HISTORY_H
#define HISTORY_H

#include <stdbool.h>
#include "lfs.h"
#include "init.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 启动历史任务 (main 在 lfs_port_mount 成功后调用; lfs=NULL 仅建队不落盘) */
void history_init(lfs_t *lfs);

/* 生产者投递 (DI/AI 采样线程): 关闭/未挂载静默丢弃; 队满丢 + 计数。
 * 返回 true=已入队, false=丢弃 */
bool send_history_data(const struct his_data *d);

/* 开关历史写入 (holding_reg[0x05] 写回调调用); 关闭为异步关文件 */
void history_enable_write(bool en);

/* 刷出缓存: 重启前 / FTP/HTTP 读文件前调用, 确保数据落盘 */
void history_sync(void);

#ifdef __cplusplus
}
#endif

#endif /* HISTORY_H */
