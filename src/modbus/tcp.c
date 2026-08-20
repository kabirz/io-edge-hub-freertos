/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modbus TCP 服务器 (LwIP tcp 回调模型, Zephyr 版 src/modbus/tcp.c 的移植;
 * ADU 逻辑抽到 mbtcp_adu.c 主机直测, 本文件只做连接管理):
 *
 *   - LwIP RAW API 回调全部在 tcpip 线程上下文执行, 无跨线程锁问题;
 *     mb_tcp_start() 仅通过 tcpip_callback 建立 listener。
 *   - 并发主站上限 2 (MB_TCP_MAX_EST): 满 2 时 accept 回调 tcp_abort
 *     新连接 (对齐旧 ioLibrary 版"连接数满不再 LISTEN"语义)。
 *   - 帧累积: 收满 6+MIN(MBAP length,256) 字节即为完整 ADU (缓冲 262B,
 *     静态 per-conn); 一个 pbuf 含多帧时逐帧消费。半帧 500ms 无进展
 *     -> 断开 (tcp_poll 每 0.5s 检查, 防恶意主站声明大长度后挂死)。
 *   - 完整帧 -> mbtcp_adu_process (proto 校验/unit 改写/广播抑制/诊断
 *     计数都在其中) -> 应答 MBAP+PDU 单次 tcp_write (拆两段会被部分
 *     上位机按"一段=一帧"误解析; <MSS 且 LWIP_NETIF_TX_SINGLE_PBUF=1,
 *     单段单帧)。tcp_write ERR_MEM 时暂存 per-conn 缓冲, poll 回调
 *     重试 (对应旧版 tx_pending 语义)。
 *   - 对端关闭 (recv p==NULL) / err 回调 / poll 超时 -> 关闭回收;
 *     err 回调时栈已释放 pcb, 仅清槽位。
 *   - 从站号 srv_unit 启动时读 holding_reg[0x09] 一次, 之后改寄存器
 *     需重启生效 (与 Zephyr init_modbus_server 的启动快照一致)。
 *
 * 与 Zephyr 版的已知实现差异 (无行为影响):
 *   - 会话/客户端数: Zephyr 无上限 (backlog 16); 本版限 2 个并发主站
 *     (RAM 预算下的设计决定, 与旧 ioLibrary 版一致)。
 *   - keepalive: LWIP_TCP_KEEPALIVE=0 未启用 (Idle 连接由两端 TCP 状态
 *     机与 RST/FIN 自然回收; Zephyr 版 SO_KEEPALIVE 探测周期 30s)。
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lwip/tcp.h"
#include "lwip/tcpip.h"
#include "lwip/ip_addr.h"

#include "mbtcp_adu.h"
#include "init.h"
#include "io_bytes.h"

#include "log.h"

#define MODBUS_TCP_PORT 502

#define MB_TCP_MAX_EST      2u   /* 并发主站上限 */
#define MB_TCP_FRAME_TMO_MS 500u /* 半帧无进展断开 */

/* ==================== 连接状态 ==================== */

struct mb_conn {
	struct tcp_pcb *pcb;   /* NULL = 槽位空闲 */
	uint16_t rx_len;       /* 已累积字节 */
	uint16_t pending_len;  /* tcp_write ERR_MEM 暂存字节数, 0 = 无 */
	uint32_t t_last_ms;    /* 最近一次收到字节的时刻 (半帧超时基准) */
	uint8_t rx[MBTCP_ADU_RX_MAX];         /* 6B 头 + 256 = 262B */
	uint8_t tx_pending[MBTCP_ADU_TX_MAX]; /* ERR_MEM 暂存 (263B) */
};

static struct mb_conn mb_conns[MB_TCP_MAX_EST];
static struct tcp_pcb *listen_pcb;
static uint8_t mb_srv_unit;

static uint32_t now_ms(void)
{
	return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}

/* 目标帧长: 6B 头 + length 钳制上限 256 (头 8B 到齐后才有意义,
 * 与 mbtcp_adu 的 MIN(len,256)-2 钳制及 Zephyr 的消费字节数一致) */
static uint16_t frame_need(const struct mb_conn *c)
{
	uint16_t mbap_len = io_get_be16(&c->rx[4]);

	if (mbap_len > 256u) {
		mbap_len = 256u;
	}
	return (uint16_t)(6u + mbap_len);
}

/* 关闭并回收槽位 (回调已注销, 栈不再触发本连接) */
static void mb_conn_close(struct mb_conn *c, const char *why)
{
	if (c->pcb != NULL) {
		tcp_arg(c->pcb, NULL);
		tcp_recv(c->pcb, NULL);
		tcp_poll(c->pcb, NULL, 0);
		tcp_err(c->pcb, NULL);
		tcp_close(c->pcb);
		c->pcb = NULL;
		LOG_INF("mbtcp: closed (%s)", why);
	}
	c->rx_len = 0;
	c->pending_len = 0;
}

/* 处理一条完整 ADU: 应答单次 tcp_write; ERR_MEM 暂存到 poll 重试。
 * 返回 false = 连接已回收。 */
static bool process_frame(struct mb_conn *c)
{
	/* tcpip 线程串行使用, 无需 per-conn */
	static uint8_t tx[MBTCP_ADU_TX_MAX];
	uint16_t adu_len = 0;
	int rsp = mbtcp_adu_process(c->rx, c->rx_len, tx, sizeof(tx),
				    &adu_len, mb_srv_unit);

	c->rx_len = 0; /* 下一帧 (流水线主站在同一 pbuf 内继续) */
	if (rsp != 1) {
		return true; /* 静默 (广播/非法 proto, 判定在 mbtcp_adu) */
	}

	err_t e = tcp_write(c->pcb, tx, adu_len, TCP_WRITE_FLAG_COPY);
	if (e == ERR_OK) {
		tcp_output(c->pcb);
		return true;
	}
	if (e == ERR_MEM) {
		memcpy(c->tx_pending, tx, adu_len);
		c->pending_len = adu_len;
		return true;
	}
	mb_conn_close(c, "tcp_write");
	return false;
}

/* ==================== 连接回调 (tcpip 线程) ==================== */

static err_t mb_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
			err_t err)
{
	struct mb_conn *c = (struct mb_conn *)arg;

	if (p == NULL) {
		mb_conn_close(c, "peer closed"); /* 对端 FIN */
		return ERR_OK;
	}
	if (err != ERR_OK) {
		pbuf_free(p);
		return err;
	}

	for (struct pbuf *q = p; q != NULL; q = q->next) {
		const uint8_t *src = (const uint8_t *)q->payload;
		uint16_t rem = (uint16_t)q->len;

		while (rem > 0) {
			uint16_t want = (c->rx_len < 8u) ? 8u : frame_need(c);
			uint16_t room = (uint16_t)(want - c->rx_len);
			uint16_t n = (rem < room) ? rem : room;

			memcpy(&c->rx[c->rx_len], src, n);
			c->rx_len = (uint16_t)(c->rx_len + n);
			src += n;
			rem -= n;
			c->t_last_ms = now_ms();
			/* 攒满 8B 只是头就绪: 还须收满 frame_need 才是完整 ADU
			 * (旧版同款双条件, 防止 8B 处误触发) */
			if (c->rx_len >= 8u && c->rx_len >= frame_need(c) &&
			    !process_frame(c)) {
				/* 连接已回收, 剩余数据作废 */
				tcp_recved(pcb, p->tot_len);
				pbuf_free(p);
				return ERR_OK;
			}
		}
	}

	tcp_recved(pcb, p->tot_len); /* 窗口返还: 全部已消费 */
	pbuf_free(p);
	return ERR_OK;
}

static err_t mb_poll_cb(void *arg, struct tcp_pcb *pcb)
{
	struct mb_conn *c = (struct mb_conn *)arg;

	if (c == NULL || c->pcb == NULL) {
		return ERR_OK;
	}

	/* 冲刷 ERR_MEM 暂存应答 */
	if (c->pending_len != 0) {
		err_t e = tcp_write(pcb, c->tx_pending, c->pending_len,
				    TCP_WRITE_FLAG_COPY);
		if (e == ERR_OK) {
			tcp_output(pcb);
			c->pending_len = 0;
		} else if (e != ERR_MEM) {
			mb_conn_close(c, "tcp_write retry");
			return ERR_OK;
		}
	}

	/* 半帧 500ms 无进展: 断开回收 */
	if (c->rx_len > 0 &&
	    (now_ms() - c->t_last_ms) > MB_TCP_FRAME_TMO_MS) {
		mb_conn_close(c, "half frame");
	}
	return ERR_OK;
}

static void mb_err_cb(void *arg, err_t err)
{
	struct mb_conn *c = (struct mb_conn *)arg;

	LOG_WRN("mbtcp: conn err %d", (int)err);
	if (c != NULL) {
		/* pcb 已被栈释放, 仅清槽位 */
		c->pcb = NULL;
		c->rx_len = 0;
		c->pending_len = 0;
	}
}

static err_t mb_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
	(void)arg;

	if (err != ERR_OK || newpcb == NULL) {
		return ERR_OK;
	}

	struct mb_conn *c = NULL;
	for (uint8_t i = 0; i < MB_TCP_MAX_EST; i++) {
		if (mb_conns[i].pcb == NULL) {
			c = &mb_conns[i];
			break;
		}
	}
	if (c == NULL) {
		tcp_abort(newpcb); /* 满 2 连接: 拒绝第 3 个主站 */
		return ERR_ABRT;
	}

	memset(c, 0, offsetof(struct mb_conn, rx));
	c->pcb = newpcb;
	c->t_last_ms = now_ms();

	tcp_arg(newpcb, c);
	tcp_recv(newpcb, mb_recv_cb);
	tcp_poll(newpcb, mb_poll_cb, 1); /* interval 单位 ~0.5s */
	tcp_err(newpcb, mb_err_cb);
	LOG_INF("mbtcp: client %s:%u", ipaddr_ntoa(&newpcb->remote_ip),
		(unsigned)newpcb->remote_port);
	return ERR_OK;
}

/* ==================== listener 建立 (tcpip 线程) ==================== */

static void mb_listen_init(void *arg)
{
	(void)arg;

	struct tcp_pcb *lp = tcp_new();
	err_t e;

	if (lp == NULL) {
		LOG_ERR("mbtcp: tcp_new failed");
		return;
	}
	e = tcp_bind(lp, IP_ADDR_ANY, MODBUS_TCP_PORT);
	if (e != ERR_OK) {
		LOG_ERR("mbtcp: bind 502 failed (%d)", (int)e);
		tcp_close(lp);
		return;
	}
	listen_pcb = tcp_listen_with_backlog(lp, 1);
	tcp_accept(listen_pcb, mb_accept_cb);
	LOG_INF("mbtcp: listening on %u (LwIP)", MODBUS_TCP_PORT);
}

void mb_tcp_start(void)
{
	/* 启动快照: 之后改寄存器需重启生效 (Zephyr 语义) */
	mb_srv_unit = (uint8_t)get_holding_reg(HOLDING_SLAVE_ID_IDX);

	/* RAW API 须持 core lock: 转到 tcpip 线程执行 */
	tcpip_callback(mb_listen_init, NULL);
}
