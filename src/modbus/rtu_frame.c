/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modbus RTU 从站帧状态机 (纯逻辑, host 可测)。传输层 (rtu.c) 喂数据
 * 与 t3.5 超时, 本模块负责拼帧、校验、单播过滤、交付解码与应答组装。
 *
 * 诊断计数组合 (mb_server_process 在解码器入口计 bus/srv, 故本层只对
 * "未到达解码器"的丢弃帧补 MB_DIAG_BUS_MSG, 不重复计):
 *   溢出 / len<4  -> bus_msg + no_resp
 *   CRC 错        -> bus_msg + crc_err + no_resp
 *   他站 unit     -> bus_msg + no_resp
 *   广播 unit==0  -> [解码器计 bus/srv] + no_resp (副作用照常执行)
 *   单播 PDU 长度违例被解码器静默 -> [解码器计 bus/srv] + no_resp
 *   空 t3.5 (无字节) -> 无操作 (防御性忽略)
 *
 * 并发: target 上 rtu_rx_feed 运行于 USART2 ISR, rtu_t35_expired 运行
 * 于 RTU 任务; t3.5 到期时刻帧必已收完 (3.5 字符静默 >= 任何字节间隔),
 * 入口先快照长度/溢出再复位, 处理中新到的字节属于下一帧 (被复位清掉)。
 */

#include <string.h>

#include "rtu_frame.h"
#include "mb_server.h"
#include "io_crc.h"
#include "io_compat.h" /* IO_WEAK (MSVC 主机测试兼容) */

#define RTU_FRAME_MAX 256u
#define RTU_FRAME_MIN 4u

/* 拼帧状态 (ISR 写 / 任务读) */
static uint8_t rx_buf[RTU_FRAME_MAX];
static volatile uint16_t rx_len;
static volatile bool rx_overflow;

/* 绑定配置 */
static uint8_t srv_unit;
static uint32_t bound_baud; /* 绑定波特率记录, 见头文件 */
static void (*tx_cb)(const uint8_t *frame, uint16_t len);

/* 应答组装缓冲: unit + PDU 上限 + crc16 LE16 */
static uint8_t rsp_pdu[MB_SERVER_PDU_MAX];
static uint8_t tx_frame[1 + MB_SERVER_PDU_MAX + 2];

/* rtu_t35_kick 无默认实现: target 由 rtu.c 提供 (强符号),
 * host 测试提供计数假件 (跨编译器: MSVC 无弱符号机制)。 */

void rtu_reset(void)
{
	rx_len = 0;
	rx_overflow = false;
}

void rtu_rx_feed(const uint8_t *bytes, uint16_t len)
{
	if (bytes == NULL || len == 0) {
		return;
	}

	if (!rx_overflow) {
		uint16_t space = (uint16_t)(RTU_FRAME_MAX - rx_len);
		uint16_t take = (len <= space) ? len : space;

		memcpy(&rx_buf[rx_len], bytes, take);
		rx_len = (uint16_t)(rx_len + take);
		if (take < len) {
			/* 缓冲满: 后续字节丢弃至复位 (仍踢定时器, 让帧
			 * 在 t3.5 后终结并复位) */
			rx_overflow = true;
		}
	}

	rtu_t35_kick();
}

void rtu_t35_expired(void)
{
	uint16_t len = rx_len;
	bool overflow = rx_overflow;
	uint8_t unit;
	uint16_t crc_rx, crc_calc, rsp_len;
	bool rsp;

	/* 快照后立即复位: 处理期间新到的字节属于下一帧 */
	rx_len = 0;
	rx_overflow = false;

	if (len == 0) {
		return;
	}

	if (overflow || len < RTU_FRAME_MIN) {
		/* 长度违例: 静默 */
		mb_server_diag_count(MB_DIAG_BUS_MSG);
		mb_server_diag_count(MB_DIAG_NO_RESP);
		return;
	}

	unit = rx_buf[0];
	crc_rx = (uint16_t)(rx_buf[len - 2] | (rx_buf[len - 1] << 8));
	crc_calc = crc16_modbus(rx_buf, (size_t)(len - 2));
	if (crc_rx != crc_calc) {
		/* CRC 错: 静默 */
		mb_server_diag_count(MB_DIAG_BUS_MSG);
		mb_server_diag_count(MB_DIAG_CRC_ERR);
		mb_server_diag_count(MB_DIAG_NO_RESP);
		return;
	}

	if (unit != 0 && unit != srv_unit) {
		/* 他站帧: 静默 */
		mb_server_diag_count(MB_DIAG_BUS_MSG);
		mb_server_diag_count(MB_DIAG_NO_RESP);
		return;
	}

	/* 送达解码器 (单播+广播): bus/srv 由 mb_server_process 入口计,
	 * 广播副作用照常执行 */
	rsp_len = MB_SERVER_PDU_MAX;
	rsp = mb_server_process(&rx_buf[1], (uint16_t)(len - 3),
				rsp_pdu, &rsp_len);

	if (!rsp || unit == 0) {
		/* PDU 长度违例静默 / 广播不应答 */
		mb_server_diag_count(MB_DIAG_NO_RESP);
		return;
	}

	/* 应答: unit 回显请求值 + PDU + crc16 LE16 */
	tx_frame[0] = unit;
	memcpy(&tx_frame[1], rsp_pdu, rsp_len);
	crc_calc = crc16_modbus(tx_frame, (size_t)rsp_len + 1);
	tx_frame[1 + rsp_len] = (uint8_t)(crc_calc & 0xFF);
	tx_frame[2 + rsp_len] = (uint8_t)(crc_calc >> 8);
	if (tx_cb != NULL) {
		tx_cb(tx_frame, (uint16_t)(rsp_len + 3));
	}
}

void rtu_frame_bind(uint8_t unit, uint32_t baud,
		    void (*tx)(const uint8_t *frame, uint16_t len))
{
	srv_unit = unit;
	bound_baud = baud;
	tx_cb = tx;
	rtu_reset();
}

uint32_t rtu_t35_ms(uint32_t baud)
{
	uint32_t us;

	if (baud == 0 || baud > 19200u) {
		/* 规范推荐定值: 高波特率下 t3.5 固定取 2ms */
		return 2u;
	}

	/* ceil(3.5 字符 x 11 bit x 1e6 / baud) us, 再 ceil 到 ms
	 * (FreeRTOS 软件定时器分辨率) */
	us = (38500000u + baud - 1u) / baud;
	return (us + 999u) / 1000u;
}
