/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * WebSocket 实时通道 (/ws) — httpd.c 集成点声明。
 * 会话为单连接 (第二个升级请求被拒, 前端自动降级轮询, 对齐 Zephyr 版)。
 */

#ifndef __WS_H__
#define __WS_H__

#include <stdbool.h>
#include <stdint.h>

#include "lwip/tcp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ws 会话关闭回调: httpd 持有 pcb, 由其执行 conn_close */
typedef void (*ws_close_cb_t)(void);

/* 解析握手请求头。是 WS 升级请求则把 101 响应写入 resp 并返回 true。 */
bool ws_handshake(const char *req_hdr, uint16_t hdr_len,
		  char *resp, uint16_t resp_max, uint16_t *resp_len);

/* 会话接管 (101 响应已排队); pending = 握手请求之后到达的字节 */
void ws_attach(struct tcp_pcb *pcb, const uint8_t *pending, uint16_t len,
	       ws_close_cb_t on_close);

bool ws_active(void);

/* httpd 侧连接死亡时调用 (不回调 on_close, 幂等) */
void ws_detach(void);

/* httpd 回调路由: rx 字节 / 周期推送 (poll ~1s) */
void ws_feed(const uint8_t *data, uint16_t len);
void ws_poll(void);

/* 创建 fw start/end 工作任务 (长擦除/校验不占 tcpip 线程) */
void ws_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __WS_H__ */
