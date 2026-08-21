/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * MCUboot CAN 紧急升级 (boot 域, Zephyr libs/can_fw_upgrade BOOT_WAIT 移植):
 *
 *   上电/复位后 boot_go 之前: 500ms 窗口内每 200ms 发 0x106 探测帧
 *   ("BTO1"+版本), 收到 0x107 应答进入救援会话: keyhash(0x104) /
 *   START(0x101, 擦 slot0) / DATA(0x103, 直写 slot0) / CONFIRM(0x101,
 *   0x55AA55AA) — CONFIRM 后本会话内 boot_go 直接验证并启动新镜像
 *   (无 swap 标记)。15s 无固件帧退出等待继续正常引导。
 *
 *   forever 模式 (无有效镜像的救援循环): 持续探测直到会话完成,
 *   CONFIRM 后软复位重新走 boot_go 验证。
 */

#ifndef BOOT_CAN_H
#define BOOT_CAN_H

#include <stdbool.h>

/* 探测等待 (forever=false): 无人应答最多 ~500ms 后返回正常引导 */
void boot_can_wait(bool forever);

/* 最近一次 wait 以 CONFIRM 结束 (装好镜像; forever 调用方复位重启) */
bool boot_rescue_done(void);

#endif /* BOOT_CAN_H */
