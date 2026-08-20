/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP 配置服务传输层 (W5500 socket Sn0/SN_UDP_CFG, 端口 8600; Zephyr 版
 * udp_fw_upgrade 库 RX 线程 + udp_fw_reply 路由部分的 FreeRTOS 移植,
 * 命令语义在 udp_cfg.c, host 可测; 本文件只做收发与路由)。
 *
 * 收包路径 (阻塞 socket + SO_RECVBUF 门控, 同 src/modbus/tcp.c):
 *   - socket() 不带 SF_IO_NONBLOCK (阻塞模式); 上游 socket.c 的
 *     recvfrom() 在 RSR==0 时阻塞模式会死循环忙等、非阻塞模式直接
 *     返回 SOCK_BUSY -- 不改 vendored 库, 收包前先 getsockopt(SO_RECVBUF)
 *     探明已到字节数, >0 才调 recvfrom() (此时 RSR>0 立即返回一个
 *     完整数据报)。
 *   - RX 缓冲取整块 socket RX 窗口 (2KB): recvfrom 的 len 小于数据报
 *     长度时会把余量留给下一次调用, 且续读分支不回填 addr/port
 *     (socket.c sock_remained_size 路径) -- 缓冲 >= 任意单报文 (受
 *     2KB socket 缓冲上限) 保证一次收完, 不出现半报文续读。
 *
 * 路由 (对齐 Zephyr udp_fw_reply / RX 线程的跨网段放行):
 *   - 同 /24 网段 (remote & /24 == local & /24, local 取 holding reg
 *     0x0A-0x0D, 掩码固定 /24): 单播回源 IP:源端口;
 *   - 跨网段: 命令在 udp_cmd_bcast_allowed 白名单 (仅 GET_IP 网络发现)
 *     才执行, 应答广播到 255.255.255.255:8601 (config+1); 白名单外
 *     静默丢弃 -- 不执行不应答 (避免跨网段误触发配置/复位)。
 *   - FACTORY_RESET 确认步: udp_app_cmd 只擦配置 + 置
 *     udp_cfg_reboot_pending() 标志; 应答 sendto 上线之后本层才
 *     history_sync() + io_reboot_cold() (对齐 Zephyr udp.c 顺序:
 *     sendto -> sync -> 100ms -> reboot)。
 *
 * 广播接收: socket() 不带 SF_BROAD_BLOCK, W5500 默认接收目的端口 8600
 * 的广播报文 (上位机跨网段发现走此路径)。
 */

#include <stdint.h>
#include <stdbool.h>

#include "FreeRTOS.h"
#include "task.h"

/* ioLibrary socket.h: 上游头声明了 W6x00 专用的 static 函数却不在本芯片
 * 配置下定义 -> -Wunused-function 警告在 TU 末尾发出 (pragma 包不住),
 * 由 CMake 对本文件单独加 -Wno-unused-function (同 src/modbus/tcp.c) */
#include "socket.h" /* ioLibrary (include 路径唯一命中 deps/ioLibrary/Ethernet) */

#include "w5500.h"  /* SN_UDP_CFG / socket 布局 (src/include) */
#include "udp_cfg.h"
#include "init.h"
#include "io_hooks.h" /* history_sync / io_reboot_cold (0x19 重启路径) */

/* LOG 占位 (Task 13 替换为真实日志) */
#define LOG_INF(...) do {} while (0)
#define LOG_WRN(...) do {} while (0)

#define UDP_CFG_PORT       8600u /* 配置端口 (Zephyr CONFIG_UDP_FW_CONFIG_PORT) */
#define UDP_CFG_BCAST_PORT (UDP_CFG_PORT + 1u) /* 跨网段应答端口 (config+1) */
#define UDP_CFG_POLL_MS    100u  /* 轮询周期 */
#define UDP_CFG_RX_MAX     2048u /* = socket RX 窗口 (2KB), 见文件头 */

/* 任务 (prio 4, 栈 512 字 = 2048B: 收发直落 udp_app_cmd, 无本地大数组,
 * RX/应答缓冲均为 static) */
static StackType_t udp_cfg_stack[512];
static StaticTask_t udp_cfg_tcb;

/* FACTORY_RESET 两步确认计时源: tick -> ms (configTICK_RATE_HZ=1000 时
 * 恒等; 换算保持对任意 tick 率正确, 回绕差值语义见 udp_cfg.c) */
static uint32_t udp_now_ms_target(void)
{
	return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}

/* 发送方与本机是否同 /24 网段 (本机 IP 取 holding reg 0x0A-0x0D;
 * Zephyr 版经 net_if 查真实掩码, 本版按部署约定固定 /24) */
static bool same_subnet24(const uint8_t rip[4])
{
	return rip[0] == (uint8_t)get_holding_reg(HOLDING_IP_OCTET1_IDX) &&
	       rip[1] == (uint8_t)get_holding_reg(HOLDING_IP_OCTET2_IDX) &&
	       rip[2] == (uint8_t)get_holding_reg(HOLDING_IP_OCTET3_IDX);
}

/* 配置 socket 维持: 非 SOCK_UDP 即重开 (blocking, 不带 SF_IO_NONBLOCK;
 * 端口 8600 由 socket() 的 port 参数绑定, W5500 无独立 bind) */
static bool udp_sock_open(void)
{
	uint8_t sr = SOCK_CLOSED;

	(void)getsockopt(SN_UDP_CFG, SO_STATUS, &sr);
	if (sr == SOCK_UDP) {
		return true;
	}
	if (sr != SOCK_CLOSED) {
		(void)close(SN_UDP_CFG);
	}
	if (socket(SN_UDP_CFG, Sn_MR_UDP, UDP_CFG_PORT, 0x00) ==
	    (int8_t)SN_UDP_CFG) {
		LOG_INF("udpcfg: port %u listening", UDP_CFG_PORT);
		return true;
	}
	LOG_WRN("udpcfg: socket open failed");
	return false; /* 下轮 poll 重试 */
}

static void udp_cfg_task(void *arg)
{
	static uint8_t rx[UDP_CFG_RX_MAX]; /* 大于任何单报文, 见文件头 */
	static uint8_t rep[64];            /* 协议应答缓冲 (数据 <=63B) */
	static const uint8_t bcast_ip[4] = {255, 255, 255, 255};

	(void)arg;

	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(UDP_CFG_POLL_MS));

		if (!udp_sock_open()) {
			continue;
		}

		/* 排空本轮已缓冲的数据报 (每条即时应答, 不等下轮 poll) */
		for (;;) {
			uint16_t avail = 0;
			uint8_t rip[4];
			uint16_t rport = 0;
			uint16_t rlen;
			bool same;
			int32_t n;

			/* 只在确有数据时调 recvfrom (阻塞模式下 RSR==0
			 * 会忙等, 见文件头) */
			(void)getsockopt(SN_UDP_CFG, SO_RECVBUF, &avail);
			if (avail == 0u) {
				break;
			}

			n = recvfrom(SN_UDP_CFG, rx, sizeof(rx), rip, &rport);
			if (n < 0) {
				(void)close(SN_UDP_CFG); /* 错误路径多已内部 close */
				break; /* 下轮 poll 重开 */
			}
			if (n == 0) {
				continue; /* 空数据报: 无 cmd 字节, 丢弃 */
			}

			/* 跨网段白名单外的命令: 静默丢弃, 不执行不应答
			 * (对齐 Zephyr 库 RX 线程 drop, udp_app_cmd 不调用) */
			same = same_subnet24(rip);
			if (!same && !udp_cmd_bcast_allowed(rx[0])) {
				LOG_WRN("udpcfg: drop cross-subnet cmd 0x%02x",
					rx[0]);
				continue;
			}

			rlen = udp_app_cmd(rx[0], &rx[1], (uint16_t)n - 1u,
					   rep, sizeof(rep));
			if (rlen == 0u) {
				continue; /* 未知命令静默 */
			}

			if (same) {
				/* 同网段: 单播回源地址 */
				(void)sendto(SN_UDP_CFG, rep, rlen, rip,
					     rport);
			} else {
				/* 跨网段: 定向广播应答 (config+1) */
				(void)sendto(SN_UDP_CFG, rep, rlen,
					     (uint8_t *)bcast_ip,
					     UDP_CFG_BCAST_PORT);
			}

			/* FACTORY_RESET 确认步 (契约见 udp_cfg.h): 应答已
			 * 上线, 现在刷历史 + 冷重启 -- 顺序对齐 Zephyr
			 * (sendto -> sync -> 100ms -> reboot, 延时与不复
			 * 返回在 io_reboot_cold 实现内) */
			if (udp_cfg_reboot_pending()) {
				history_sync();
				io_reboot_cold();
			}
		}
	}
}

void udp_cfg_start(void)
{
	udp_now_ms = udp_now_ms_target; /* 绑定真实时钟 (host 测试自绑) */
	xTaskCreateStatic(udp_cfg_task, "udpcfg", 512, NULL, 4,
			  udp_cfg_stack, &udp_cfg_tcb);
	LOG_INF("udpcfg: task started");
}
