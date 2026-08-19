/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * littlefs 移植层 (Zephyr 版 src/storage/fs_littlefs.c 的裸 littlefs 对应物)。
 *
 * 把 io_flash NOR 后端 (目标: W25Qxx SPI; host 测试: fake_flash) 接到
 * deps/littlefs, 分区 LFS_OFFSET/LFS_SIZE (config_store.h)。挂载失败
 * (含脏分区/半擦除分区) 时自动格式化重挂 — 对齐 Zephyr 版 "任何失败都
 * mkfs 一次" 的语义。擦除回调内喂狗 (全片格式化可达分钟级)。
 *
 * 无动态分配: cache/lookahead 缓冲均为静态。
 */

#ifndef LFS_PORT_H
#define LFS_PORT_H

#include "lfs.h"
#include "io_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 在 flash 后端上挂载 littlefs 分区。成功返回 0; 任何挂载失败都会先
 * lfs_format 再重挂 (数据丢失可接受 — 格式化语义), 仍失败返回 -1。
 * lfs 与 port 内部的 lfs_config/缓冲在挂载期间必须保持有效; lfs 卸载
 * 后可再次挂载 (同一静态 config)。
 */
int lfs_port_mount(lfs_t *lfs, const struct io_flash *flash);

#ifdef __cplusplus
}
#endif

#endif /* LFS_PORT_H */
