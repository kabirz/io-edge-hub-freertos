/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modbus PDU 解码器 (基于 uC/Modbus mbs_core.c 的 Apache-2.0 派生实现)。
 * 本文件只见 PDU (fc + data); ADU 层 (MBAP 头 / RTU CRC / unit-id 过滤 /
 * 广播不应答) 归传输层 (Task 10/11)。
 *
 * FP 扩展区 (地址 >= 5000) 无 fp 回调: FC03/04/16 一律异常 0x01
 * (ILLEGAL_FC), 无浮点路径; FC06 无 FP 分支, 走整数回调越界 -> 异常 0x02。
 * 自定义 FC 表为空: 未知 FC -> 0x01。
 *
 * 诊断计数器对应关系 (FC08 子功能):
 *   bus_msg (0x0B): mb_server_process 每次入口 +1
 *   srv_msg (0x0E): 同上 (传输层只把发往本机的 PDU 送来)
 *   exc     (0x0D): 每条异常响应 +1
 *   crc_err (0x0C) / no_resp (0x0F): 传输层经 mb_server_diag_count 上报
 */

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "mb_server.h"
#include "init.h"
#include "io_bytes.h"

/* ==================== 常量 ==================== */

/* Function codes */
#define MB_FC01_COIL_RD		0x01
#define MB_FC02_DI_RD		0x02
#define MB_FC03_HOLDING_REG_RD	0x03
#define MB_FC04_IN_REG_RD	0x04
#define MB_FC05_COIL_WR		0x05
#define MB_FC06_HOLDING_REG_WR	0x06
#define MB_FC08_DIAGNOSTICS	0x08
#define MB_FC15_COILS_WR	0x0F
#define MB_FC16_HOLDING_REGS_WR	0x10

/* FC08 诊断子功能 */
#define MB_FC08_SUBF_QUERY		0x0000
#define MB_FC08_SUBF_CLR_CTR		0x000A
#define MB_FC08_SUBF_BUS_MSG_CTR		0x000B
#define MB_FC08_SUBF_BUS_CRC_CTR		0x000C
#define MB_FC08_SUBF_BUS_EXCEPT_CTR	0x000D
#define MB_FC08_SUBF_SERVER_MSG_CTR	0x000E
#define MB_FC08_SUBF_SERVER_NO_RESP_CTR	0x000F

/* 异常码 */
#define MB_EXC_ILLEGAL_FC		0x01
#define MB_EXC_ILLEGAL_DATA_ADDR		0x02
#define MB_EXC_ILLEGAL_DATA_VAL		0x03

#define MB_COIL_OFF_CODE		0x0000 /* FC05: 值 0x0000 = OFF, 其他 = ON */

/* FP 扩展区起始地址 */
#define MB_FP_EXTENSIONS_ADDR		5000

/* ==================== 诊断计数器 ==================== */

static uint16_t diag_ctr[5]; /* 按 enum mb_diag_counter 下标 */

void mb_server_diag_count(enum mb_diag_counter c)
{
	if ((unsigned int)c <= MB_DIAG_NO_RESP) {
		diag_ctr[c]++;
	}
}

/* ==================== 寄存器回调 ==================== */
/* 返回 0 = 成功, -1 = 地址不支持 (-> 异常 0x02) */

static int holding_reg_rd_cb(uint16_t addr, uint16_t *reg)
{
	if (addr >= MODBUS_HOLDING_REGISTER_NUMBERS) {
		return -1;
	}
	*reg = io_read_holding(addr); /* 0x0E/0x0F 返回实时时间 */
	return 0;
}

static int input_reg_rd_cb(uint16_t addr, uint16_t *reg)
{
	if (addr >= MODBUS_INPUT_REGISTER_NUMBERS) {
		return -1;
	}
	*reg = get_input_reg(addr);
	return 0;
}

static int coil_rd_cb(uint16_t addr, bool *state)
{
	return io_coil_rd(addr, state);
}

static int discrete_input_rd_cb(uint16_t addr, bool *state)
{
	return io_discrete_rd(addr, state);
}

static int coil_wr_cb(uint16_t addr, bool state)
{
	return io_write_do_bit(addr, state);
}

static int holding_reg_wr_cb(uint16_t addr, uint16_t reg)
{
	return io_write_holding(addr, reg);
}

/* ==================== 响应组装辅助 ==================== */

/* 异常响应: fc|0x80 + 异常码, 计 exc++。
 * 返回 true = 有响应 */
static bool exc_rsp(uint8_t fc, uint8_t code, uint8_t *out, uint16_t *out_len)
{
	diag_ctr[MB_DIAG_EXC]++;

	out[0] = fc | 0x80;
	out[1] = code;
	*out_len = 2;
	return true;
}

/* FC01 (0x01) Read Coils
 *
 * Request:  fc | 起始地址(2) | 线圈数(2)
 * Response: fc | 字节数(1) | 线圈状态 N*1
 */
static bool fc01_coil_read(const uint8_t *d, uint16_t dlen,
			   uint8_t *out, uint16_t *out_len)
{
	const uint16_t coils_limit = 2000;
	uint16_t coil_addr, coil_qty, num_bytes, coil_cntr;
	uint8_t bit_mask;
	uint8_t *presp;
	bool state;

	if (dlen != 4) {
		return false; /* 静默丢弃 */
	}

	coil_addr = io_get_be16(&d[0]);
	coil_qty = io_get_be16(&d[2]);

	/* 每请求数量上限 */
	if (coil_qty == 0 || coil_qty > coils_limit) {
		return exc_rsp(MB_FC01_COIL_RD, MB_EXC_ILLEGAL_DATA_VAL,
			       out, out_len);
	}

	/* 响应字节数: 每 8 线圈 1 字节 */
	num_bytes = ((coil_qty - 1) / 8) + 1;
	out[0] = MB_FC01_COIL_RD;
	out[1] = (uint8_t)num_bytes;
	memset(&out[2], 0, num_bytes);
	*out_len = num_bytes + 2;

	presp = &out[2];
	bit_mask = 1u << 0;
	coil_cntr = 0;

	while (coil_cntr < coil_qty) {
		if (coil_rd_cb(coil_addr, &state) != 0) {
			return exc_rsp(MB_FC01_COIL_RD, MB_EXC_ILLEGAL_DATA_ADDR,
				       out, out_len);
		}

		if (state) {
			*presp |= bit_mask;
		}

		coil_addr++;
		coil_cntr++;
		if ((coil_cntr % 8) == 0) {
			bit_mask = 1u << 0;
			presp++;
		} else {
			bit_mask <<= 1;
		}
	}

	return true;
}

/* FC02 (0x02) Read Discrete Inputs — 结构同 FC01 */
static bool fc02_di_read(const uint8_t *d, uint16_t dlen,
			 uint8_t *out, uint16_t *out_len)
{
	const uint16_t di_limit = 2000;
	uint16_t di_addr, di_qty, num_bytes, di_cntr;
	uint8_t bit_mask;
	uint8_t *presp;
	bool state;

	if (dlen != 4) {
		return false;
	}

	di_addr = io_get_be16(&d[0]);
	di_qty = io_get_be16(&d[2]);

	if (di_qty == 0 || di_qty > di_limit) {
		return exc_rsp(MB_FC02_DI_RD, MB_EXC_ILLEGAL_DATA_VAL,
			       out, out_len);
	}

	num_bytes = ((di_qty - 1) / 8) + 1;
	out[0] = MB_FC02_DI_RD;
	out[1] = (uint8_t)num_bytes;
	memset(&out[2], 0, num_bytes);
	*out_len = num_bytes + 2;

	presp = &out[2];
	bit_mask = 1u << 0;
	di_cntr = 0;

	while (di_cntr < di_qty) {
		if (discrete_input_rd_cb(di_addr, &state) != 0) {
			return exc_rsp(MB_FC02_DI_RD, MB_EXC_ILLEGAL_DATA_ADDR,
				       out, out_len);
		}

		if (state) {
			*presp |= bit_mask;
		}

		di_addr++;
		di_cntr++;
		if ((di_cntr % 8) == 0) {
			bit_mask = 1u << 0;
			presp++;
		} else {
			bit_mask <<= 1;
		}
	}

	return true;
}

/* FC03 (0x03) Read Holding Registers
 *
 * Request:  fc | 起始地址(2) | 寄存器数(2)
 * Response: fc | 字节数(1) | 寄存器值 N*2
 */
static bool fc03_hreg_read(const uint8_t *d, uint16_t dlen,
			   uint8_t *out, uint16_t *out_len)
{
	const uint16_t regs_limit = 125;
	uint16_t reg_addr, reg_qty, num_bytes;
	uint8_t *presp;

	if (dlen != 4) {
		return false;
	}

	reg_addr = io_get_be16(&d[0]);
	reg_qty = io_get_be16(&d[2]);

	if (reg_qty == 0 || reg_qty > regs_limit) {
		return exc_rsp(MB_FC03_HOLDING_REG_RD, MB_EXC_ILLEGAL_DATA_VAL,
			       out, out_len);
	}

	num_bytes = reg_qty * sizeof(uint16_t);

	if (reg_addr >= MB_FP_EXTENSIONS_ADDR) {
		/* FP 扩展区无 fp 回调 -> ILLEGAL_FC */
		return exc_rsp(MB_FC03_HOLDING_REG_RD, MB_EXC_ILLEGAL_FC,
			       out, out_len);
	}

	out[0] = MB_FC03_HOLDING_REG_RD;
	out[1] = (uint8_t)num_bytes;
	*out_len = num_bytes + 2;

	presp = &out[2];
	while (reg_qty > 0) {
		uint16_t reg;

		if (holding_reg_rd_cb(reg_addr, &reg) != 0) {
			return exc_rsp(MB_FC03_HOLDING_REG_RD,
				       MB_EXC_ILLEGAL_DATA_ADDR, out, out_len);
		}
		io_put_be16(reg, presp);
		presp += sizeof(uint16_t);

		reg_addr++;
		reg_qty--;
	}

	return true;
}

/* FC04 (0x04) Read Input Registers — 结构同 FC03 */
static bool fc04_inreg_read(const uint8_t *d, uint16_t dlen,
			    uint8_t *out, uint16_t *out_len)
{
	const uint16_t regs_limit = 125;
	uint16_t reg_addr, reg_qty, num_bytes;
	uint8_t *presp;

	if (dlen != 4) {
		return false;
	}

	reg_addr = io_get_be16(&d[0]);
	reg_qty = io_get_be16(&d[2]);

	if (reg_qty == 0 || reg_qty > regs_limit) {
		return exc_rsp(MB_FC04_IN_REG_RD, MB_EXC_ILLEGAL_DATA_VAL,
			       out, out_len);
	}

	num_bytes = reg_qty * sizeof(uint16_t);

	if (reg_addr >= MB_FP_EXTENSIONS_ADDR) {
		return exc_rsp(MB_FC04_IN_REG_RD, MB_EXC_ILLEGAL_FC,
			       out, out_len);
	}

	out[0] = MB_FC04_IN_REG_RD;
	out[1] = (uint8_t)num_bytes;
	*out_len = num_bytes + 2;

	presp = &out[2];
	while (reg_qty > 0) {
		uint16_t reg;

		if (input_reg_rd_cb(reg_addr, &reg) != 0) {
			return exc_rsp(MB_FC04_IN_REG_RD,
				       MB_EXC_ILLEGAL_DATA_ADDR, out, out_len);
		}
		io_put_be16(reg, presp);
		presp += sizeof(uint16_t);

		reg_addr++;
		reg_qty--;
	}

	return true;
}

/* FC05 (0x05) Write Single Coil
 *
 * Request/Response: fc | 输出地址(2) | 输出值(2)
 * 值 0x0000 = OFF, 其他一律 ON; 响应回显请求的原始值
 */
static bool fc05_coil_write(const uint8_t *d, uint16_t dlen,
			    uint8_t *out, uint16_t *out_len)
{
	uint16_t coil_addr, coil_val;
	bool coil_state;

	if (dlen != 4) {
		return false;
	}

	coil_addr = io_get_be16(&d[0]);
	coil_val = io_get_be16(&d[2]);

	coil_state = (coil_val != MB_COIL_OFF_CODE);

	if (coil_wr_cb(coil_addr, coil_state) != 0) {
		return exc_rsp(MB_FC05_COIL_WR, MB_EXC_ILLEGAL_DATA_ADDR,
			       out, out_len);
	}

	out[0] = MB_FC05_COIL_WR;
	io_put_be16(coil_addr, &out[1]);
	io_put_be16(coil_val, &out[3]); /* 回显原始值 */
	*out_len = 5;
	return true;
}

/* FC06 (0x06) Write Single Register
 *
 * Request/Response: fc | 寄存器地址(2) | 寄存器值(2)
 * 无 FP 分支: addr >= 5000 走整数写回调, 越界 -> 0x02
 */
static bool fc06_hreg_write(const uint8_t *d, uint16_t dlen,
			    uint8_t *out, uint16_t *out_len)
{
	uint16_t reg_addr, reg_val;

	if (dlen != 4) {
		return false;
	}

	reg_addr = io_get_be16(&d[0]);
	reg_val = io_get_be16(&d[2]);

	if (holding_reg_wr_cb(reg_addr, reg_val) != 0) {
		return exc_rsp(MB_FC06_HOLDING_REG_WR, MB_EXC_ILLEGAL_DATA_ADDR,
			       out, out_len);
	}

	out[0] = MB_FC06_HOLDING_REG_WR;
	io_put_be16(reg_addr, &out[1]);
	io_put_be16(reg_val, &out[3]);
	*out_len = 5;
	return true;
}

/* FC08 (0x08) Diagnostics
 *
 * Request/Response: fc | 子功能(2) | 数据(2)
 * 0x00 回显数据; 0x0A 清零计数; 0x0B..0x0F 返回对应计数器; 其他 -> 0x01
 */
static bool fc08_diagnostics(const uint8_t *d, uint16_t dlen,
			     uint8_t *out, uint16_t *out_len)
{
	uint16_t sfunc, data;

	if (dlen != 4) {
		return false;
	}

	sfunc = io_get_be16(&d[0]);
	data = io_get_be16(&d[2]);

	switch (sfunc) {
	case MB_FC08_SUBF_QUERY:
		break;

	case MB_FC08_SUBF_CLR_CTR:
		memset(diag_ctr, 0, sizeof(diag_ctr));
		break;

	case MB_FC08_SUBF_BUS_MSG_CTR:
		data = diag_ctr[MB_DIAG_BUS_MSG];
		break;

	case MB_FC08_SUBF_BUS_CRC_CTR:
		data = diag_ctr[MB_DIAG_CRC_ERR];
		break;

	case MB_FC08_SUBF_BUS_EXCEPT_CTR:
		data = diag_ctr[MB_DIAG_EXC];
		break;

	case MB_FC08_SUBF_SERVER_MSG_CTR:
		data = diag_ctr[MB_DIAG_SRV_MSG];
		break;

	case MB_FC08_SUBF_SERVER_NO_RESP_CTR:
		data = diag_ctr[MB_DIAG_NO_RESP];
		break;

	default:
		return exc_rsp(MB_FC08_DIAGNOSTICS, MB_EXC_ILLEGAL_FC,
			       out, out_len);
	}

	out[0] = MB_FC08_DIAGNOSTICS;
	io_put_be16(sfunc, &out[1]);
	io_put_be16(data, &out[3]);
	*out_len = 5;
	return true;
}

/* FC15 (0x0F) Write Multiple Coils
 *
 * Request:  fc | 起始地址(2) | 输出数(2) | 字节数(1) | 线圈值 N*1
 * Response: fc | 起始地址(2) | 输出数(2)
 * 逐线圈写, 失败前的写入保留
 */
static bool fc15_coils_write(const uint8_t *d, uint16_t dlen,
			     uint8_t *out, uint16_t *out_len)
{
	const uint16_t coils_limit = 2000;
	uint16_t coil_addr, coil_qty, num_bytes, coil_cntr;
	uint8_t temp, data_ix;
	bool coil_state;

	if (dlen < 6) {
		return false;
	}

	coil_addr = io_get_be16(&d[0]);
	coil_qty = io_get_be16(&d[2]);
	num_bytes = d[4];

	if (coil_qty == 0 || coil_qty > coils_limit) {
		return exc_rsp(MB_FC15_COILS_WR, MB_EXC_ILLEGAL_DATA_VAL,
			       out, out_len);
	}

	/* 字节数须与线圈数匹配, 且 PDU 长度自洽 */
	if (((((coil_qty - 1) / 8) + 1) != num_bytes) ||
	    (dlen != (uint16_t)(num_bytes + 5))) {
		return exc_rsp(MB_FC15_COILS_WR, MB_EXC_ILLEGAL_DATA_VAL,
			       out, out_len);
	}

	coil_cntr = 0;
	temp = 0;
	data_ix = 5; /* 第 1 个数据字节是 PDU 第 6 字节 */
	while (coil_cntr < coil_qty) {
		if ((coil_cntr % 8) == 0) {
			temp = d[data_ix++];
		}

		coil_state = (temp & 0x01) != 0;

		if (coil_wr_cb(coil_addr + coil_cntr, coil_state) != 0) {
			return exc_rsp(MB_FC15_COILS_WR,
				       MB_EXC_ILLEGAL_DATA_ADDR, out, out_len);
		}

		temp >>= 1;
		coil_cntr++;
	}

	out[0] = MB_FC15_COILS_WR;
	io_put_be16(coil_addr, &out[1]);
	io_put_be16(coil_qty, &out[3]);
	*out_len = 5;
	return true;
}

/* FC16 (0x10) Write Multiple Registers
 *
 * Request:  fc | 起始地址(2) | 寄存器数(2) | 字节数(1) | 寄存器值 N*2
 * Response: fc | 起始地址(2) | 寄存器数(2)
 * 逐寄存器写, 失败前的写入保留
 */
static bool fc16_hregs_write(const uint8_t *d, uint16_t dlen,
			     uint8_t *out, uint16_t *out_len)
{
	const uint16_t regs_limit = 125;
	uint16_t reg_addr, reg_qty, num_bytes;
	const uint8_t *prx;

	if (dlen < 6) {
		return false;
	}

	reg_addr = io_get_be16(&d[0]);
	reg_qty = io_get_be16(&d[2]);
	num_bytes = d[4];

	if (reg_qty == 0 || reg_qty > regs_limit) {
		return exc_rsp(MB_FC16_HOLDING_REGS_WR, MB_EXC_ILLEGAL_DATA_VAL,
			       out, out_len);
	}

	if (reg_addr >= MB_FP_EXTENSIONS_ADDR) {
		/* FP 扩展区无 fp 回调 -> ILLEGAL_FC */
		return exc_rsp(MB_FC16_HOLDING_REGS_WR, MB_EXC_ILLEGAL_FC,
			       out, out_len);
	}

	/* PDU 长度与字节数自洽: length-5 != num_bytes -> 0x03 */
	if ((dlen - 5) != num_bytes) {
		return exc_rsp(MB_FC16_HOLDING_REGS_WR, MB_EXC_ILLEGAL_DATA_VAL,
			       out, out_len);
	}

	/* 字节数整除判断: num_bytes/reg_qty != 2 -> 0x03 (整数除法:
	 * 尾部多余字节通过, 如 qty=2/nbytes=5; reg_qty>=1 已由上面的
	 * 数量门保证) */
	if ((uint16_t)(num_bytes / reg_qty) != 2) {
		return exc_rsp(MB_FC16_HOLDING_REGS_WR, MB_EXC_ILLEGAL_DATA_VAL,
			       out, out_len);
	}

	prx = &d[5];
	for (uint16_t reg_cntr = 0; reg_cntr < reg_qty; reg_cntr++) {
		uint16_t reg_val = io_get_be16(prx);

		prx += sizeof(uint16_t);
		if (holding_reg_wr_cb(reg_addr + reg_cntr, reg_val) != 0) {
			return exc_rsp(MB_FC16_HOLDING_REGS_WR,
				       MB_EXC_ILLEGAL_DATA_ADDR, out, out_len);
		}
	}

	out[0] = MB_FC16_HOLDING_REGS_WR;
	io_put_be16(reg_addr, &out[1]);
	io_put_be16(reg_qty, &out[3]);
	*out_len = 5;
	return true;
}

/* ==================== PDU 分发 ==================== */

bool mb_server_process(const uint8_t *in, uint16_t in_len,
		       uint8_t *out, uint16_t *out_len)
{
	uint8_t fc;
	const uint8_t *d;
	uint16_t dlen;

	if (in == NULL || out == NULL || out_len == NULL || in_len == 0) {
		return false;
	}

	fc = in[0];
	d = in + 1;
	dlen = in_len - 1;

	/* 诊断计数: 到达本解码器的均为发往本机的有效 PDU */
	diag_ctr[MB_DIAG_BUS_MSG]++;
	diag_ctr[MB_DIAG_SRV_MSG]++;

	switch (fc) {
	case MB_FC01_COIL_RD:
		return fc01_coil_read(d, dlen, out, out_len);
	case MB_FC02_DI_RD:
		return fc02_di_read(d, dlen, out, out_len);
	case MB_FC03_HOLDING_REG_RD:
		return fc03_hreg_read(d, dlen, out, out_len);
	case MB_FC04_IN_REG_RD:
		return fc04_inreg_read(d, dlen, out, out_len);
	case MB_FC05_COIL_WR:
		return fc05_coil_write(d, dlen, out, out_len);
	case MB_FC06_HOLDING_REG_WR:
		return fc06_hreg_write(d, dlen, out, out_len);
	case MB_FC08_DIAGNOSTICS:
		return fc08_diagnostics(d, dlen, out, out_len);
	case MB_FC15_COILS_WR:
		return fc15_coils_write(d, dlen, out, out_len);
	case MB_FC16_HOLDING_REGS_WR:
		return fc16_hregs_write(d, dlen, out, out_len);
	default:
		/* 未知 FC: 自定义 FC 表为空 -> ILLEGAL_FC */
		return exc_rsp(fc, MB_EXC_ILLEGAL_FC, out, out_len);
	}
}
