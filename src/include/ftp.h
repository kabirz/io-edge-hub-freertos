/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * FTP 服务器配置 (根目录 = littlefs 根, admin/admin) — Zephyr 版
 * src/ftp_server/ftp.h 的移植。FTP_ROOT 为空: 本版 littlefs 直接以
 * "/" 为根挂载 (Zephyr 为 /lfs1 挂载点)。
 */

#ifndef __FTP_H__
#define __FTP_H__

#define FTP_CTRL_PORT 21
#define FTP_USER      "admin"
#define FTP_PASS      "admin"
#define FTP_ROOT      ""
#define FTP_BUF_SIZE  512

/* 启动 FTP 任务 (main 在 history_init 之后调用; fs 未挂载时命令层
 * 以 550 应答)。target-only */
void ftpd_start(void);

#endif /* __FTP_H__ */
