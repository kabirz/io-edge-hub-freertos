/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN1 业务通道对外接口 (Zephyr 版 src/can.c + libs/can_fw_upgrade 初始化
 * 部分的 FreeRTOS 移植, src/net/can.c)
 */

#ifndef APP_IO_CAN_H
#define APP_IO_CAN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CAN1 初始化: 波特率取 holding_reg[0x07] (reg 值即 kbps, 默认 250) 查表,
 * RX 过滤器匹配 holding_reg[0x06] (业务 ID, 默认 0x0111), 启动后进入
 * 正常模式。失败不阻断启动 (对齐 Zephyr can_fw_upgrade: CAN 失败仅损失
 * CAN 功能)。main 在 adc_start 之后、网络之前调用一次。 */
void can_start(void);

/* 发送标准数据帧 (Zephyr 版 mod_can_send 对应物):
 *   - len > 8 或未初始化 -> -1
 *   - 空闲发送邮箱等待 <=100ms (HAL_GetTick 忙等, 对齐 Zephyr
 *     can_send K_MSEC(100) 的等待上限)
 *   - HAL_CAN_AddTxMessage 成功 -> 0, 失败/超时 -> -1
 * 当前无调用者 (现版固件无周期推送), 供后续业务接入 */
int mod_can_send(uint32_t id, const uint8_t *data, uint8_t len);

#ifdef __cplusplus
}
#endif

#endif /* APP_IO_CAN_H */
