/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN 固件升级通道 (Zephyr libs/can_fw_upgrade 协议的 FreeRTOS 移植):
 *   0x101 命令 [cmd LE32][arg LE32]: 0=START(size) 1=CONFIRM(perm)
 *                                    2=VERSION 3=REBOOT
 *   0x102 应答 [code LE32][arg LE32]: 0=OFFSET 1=UPDATE_SUCCESS 2=VERSION
 *      3=CONFIRM(0x55AA55AA) 4=FLASH_ERROR 5=TRANSFER_ERROR 6=KEYHASH_ERROR
 *   0x103 数据 <=8B/帧, 每 64B 回 OFFSET, 收满回 UPDATE_SUCCESS
 *   0x104 keyhash [seq][7B chunk] x5 帧 (START 前送, 到齐才校验)
 *   0x105 版本分片 [seq][7B 文本] (设备发)
 */

#ifndef FW_CAN_H
#define FW_CAN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 创建 RX 队列 + 任务; 须在 can_start() 之前调用 (ISR 入队依赖) */
void fw_can_start(void);

/* CAN RX 帧注入 (ISR 上下文, can.c 调用): 入队, 满则丢弃 */
void fw_can_frame_isr(uint32_t id, const uint8_t *data, uint8_t dlc);

#ifdef __cplusplus
}
#endif

#endif /* FW_CAN_H */
