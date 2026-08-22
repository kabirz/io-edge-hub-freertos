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

/* ==================== Web/HTTP 文件访问 (httpd.c 调用) ====================
 * 与历史写任务共享同一把串行锁, 每次 lfs 操作单独持锁 (下载分块读
 * 之间放锁, 不阻塞采样落盘)。单客户端串行使用。 */

/* 文件名合法性: data_ 前缀 + 字母数字/._-, 6..31 字符 (防路径穿越) */
bool history_web_name_valid(const char *name);

/* {"files":[...]} 写入 buf, 返回长度; fs 未就绪 -1 */
int history_web_list_json(char *buf, size_t bufsz);

/* 打开 data_* 供下载, 返回文件大小, -1 失败 */
int history_web_open(const char *name);

/* 顺序分块读: >=0 实际字节 (0=EOF), <0 错误 */
int history_web_read(uint8_t *buf, uint16_t len);

void history_web_close(void); /* 幂等 */

int history_web_remove(const char *name); /* 0 成功 */

void history_web_usage(uint64_t *free_b, uint64_t *total_b); /* 字节, 未挂载 0/0 */

/* ==================== FTP 通用文件访问 (ftpd.c) ====================
 * littlefs 实例 + 共享串行锁: ftpd 直接调 lfs API, 每次 lfs 操作
 * 单独持锁 (与 Web 下载同模式; 分块之间放锁不阻塞采样落盘) */
lfs_t *history_fs(void);      /* 未挂载 NULL */
void history_fs_lock(void);
void history_fs_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* HISTORY_H */
