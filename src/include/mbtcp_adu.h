/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modbus TCP ADU 层 (纯逻辑, 主机可测; Zephyr 版 tcp.c handle_client 的
 * 移植, MBAP 头解析/校验、unit 改写、广播抑制、MBAP+PDU 合并应答)。
 *
 * 传输层 (tcp.c / RTU 任务) 收满一条完整 ADU 帧后调用本函数:
 *   - proto_id != 0 -> server-failure 应答 (fc|0x80 + 0x04, proto 回显
 *     请求原始值, trans/unit 回显), 不进 PDU 解码器
 *   - MBAP length: MIN(len, 256) - 2 -> PDU data 长度 (Zephyr 同款钳制;
 *     长度上限检查在钳制后为死代码, 故只在 proto!=0 时失败)
 *   - unit != 0 -> 内部改写为 srv_unit 后进解码器 (mb_server 不校验 unit,
 *     改写仅为与 Zephyr 行为对齐); 应答 unit 恒为请求原始 unit
 *   - unit == 0 (广播) -> 副作用执行但不产生任何响应, 上报 NO_RESP 诊断
 *   - PDU 长度违例 (mb_server 静默丢弃) -> 返回 0, 上报 NO_RESP 诊断
 */

#ifndef MBTCP_ADU_H
#define MBTCP_ADU_H

#include <stdint.h>

#include "mb_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 处理一条完整 ADU 帧 (MBAP 7B + PDU), 输出应答帧到 out。
 * srv_unit: 本机从站号 (任务启动时读 holding_reg[0x09] 缓存)。
 * 返回 1 = 发送应答 (out 前 *out_len 字节, 单缓冲已合并 MBAP+PDU);
 *        0 = 静默 (广播 / PDU 长度违例), *out_len 不被触碰。
 * out 容量须 >= MBTCP_ADU_TX_MAX。 */
int mbtcp_adu_process(const uint8_t *in, uint16_t in_len,
		      uint8_t *out, uint16_t out_cap, uint16_t *out_len,
		      uint8_t srv_unit);

/* 应答帧缓冲上限: MBAP 7B + 响应 PDU 上限 (MB_SERVER_PDU_MAX) */
#define MBTCP_ADU_TX_MAX (7 + MB_SERVER_PDU_MAX)

/* 接收帧缓冲上限: 6B 头 + length 钳制上限 256 (与 Zephyr 消费字节数一致) */
#define MBTCP_ADU_RX_MAX (6 + 256)

#ifdef __cplusplus
}
#endif

#endif /* MBTCP_ADU_H */
