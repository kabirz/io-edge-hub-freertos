/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modbus PDU 解码器 (Zephyr subsys/modbus/modbus_server.c 的从零重写,
 * 行为逐分支对齐源实现)。本文件硬件/OS 无关: 输入为一条完整 PDU
 * (fc + data, 不含 MBAP 头 / RTU CRC / unit-id), 可在主机直接测试。
 *
 * 寄存器后端是 regmap.c (Task 5): Zephyr 的 modbus_user_callbacks
 * (io_modbus_cbs 表) 在此直接映射为 io_read_holding / io_write_holding /
 * io_write_do_bit / io_coil_rd / io_discrete_rd / get_input_reg 调用。
 *
 * FP 扩展区 (地址 >= 5000) 同 Zephyr: 仅 FC03/04/16 走 FP 分支且 fp
 * 回调为 NULL -> 异常 0x01; FC06 无 FP 分支, 走整数写回调越界 -> 0x02。
 */

#ifndef MB_SERVER_H
#define MB_SERVER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FC08 诊断计数器 (对应 Zephyr ctx->mbs_*_ctr, 16 位)。
 * bus_msg/srv_msg/exc 由 mb_server_process 内部维护;
 * crc_err/no_resp 由传输层上报 (对应 Zephyr 的 rx_adu_err /
 * unit_id 不匹配 / 广播 / 静默丢弃路径)。 */
enum mb_diag_counter {
	MB_DIAG_BUS_MSG, /* FC08 子功能 0x0B */
	MB_DIAG_CRC_ERR, /* FC08 子功能 0x0C */
	MB_DIAG_EXC,     /* FC08 子功能 0x0D */
	MB_DIAG_SRV_MSG, /* FC08 子功能 0x0E */
	MB_DIAG_NO_RESP, /* FC08 子功能 0x0F */
};

/* 处理一条 PDU (fc + data, 不含 MBAP/CRC)。
 * 返回 true = 有响应 (out 前 *out_len 字节有效, 长度含 fc 字节);
 *        false = 静默丢弃 (长度违例, 不回异常, 对应 Zephyr 各 FC
 *                开头 "Wrong request length" 的 return false)。
 * out 缓冲至少 MB_SERVER_PDU_MAX 字节。 */
bool mb_server_process(const uint8_t *in, uint16_t in_len,
		       uint8_t *out, uint16_t *out_len);

/* 传输层上报诊断事件: 收到 CRC 错误帧报 MB_DIAG_CRC_ERR;
 * mb_server_process 返回 false (静默丢弃) 或收到广播帧 (不应答)
 * 时报 MB_DIAG_NO_RESP。 */
void mb_server_diag_count(enum mb_diag_counter c);

/* 响应 PDU 缓冲上限 (MODBUS_RTU_MTU; FC01/02 qty<=2000 -> 252 字节) */
#define MB_SERVER_PDU_MAX 256

#ifdef __cplusplus
}
#endif

#endif /* MB_SERVER_H */
