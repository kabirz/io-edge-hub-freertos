/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP 配置协议命令层 (配置端口 8600; Zephyr 版
 * applications/io-edge-hub/src/udp.c 的 FreeRTOS 移植, 传输层/路由在
 * udp_task.c)。本模块 OS-free, host 可测。
 *
 * 帧格式 (无 magic/长度/CRC, 对齐 Zephyr udp_fw_upgrade 库):
 *   请求 [cmd 1B][data...], 应答 [cmd 1B][data...], 应答缓冲 64B
 *   (数据 <=63B)。已知命令一律应答 (非法输入 -> ok=0); 未知命令
 *   返回 0 静默 (对齐库 RX 线程 "unhandled -> LOG_WRN 不回复")。
 */

#ifndef UDP_CFG_H
#define UDP_CFG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 命令码 (Zephyr 版 udp.h + udp_fw_upgrade.h 逐值保留):
 * 0x04/0x05 对齐 Zephyr fw_cmd (一期无 MCUboot, 直接在 app 层处理);
 * 0x10-0x19 对齐 Zephyr udp_app_cmd */
enum udp_cmd {
	UDP_CMD_GET_VERSION   = 0x04, /*                           -> "v<major>.<minor>.<patch>_<git>" */
	UDP_CMD_REBOOT        = 0x05, /* 两步确认 (5s 窗)          -> [0x05][ok] */
	UDP_CMD_SET_IP        = 0x10, /* [a][b][c][d]              -> [0x10][ok] */
	UDP_CMD_GET_IP        = 0x11, /*                           -> [0x11][a][b][c][d] */
	UDP_CMD_SET_MODBUS    = 0x12, /* [slave 1B][baud BE16]     -> [0x12][ok] */
	UDP_CMD_GET_MODBUS    = 0x13, /*                           -> [0x13][slave][baud BE16] */
	UDP_CMD_SET_TIME      = 0x14, /* [unix BE32]               -> [0x14][ok] */
	UDP_CMD_FACTORY_RESET = 0x19, /* 两步确认 (5s 窗)          -> [0x19][ok] */
};

/* 处理一条命令; 返回应答长度 (0=静默), reply[0]=cmd。
 * data 与 len 为去掉 cmd 字节后的净荷 (可为 NULL/0)。cap 不足时返回 0
 * (调用方缓冲 64B, 协议最大应答 5B, 实际不可达的防御路径)。 */
uint16_t udp_app_cmd(uint8_t cmd, const uint8_t *data, uint16_t len,
		     uint8_t *reply, uint16_t cap);

/* 跨网段命令白名单: 仅 GET_IP 0x11 (网络发现), 对齐 Zephyr
 * udp_fw_allow_broadcast_cmd(UDP_CMD_GET_IP)。白名单外的命令跨网段
 * 接收时被 udp_task 静默丢弃 -- 不执行不应答。 */
bool udp_cmd_bcast_allowed(uint8_t cmd);

/* FACTORY_RESET 两步确认状态复位, 含重启待办标志 (模拟重新上电;
 * host 测试钩子) */
void udp_cfg_reset_pending(void);

/* FACTORY_RESET 确认步已执行 (重启待办)。传输层契约: 必须先把
 * udp_app_cmd 返回的应答 sendto 上线, 然后查询本标志 -- 为真时调用
 * history_sync() + io_reboot_cold() (对齐 Zephyr udp.c 顺序: 应答 ->
 * 刷历史 -> 100ms -> 重启, 延时在 io_reboot_cold 实现内)。命令层不
 * 自行重启, 否则 [0x19][01] 随重启上不了线。真实重启不返回, 标志仅由
 * 重新上电 (udp_cfg_reset_pending) 复位。 */
bool udp_cfg_reboot_pending(void);

/* 毫秒时钟钩子 (FACTORY_RESET 两步确认计时源, Zephyr k_uptime_get 的
 * 对应物)。target 由 udp_cfg_start() 绑 xTaskGetTickCount 换算值;
 * host 测试绑可控计数器。未绑定时内建回退恒 0 -- 每条命令都落在
 * "开机 5s 内"确认窗 (单命令立即执行怪癖的退化形态)。 */
extern uint32_t (*udp_now_ms)(void);

/* 启动 UDP 配置任务 (target-only, 实现在 src/net/udp_task.c;
 * host 测试不链接)。 */
void udp_cfg_start(void);

#ifdef __cplusplus
}
#endif

#endif /* UDP_CFG_H */
