/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modbus TCP ADU 层处理顺序:
 *
 *   1. MBAP 解析: trans_id be16@0 / proto_id be16@2 / length be16@4 /
 *      unit_id@6 / fc@7; length 钳制 MIN(len, 256) - 2 = PDU data 长度。
 *   2. proto_id != 0 -> 不进解码器, 直接回 server-failure ADU
 *      (fc|0x80 + data[0]=0x04 + len=1, 错误的 proto 原样回显,
 *      trans/unit 回显)。先于 unit 判定: 广播帧 (unit=0) 的 proto
 *      错误同样应答。
 *   3. unit != 0 -> 提交解码; 应答 unit 恒回填请求原始 unit,
 *      不暴露内部改写。
 *   4. unit == 0 (广播) -> 副作用照常执行 (mb_server 被调用), 但不
 *      产生任何响应; 上报 MB_DIAG_NO_RESP。
 *   5. mb_server 静默丢弃 (PDU 长度违例) -> 返回 0, 上报
 *      MB_DIAG_NO_RESP; bus/srv 计数解码器入口已计, 不重复。
 *   6. 应答: MBAP 头 + PDU 写入同一缓冲, 传输层单次 send()
 *      (分两次 send 会拆成两个 TCP 段, 部分上位机按"一段=一帧"解析)。
 */

#include <string.h>

#include "mbtcp_adu.h"
#include "mb_server.h"
#include "io_bytes.h"

#define MB_EXC_SERVER_DEVICE_FAILURE 0x04

/* length 字段钳制上限 */
#define MBTCP_MBAP_LEN_CLAMP 256u

/* 帧内偏移 */
#define MBTCP_OFF_TRANS_ID 0
#define MBTCP_OFF_PROTO_ID 2
#define MBTCP_OFF_LENGTH   4
#define MBTCP_OFF_UNIT_ID  6
#define MBTCP_OFF_FC       7

int mbtcp_adu_process(const uint8_t *in, uint16_t in_len,
		      uint8_t *out, uint16_t out_cap, uint16_t *out_len,
		      uint8_t srv_unit)
{
	uint16_t trans_id, proto_id, mbap_len, pdu_len, rsp_len;
	uint8_t unit_id, fc;

	if (in == NULL || out == NULL || out_len == NULL) {
		return 0;
	}
	/* 完整帧最少 8B (MBAP 7B + fc); 不足说明传输层未收满, 静默 */
	if (in_len < MBTCP_OFF_FC + 1) {
		return 0;
	}
	/* 应答路径需要 MBAP 7B + 响应 PDU 上限 (解码器契约) */
	if (out_cap < MBTCP_ADU_TX_MAX) {
		return 0;
	}

	trans_id = io_get_be16(&in[MBTCP_OFF_TRANS_ID]);
	proto_id = io_get_be16(&in[MBTCP_OFF_PROTO_ID]);
	mbap_len = io_get_be16(&in[MBTCP_OFF_LENGTH]);
	unit_id = in[MBTCP_OFF_UNIT_ID];
	fc = in[MBTCP_OFF_FC];

	/* 2. proto_id != 0: server-failure 应答 (先于广播判定);
	 *    proto 回显请求原始值 */
	if (proto_id != 0) {
		io_put_be16(trans_id, &out[MBTCP_OFF_TRANS_ID]);
		io_put_be16(proto_id, &out[MBTCP_OFF_PROTO_ID]);
		io_put_be16(3, &out[MBTCP_OFF_LENGTH]); /* unit+fc+exc */
		out[MBTCP_OFF_UNIT_ID] = unit_id;      /* 原始 unit 回显 */
		out[MBTCP_OFF_FC] = fc | 0x80;
		out[8] = MB_EXC_SERVER_DEVICE_FAILURE;
		*out_len = 9;
		return 1;
	}

	/* 1. length 钳制 MIN(mbap_len, 256) - 2 -> PDU data 长度;
	 *    fc 单独计入 -> PDU (fc+data) 总长。mbap_len < 2 时按 1 字节
	 *    PDU (仅 fc) 处理 -> 长度违例静默。 */
	if (mbap_len > MBTCP_MBAP_LEN_CLAMP) {
		mbap_len = MBTCP_MBAP_LEN_CLAMP;
	}
	pdu_len = (mbap_len >= 2) ? (uint16_t)(mbap_len - 2 + 1)
				  : (uint16_t)1;
	/* 防御: 传输层实际收到的字节不得少于声明的 PDU 长度 */
	if (pdu_len > in_len - MBTCP_OFF_FC) {
		pdu_len = (uint16_t)(in_len - MBTCP_OFF_FC);
	}

	/* 3. 应答 unit 恒为请求原始值, 见下方 out[6] 回填 */
	rsp_len = pdu_len; /* PDU 先写进 out+7, 应答头随后原地回填 */
	(void)srv_unit;    /* mb_server 不校验 unit, 参数未用 */

	/* 4./5. 解码: 广播帧同样进入 (副作用执行), 静默与否随后判定 */
	bool rsp = mb_server_process(&in[MBTCP_OFF_FC], pdu_len,
				     &out[MBTCP_OFF_FC], &rsp_len);

	if (unit_id == 0) {
		/* 广播: 副作用已执行, 不应答 */
		mb_server_diag_count(MB_DIAG_NO_RESP);
		return 0;
	}
	if (!rsp) {
		/* PDU 长度违例: 解码器静默丢弃 (bus/srv 已在其入口计数) */
		mb_server_diag_count(MB_DIAG_NO_RESP);
		return 0;
	}

	/* 6. 应答: trans 回显 + proto 0 + length=1+PDU 长度 + 原始 unit */
	io_put_be16(trans_id, &out[MBTCP_OFF_TRANS_ID]);
	io_put_be16(0, &out[MBTCP_OFF_PROTO_ID]);
	io_put_be16((uint16_t)(rsp_len + 1), &out[MBTCP_OFF_LENGTH]);
	out[MBTCP_OFF_UNIT_ID] = unit_id;
	*out_len = (uint16_t)(MBTCP_OFF_FC + rsp_len);
	return 1;
}
