/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP 固件升级通道入口 (target-only)。udp_task.c recv 回调先经
 * fw_udp_cmd 过滤 0x01-0x03; fw_udp_start 须在调度器运行后调用。
 */

#ifndef FW_UDP_H
#define FW_UDP_H

#include <stdbool.h>
#include <stdint.h>

#include "lwip/ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 收到固件升级命令 (tcpip 回调上下文)。返回 true = 已入队消费。 */
bool fw_udp_cmd(const uint8_t *rx, uint16_t len, const ip_addr_t *src,
		uint16_t port);

/* 创建 fw worker 任务 + 命令队列 */
void fw_udp_start(void);

#ifdef __cplusplus
}
#endif

#endif /* FW_UDP_H */
