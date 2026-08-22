/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Web 命令执行器 + 共享 JSON 构造器 (httpd.c 与 WS 推送共用)。
 * 写路径与 Modbus 回调共用 io_write_* (副作用同 FC05/FC06);
 * 重启寄存器拦截为 set_reboot_status 延迟重启, 保住 HTTP 应答。
 */

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "init.h"
#include "io_hooks.h"
#include "io_time.h"
#include "history.h"
#include "w5500_macraw.h"
#include "fw_version.h"

#include "web_json.h"
#include "web_cmds.h"

/* ==================== JSON 构造器 ==================== */

int web_build_info_json(char *buf, size_t bufsz)
{
	uint8_t mac[6] = {0};
	char mac_str[18] = "00:00:00:00:00:00";

	if (w5500_macraw_get_mac(mac)) {
		snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
			 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	}

	uint64_t lfs_free = 0, lfs_total = 0;

	history_web_usage(&lfs_free, &lfs_total);

	int n = snprintf(
		buf, bufsz,
		"{\"t\":\"info\","
		"\"version\":\"v%d.%d.%d_%s\","
		"\"build\":\"%s %s\","
		"\"board\":\"%s\","
		"\"hclk_mhz\":%u,"
		"\"flash_kb\":%d,\"sram_kb\":%d,"
		"\"mac\":\"%s\","
		"\"ip\":\"%u.%u.%u.%u\","
		"\"slave_id\":%u,\"rs485_baud\":%u,"
		"\"can_id\":%u,\"can_baud\":%u,"
		"\"uptime_ms\":%lu,\"time\":%lu,"
		"\"hist_en\":%u,"
		"\"lfs_free\":%lu,\"lfs_total\":%lu,"
		"\"net_up\":%s,"
		"\"di_ms\":%u,\"ai_ms\":%u}",
		FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH, FW_GIT_VERSION,
		__DATE__, __TIME__, "io_edge_f407vet6", 168u, 512, 192, mac_str,
		get_holding_reg(HOLDING_IP_OCTET1_IDX), get_holding_reg(HOLDING_IP_OCTET2_IDX),
		get_holding_reg(HOLDING_IP_OCTET3_IDX), get_holding_reg(HOLDING_IP_OCTET4_IDX),
		get_holding_reg(HOLDING_SLAVE_ID_IDX), get_holding_reg(HOLDING_RS485_BAUDRATE_IDX),
		get_holding_reg(HOLDING_CAN_ID_IDX), get_holding_reg(HOLDING_CAN_BAUDRATE_IDX),
		(unsigned long)pdTICKS_TO_MS(xTaskGetTickCount()),
		(unsigned long)io_now_epoch(),
		get_holding_reg(HOLDING_HISTORY_ENABLE_IDX) != 0,
		(unsigned long)lfs_free, (unsigned long)lfs_total,
		w5500_macraw_link_up() ? "true" : "false",
		get_holding_reg(HOLDING_DI_SAMPLE_MS_IDX),
		get_holding_reg(HOLDING_AI_SAMPLE_MS_IDX));
	return (n > (int)bufsz) ? (int)bufsz : n;
}

/* 实时 IO 快照 */
int web_build_io_json(char *buf, size_t bufsz)
{
	uint16_t di = get_input_reg(INPUT_DI_IDX);
	uint16_t do_v = get_holding_reg(HOLDING_DO_IDX);
	uint16_t di_en = get_holding_reg(HOLDING_DI_ENABLE_IDX);
	uint16_t ai_en = get_holding_reg(HOLDING_AI_ENABLE_IDX);
	int n = 0;

	n += snprintf(buf + n, bufsz - n, "{\"t\":\"io\",\"di\":[");
	for (int i = 0; i < DI_NUM; i++) {
		n += snprintf(buf + n, bufsz - n, "%s%u", i ? "," : "", (di >> i) & 1);
	}
	n += snprintf(buf + n, bufsz - n, "],\"do\":[");
	for (int i = 0; i < DO_NUM; i++) {
		n += snprintf(buf + n, bufsz - n, "%s%u", i ? "," : "", (do_v >> i) & 1);
	}
	n += snprintf(buf + n, bufsz - n, "],\"ai\":[");
	for (int i = 0; i < AI_NUM; i++) {
		n += snprintf(buf + n, bufsz - n, "%s%u", i ? "," : "",
			      get_input_reg(INPUT_AI0_IDX + i));
	}
	n += snprintf(buf + n, bufsz - n, "],\"di_en\":%u,\"ai_en\":%u,\"ms\":%lu}",
		      di_en, ai_en, (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount()));
	return (n > (int)bufsz) ? (int)bufsz : n;
}

int web_build_regs_json(char *buf, size_t bufsz)
{
	int n = snprintf(buf, bufsz, "{\"t\":\"regs\",\"holding\":[");

	for (int i = 0; i < MODBUS_HOLDING_REGISTER_NUMBERS; i++) {
		/* io_read_holding: 时间戳寄存器返回实时时间 (同 FC03) */
		n += snprintf(buf + n, bufsz - n, "%s%u", i ? "," : "", io_read_holding(i));
	}
	n += snprintf(buf + n, bufsz - n, "],\"input\":[");
	for (int i = 0; i < MODBUS_INPUT_REGISTER_NUMBERS; i++) {
		n += snprintf(buf + n, bufsz - n, "%s%u", i ? "," : "", get_input_reg(i));
	}
	n += snprintf(buf + n, bufsz - n, "]}");
	return (n > (int)bufsz) ? (int)bufsz : n;
}

/* ==================== 命令执行器 ==================== */

int web_cmd_exec_do(int32_t index, int32_t value)
{
	if (index < 0 || index >= DO_NUM) {
		return -1;
	}
	return io_write_do_bit((uint16_t)index, value != 0);
}

int web_cmd_exec_reg(int32_t addr, int32_t value)
{
	if (addr < 0 || addr >= MODBUS_HOLDING_REGISTER_NUMBERS || value < 0 ||
	    value > 0xFFFF) {
		return -1;
	}
	if (addr == HOLDING_REBOOT_IDX) {
		if (value) {
			set_reboot_status(true); /* 延迟重启, 保住 HTTP 应答 */
		}
		return 0;
	}
	return io_write_holding((uint16_t)addr, (uint16_t)value);
}

/* 系统配置批量写 (POST /api/cfg, 字段均可选; 校验通过才写入) */
int web_cmd_exec_cfg(const char *json, size_t len, const char **err)
{
	static const char *e_bad_ip = "invalid ip";
	static const char *e_bad_rs = "invalid rs485 baud";
	static const char *e_bad_sid = "invalid slave id";
	static const char *e_bad_can = "invalid can baud";
	static const char *e_bad_cid = "invalid can id";

	char ip_str[16];
	int32_t v;

	if (json_get_str(json, len, "ip", ip_str, sizeof(ip_str))) {
		/* 四段点分十进制 (等价 sscanf "%u.%u.%u.%u", 尾部多余
		 * 字符忽略; 手工解析避免拖入 newlib scanf) */
		unsigned int oct[4];
		const char *p = ip_str;
		int ok = 1;

		for (int i = 0; i < 4; i++) {
			char *e;
			unsigned long v = strtoul(p, &e, 10);

			if (e == p || v > 255 || (i < 3 && *e != '.')) {
				ok = 0;
				break;
			}
			oct[i] = (unsigned int)v;
			p = e + 1;
		}
		if (!ok || !ip_addr_valid((uint8_t)oct[0], (uint8_t)oct[1],
					  (uint8_t)oct[2], (uint8_t)oct[3])) {
			*err = e_bad_ip;
			return -1;
		}
		io_write_holding(HOLDING_IP_OCTET1_IDX, (uint16_t)oct[0]);
		io_write_holding(HOLDING_IP_OCTET2_IDX, (uint16_t)oct[1]);
		io_write_holding(HOLDING_IP_OCTET3_IDX, (uint16_t)oct[2]);
		io_write_holding(HOLDING_IP_OCTET4_IDX, (uint16_t)oct[3]);
	}

	if (json_get_i32(json, len, "rs485", &v)) { /* 1200..115200 */
		if (v < 1200 || v > 115200) {
			*err = e_bad_rs;
			return -1;
		}
		io_write_holding(HOLDING_RS485_BAUDRATE_IDX, (uint16_t)v);
	}

	if (json_get_i32(json, len, "sid", &v)) { /* 1..247 */
		if (v < 1 || v > 247) {
			*err = e_bad_sid;
			return -1;
		}
		io_write_holding(HOLDING_SLAVE_ID_IDX, (uint16_t)v);
	}

	if (json_get_i32(json, len, "can_bps", &v)) { /* 常用档位 */
		if (v != 50 && v != 100 && v != 125 && v != 250 && v != 500 && v != 800 &&
		    v != 1000) {
			*err = e_bad_can;
			return -1;
		}
		io_write_holding(HOLDING_CAN_BAUDRATE_IDX, (uint16_t)v);
	}

	if (json_get_i32(json, len, "can_id", &v)) { /* 标准帧 1..0x7FF */
		if (v < 1 || v > 0x7FF) {
			*err = e_bad_cid;
			return -1;
		}
		io_write_holding(HOLDING_CAN_ID_IDX, (uint16_t)v);
	}

	return 0;
}
