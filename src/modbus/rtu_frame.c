/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modbus RTU 从站帧状态机 (纯逻辑, host 可测)。传输层 (rtu.c) 喂数据
 * 与 t3.5 超时, 本模块负责拼帧、校验、单播过滤、交付解码与应答组装。
 *
 * 语义基准 (Zephyr 源码逐条核定):
 *   - subsys/modbus/modbus_serial.c modbus_rtu_rx_adu(): len<4 或 >256
 *     -> -EMSGSIZE; CRC 不符 -> -EIO; PDU = buf[1..len-3]。
 *   - subsys/modbus/modbus_core.c modbus_rx_handler(): 解析后无条件调
 *     modbus_server_handler() (错误帧也进 handler)。
 *   - subsys/modbus/modbus_server.c modbus_server_handler(): 入口
 *     update_msg_ctr (bus_msg); rx_adu_err!=0 -> noresp (+-EIO 时
 *     crc_err); 他站 unit -> noresp; 广播 -> 副作用执行后 send_reply
 *     强制 false -> noresp; 正常应答无 noresp。
 *
 * 诊断计数组合 (本移植 mb_server_process 在解码器入口计 bus/srv,
 * 故传输层只对"未到达解码器"的丢弃帧补 MB_DIAG_BUS_MSG, 不重复计):
 *   溢出 / len<4  -> bus_msg + no_resp            (Zephyr -EMSGSIZE 路径)
 *   CRC 错        -> bus_msg + crc_err + no_resp  (Zephyr -EIO 路径)
 *   他站 unit     -> bus_msg + no_resp            (Zephyr 单计 noresp)
 *   广播 unit==0  -> [解码器计 bus/srv] + no_resp (副作用照常执行)
 *   单播 PDU 长度违例被解码器静默 -> [解码器计 bus/srv] + no_resp
 *   空 t3.5 (无字节) -> 无操作 (Zephyr 定时器只在收到字节后启动,
 *   空到期不可能发生; 传输层误触发时防御性忽略)
 * 溢出说明: Zephyr 字面行为是把截断的 256B 缓冲照做 CRC 校验 (几乎必
 * 走 -EIO -> crc_err); 本实现按任务锁定的"溢出 -> 丢弃至复位"规则归
 * 入长度违例类 (不计 crc_err), 分歧已在任务报告记录。
 *
 * 并发: target 上 rtu_rx_feed 运行于 USART2 ISR, rtu_t35_expired 运行
 * 于 RTU 任务; t3.5 到期时刻帧必已收完 (3.5 字符静默 >= 任何字节间隔),
 * 入口先快照长度/溢出再复位, 处理中新到的字节属于下一帧 (被复位清掉,
 * 对应 Zephyr 在 work 里先 rx_disable 再解析的语义)。
 */

#include <string.h>

#include "rtu_frame.h"
#include "mb_server.h"
#include "io_crc.h"

/* Zephyr CONFIG_MODBUS_BUFFER_SIZE / MODBUS_RTU_MIN_MSG_SIZE */
#define RTU_FRAME_MAX 256u
#define RTU_FRAME_MIN 4u

/* 拼帧状态 (ISR 写 / 任务读) */
static uint8_t rx_buf[RTU_FRAME_MAX];
static volatile uint16_t rx_len;
static volatile bool rx_overflow;

/* 绑定配置 */
static uint8_t srv_unit;
static uint32_t bound_baud; /* 记录对齐 Zephyr cfg->baud, 见头文件 */
static void (*tx_cb)(const uint8_t *frame, uint16_t len);

/* 应答组装缓冲: unit + PDU 上限 + crc16 LE16 */
static uint8_t rsp_pdu[MB_SERVER_PDU_MAX];
static uint8_t tx_frame[1 + MB_SERVER_PDU_MAX + 2];

/* 弱默认: host 测试无定时器; target 传输层 (rtu.c) 强符号覆盖 */
__attribute__((weak)) void rtu_t35_kick(void)
{
}

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
		/* 长度违例 (Zephyr -EMSGSIZE): 静默 */
		mb_server_diag_count(MB_DIAG_BUS_MSG);
		mb_server_diag_count(MB_DIAG_NO_RESP);
		return;
	}

	unit = rx_buf[0];
	crc_rx = (uint16_t)(rx_buf[len - 2] | (rx_buf[len - 1] << 8));
	crc_calc = crc16_modbus(rx_buf, (size_t)(len - 2));
	if (crc_rx != crc_calc) {
		/* CRC 错 (Zephyr -EIO): 静默 */
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

	/* 应答: unit 回显请求值 (Zephyr tx_adu.unit_id = rx_adu.unit_id)
	 * + PDU + crc16 LE16 */
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
		/* 规范定值 (任务锁定); Zephyr 字面公式为 >38400 钳制到
		 * ~1ms, 此处按 Modbus 规范的 2ms 推荐 */
		return 2u;
	}

	/* ceil(3.5 字符 x 11 bit x 1e6 / baud) us, 再 ceil 到 ms
	 * (FreeRTOS 软件定时器分辨率) */
	us = (38500000u + baud - 1u) / baud;
	return (us + 999u) / 1000u;
}
