/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP 配置命令处理 (纯逻辑, host 可测) — Zephyr 版
 * applications/io-edge-hub/src/udp.c 的 FreeRTOS 移植:
 *
 *   - SET/GET IP (0x10/0x11)、SET/GET MODBUS (0x12/0x13)、
 *     SET_TIME (0x14)、FACTORY_RESET (0x19)
 *   - SET 类命令改 holding_reg[] 后 holding_reg_save() 持久化
 *   - 已知命令一律应答 (非法输入/长度不足 -> ok=0);
 *     未知命令 (含 0x01-0x06 固件升级, 一期无 MCUboot) 返回 0 静默,
 *     对齐 Zephyr 库 RX 线程 "unhandled -> LOG_WRN 不回复"
 *   - FACTORY_RESET 两步确认 (5s 窗), 保留 Zephyr 怪癖: 计时起点 0,
 *     开机 5s 内 (含恰好 5000ms) 的第一条命令即视为确认步, 单命令
 *     直接执行 (见 udp_factory_reset 注释)
 *
 * 与 Zephyr 版的差异:
 *   - udp_fw_reply() (库 RX 线程内同步 sendto) -> 写调用方 reply 缓冲
 *     并返回长度, 实际发送在 udp_task.c。0x19 确认步的顺序契约
 *     (见 udp_cfg_reboot_pending 注释): 本层只 擦除 -> 写应答 -> 置
 *     重启待办标志; 传输层把应答 sendto 上线之后才 history_sync() +
 *     io_reboot_cold() -- 对齐 Zephyr "应答 -> 刷历史 -> 100ms ->
 *     重启" 顺序, 保证 [0x19][01] 先上线 (在本层直接重启会把应答
 *     一起带走)。
 *   - settings_factory_reset() 带错误码 -> config_store_erase_all()
 *     无返回值: ok 恒 1 (本端口擦除 API 无失败信号)。
 *   - 100ms 重启前延时移入 target 的 io_reboot_cold() 实现 (Task 13),
 *     由传输层在 history_sync 之后调用。
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "udp_cfg.h"
#include "init.h"
#include "io_hooks.h"
#include "io_bytes.h"
#include "config_store.h"
#ifndef FW_GIT_VERSION /* host 测试通过 tests/fw_version.h 提供; target 通过 build/generated/fw_version.h */
#include "fw_version.h"
#endif

#include "log.h"

/* ==================== 时间钩子 ==================== */

/* 未绑定时的回退: 恒 0 (见 udp_cfg.h 注释) */
static uint32_t now_ms_fallback(void)
{
	return 0;
}

uint32_t (*udp_now_ms)(void) = now_ms_fallback;

/* ==================== 应答辅助 ==================== */

/* [cmd][ok] 两字节应答 (Zephyr udp_fw_reply(cmd, &ok, 1) 的对应物);
 * cap 不足返回 0 静默 (调用方缓冲 64B, 不可达的防御路径) */
static uint16_t reply_ok(uint8_t *reply, uint16_t cap, uint8_t cmd, uint8_t ok)
{
	if (cap < 2u) {
		return 0;
	}
	reply[0] = cmd;
	reply[1] = ok;
	return 2;
}

/* ==================== FACTORY_RESET 两步确认 ==================== */

/* Zephyr 版逐语义移植 (udp.c static factory_reset_pending_ms /
 * factory_reset_confirmed): 确认判据是 "(now - pending) > 5000" 才算
 * 首步。pending 起点 0 -> 开机 5s 内 (含恰好 5000ms) 的第一条命令
 * 落入确认分支直接执行 = 单命令复位的怪癖, 有意保留。
 * uint32 减法对回绕安全 (真实流逝 < 2^32 ms 时差值正确)。 */
static uint32_t factory_reset_pending_ms;
static bool factory_reset_confirmed;
/* 确认步已执行: 传输层重启待办标志 (契约见 udp_cfg_reboot_pending) */
static volatile bool factory_reset_reboot_pending;

/* REBOOT 立即重启标志 (对齐 Zephyr FW_CMD_REBOOT, 无两步确认) */
static volatile bool reboot_reboot_pending;

void udp_cfg_reset_pending(void)
{
	factory_reset_pending_ms = 0;
	factory_reset_confirmed = false;
	factory_reset_reboot_pending = false;
	reboot_reboot_pending = false;
}

bool udp_cfg_reboot_pending(void)
{
	return factory_reset_reboot_pending || reboot_reboot_pending;
}

static uint16_t udp_factory_reset(uint8_t *reply, uint16_t cap, uint8_t cmd)
{
	if (!factory_reset_confirmed) {
		uint32_t now = udp_now_ms();

		if ((now - factory_reset_pending_ms) > 5000u) {
			/* 首条 (或距上条 >5s): 记时间, 等第二条确认 */
			factory_reset_pending_ms = now;
			LOG_INF("FACTORY_RESET: send again within 5s to confirm");
			return reply_ok(reply, cap, cmd, 0);
		}
		factory_reset_confirmed = true;
	}

	/* 确认步 (5s 内第二条, 或开机 5s 内首条): 擦配置 -> 写应答 -> 置
	 * 传输层重启待办。history_sync + io_reboot_cold 不在本层调用:
	 * 传输层必须先把 [0x19][01] sendto 上线, 再按标志执行 (Zephyr
	 * 顺序: 应答 -> 刷历史 -> 100ms -> 重启; 延时在 io_reboot_cold
	 * 实现内), 否则应答随重启丢失。 */
	config_store_erase_all();
	factory_reset_confirmed = false;
	factory_reset_reboot_pending = true;
	return reply_ok(reply, cap, cmd, 1);
}

/* ==================== 命令分发 ==================== */

uint16_t udp_app_cmd(uint8_t cmd, const uint8_t *data, uint16_t len,
		     uint8_t *reply, uint16_t cap)
{
	switch (cmd) {
	case UDP_CMD_SET_IP: {
		uint8_t ok = 0;

		if (len >= 4u && ip_addr_valid(data[0], data[1], data[2],
					       data[3])) {
			update_holding_reg(HOLDING_IP_OCTET1_IDX, data[0]);
			update_holding_reg(HOLDING_IP_OCTET2_IDX, data[1]);
			update_holding_reg(HOLDING_IP_OCTET3_IDX, data[2]);
			update_holding_reg(HOLDING_IP_OCTET4_IDX, data[3]);
			holding_reg_save();
			LOG_INF("SET_IP %u.%u.%u.%u (manual reboot required to apply)",
				data[0], data[1], data[2], data[3]);
			ok = 1;
		}
		return reply_ok(reply, cap, cmd, ok); /* 非法输入也总是应答 */
	}

	case UDP_CMD_GET_IP: {
		if (cap < 5u) {
			return 0;
		}
		reply[0] = cmd;
		reply[1] = (uint8_t)get_holding_reg(HOLDING_IP_OCTET1_IDX);
		reply[2] = (uint8_t)get_holding_reg(HOLDING_IP_OCTET2_IDX);
		reply[3] = (uint8_t)get_holding_reg(HOLDING_IP_OCTET3_IDX);
		reply[4] = (uint8_t)get_holding_reg(HOLDING_IP_OCTET4_IDX);
		return 5;
	}

	case UDP_CMD_SET_MODBUS: {
		/* slave_id(1B) + rs485_baud(BE16): 与 holding_reg 16 位
		 * 存储宽度一致 (原 4B 协议会把 >65535 的波特率截断成错误值);
		 * 重启生效 (RTU/TCP server 启动快照) */
		uint8_t ok = 0;

		if (len >= 3u) {
			update_holding_reg(HOLDING_SLAVE_ID_IDX, data[0]);
			update_holding_reg(HOLDING_RS485_BAUDRATE_IDX,
					   io_get_be16(&data[1]));
			holding_reg_save();
			ok = 1;
		}
		return reply_ok(reply, cap, cmd, ok); /* len<3 也总是应答 */
	}

	case UDP_CMD_GET_MODBUS: {
		if (cap < 4u) {
			return 0;
		}
		reply[0] = cmd;
		reply[1] = (uint8_t)get_holding_reg(HOLDING_SLAVE_ID_IDX);
		io_put_be16(get_holding_reg(HOLDING_RS485_BAUDRATE_IDX),
			    &reply[2]);
		return 4;
	}

	case UDP_CMD_SET_TIME: {
		/* unix 时间戳 (BE32) -> set_timestamp 设置 RTC + 系统时钟;
		 * 范围门 (946684800..4102444800) 在 set_timestamp 内 */
		uint8_t ok = 0;

		if (len >= 4u) {
			ok = set_timestamp((time_t)io_get_be32(data)) ? 1 : 0;
		}
		return reply_ok(reply, cap, cmd, ok); /* len<4 也总是应答 */
	}

	case UDP_CMD_GET_VERSION: {
		/* 对齐 Zephyr FW_CMD_GET_VERSION: 返回版本字符串
		 * "v<major>.<minor>.<patch>_<git_hash>" (无尾 NUL)。
		 * 局部 const 数组中转 FW_GIT_VERSION, 避免 MSVC 宏展开问题。 */
		const char git_ver[7] = FW_GIT_VERSION;
		uint16_t pos = 0;
		if (cap < 2u) {
			return 0;
		}
		reply[pos++] = cmd;
		reply[pos++] = 'v';
		if ((uint16_t)(cap - pos) < 1u) { return 0; }
		reply[pos++] = (char)('0' + FW_VERSION_MAJOR);
		reply[pos++] = '.';
		if ((uint16_t)(cap - pos) < 1u) { return 0; }
		reply[pos++] = (char)('0' + FW_VERSION_MINOR);
		reply[pos++] = '.';
		if ((uint16_t)(cap - pos) < 1u) { return 0; }
		reply[pos++] = (char)('0' + FW_VERSION_PATCH);
		reply[pos++] = '_';
		if ((uint16_t)(cap - pos) < 6u) { return 0; }
		memcpy(&reply[pos], git_ver, 6);
		pos += 6;
		return pos;
	}

	case UDP_CMD_REBOOT:
		/* 对齐 Zephyr FW_CMD_REBOOT: 立即重启 (无两步确认) */
		reboot_reboot_pending = true;
		return reply_ok(reply, cap, cmd, 1);

	case UDP_CMD_FACTORY_RESET:
		return udp_factory_reset(reply, cap, cmd);

	default:
		/* 未知命令 (含 0x01-0x06 固件升级): 静默 (Zephyr 库只告警) */
		LOG_WRN("unhandled UDP cmd: 0x%02x", cmd);
		return 0;
	}
}

bool udp_cmd_bcast_allowed(uint8_t cmd)
{
	/* 跨网段白名单: 仅 GET_IP (网络发现), 对齐 Zephyr
	 * udp_fw_allow_broadcast_cmd(UDP_CMD_GET_IP) */
	return cmd == UDP_CMD_GET_IP;
}
