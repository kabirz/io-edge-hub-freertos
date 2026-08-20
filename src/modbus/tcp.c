/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modbus TCP 服务器 (W5500 传输层, Zephyr 版 src/modbus/tcp.c 的移植;
 * ADU 逻辑抽到 mbtcp_adu.c 主机直测, 本文件只做 socket 状态机):
 *
 *   - 固定 socket 池 Sn1/2/3 (SN_MB_BASE, 不进 sn_alloc 空闲池 -- 那是
 *     二期 web/FTP 专用)。不变量: 至多 1 个 LISTEN + 至多 2 个
 *     ESTABLISHED; 连接数满 2 时不再保留 LISTEN -> 第 3 个主站连接被拒。
 *   - W5500 无 Berkeley accept(): listen socket 在 SYN 到达时自身转为
 *     ESTABLISHED -- 每轮用 getsockopt(SO_STATUS) 检测这一转换, 转换后
 *     立即在空闲 socket 上重开 LISTEN (socket()+listen(), 端口 502 由
 *     socket() 的 port 参数绑定, W5500 无独立 bind()).
 *   - 阻塞 IO (socket() 不带 SF_IO_NONBLOCK) + 收发前 getsockopt 门控:
 *     上游 socket.c 的 recv() 在检查 RSR 之前就无条件返回 SOCK_BUSY
 *     (socket.c:686-692, 非阻塞 socket 永远收不到数据) -- 不改 vendored
 *     库, 收包前先 getsockopt(SO_RECVBUF) 探明已到字节数, >0 才调
 *     recv() (阻塞模式下 RSR>0 立即返回), len 取 MIN(avail, 帧缺口)。
 *   - 帧累积: 收满 6+MIN(MBAP length,256) 字节 (与 Zephyr 的消费字节数
 *     一致, 缓冲按此上限 262B 静态分配) 即为完整 ADU; 半帧 500ms 无进展
 *     -> 断开回收 (防恶意主站声明大长度后挂死连接)。
 *   - 完整帧 -> mbtcp_adu_process (proto 校验/unit 改写/广播抑制/
 *     诊断计数都在其中) -> 应答 MBAP+PDU 已合并为单缓冲, 单次 send()
 *     (拆两段 send 会被部分上位机按"一段=一帧"误解析)。send 前用
 *     getsockopt(SO_SENDBUF) 探明 TX 空闲; 瞬态不可发 (TX 满 / 前段
 *     send 未 ACK 的 SOCK_BUSY -- 流水线主站下正常出现, 不是致命错)
 *     时暂存 per-socket 缓冲 (263B x 3), 下轮 poll (100ms) 先冲刷再收
 *     新帧; SOCKERR_* / 状态异常才是回收信号。
 *   - accept 的连接写 Sn_KPALVTR=6 (6x5s=30s 自动 keepalive, 对齐 Zephyr
 *     SO_KEEPALIVE 探测周期); 链路断开 (w5500_link_up()==false) 时暂停
 *     accept 与收包 -- 已建连接由 W5500 链路层超时/keepalive 自然回收,
 *     不主动踢 (对齐 Zephyr 只拒新连接)。
 *   - 从站号 srv_unit 任务启动时读 holding_reg[0x09] 一次, 之后改寄存器
 *     需重启生效 (与 Zephyr init_modbus_server 的启动快照一致)。
 *
 * 与 Zephyr 版的已知实现差异 (无行为影响):
 *   - SO_REUSEADDR: W5500 无此选项; 槽位回收用 close() 后重开 socket(),
 *     TIME_WAIT 由 close 的 CR_CLOSE 语义兜住。
 *   - 会话/客户端数: Zephyr 无上限 (backlog 16); 本版限 2 个并发主站
 *     (3 socket 预算下的设计决定)。
 */

#include <stdbool.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

/* ioLibrary socket.h: 上游头声明了 W6x00 专用的 static 函数却不在本芯片
 * 配置下定义 -> -Wunused-function 警告在 TU 末尾发出 (pragma 包不住),
 * 由 CMake 对本文件单独加 -Wno-unused-function */
#include "socket.h" /* ioLibrary (include 路径唯一命中 deps/ioLibrary/Ethernet) */

#include "w5500.h"     /* SN_MB_BASE / w5500_link_up (src/include) */
#include "mbtcp_adu.h"
#include "init.h"
#include "io_bytes.h"

#include "log.h"

#define MODBUS_TCP_PORT 502

#define MB_TCP_POLL_MS      100u /* 轮询周期 */
#define MB_TCP_FRAME_TMO_MS 500u /* 半帧无进展断开 */
#define MB_TCP_MAX_EST      2u   /* 并发主站上限 (=socket 数-1) */
#define MB_TCP_SOCK_NUM     3u   /* Sn1/2/3 固定 */
#define MB_TCP_KPALVTR      6u   /* 6 x 5s = 30s keepalive */

/* ==================== socket 状态机 ==================== */

enum mb_sock_state {
	MBSS_FREE = 0, /* 空闲 (未 open, 可作下一个 LISTEN 候选) */
	MBSS_LISTEN,   /* LISTEN, 等 W5500 自动 accept */
	MBSS_DATA,     /* ESTABLISHED, 收发数据 */
};

struct mb_sock {
	uint8_t sn;
	uint8_t state;
	uint16_t rx_len;       /* 已累积字节 */
	uint16_t pending_len;  /* 暂存应答字节数, 0 = 无 */
	TickType_t t_last;     /* 最近一次收到字节的 tick (半帧超时基准) */
	uint8_t rx[MBTCP_ADU_RX_MAX];        /* 6B 头 + length 钳制上限 256 = 262B */
	uint8_t tx_pending[MBTCP_ADU_TX_MAX]; /* send 瞬态不可发时的暂存 (263B) */
};

static struct mb_sock mb_socks[MB_TCP_SOCK_NUM];
static uint8_t mb_tx[MBTCP_ADU_TX_MAX]; /* 单任务串行使用, 无需 per-socket */

/* 任务 (prio 3, 栈 1024 字 = 4096B -- Zephyr 版 2048B 预算的 2 倍:
 * 单任务串行收发 + 262B 帧累积调用链, 留足余量) */
static StackType_t mb_tcp_stack[1024];
static StaticTask_t mb_tcp_tcb;

/* 读 socket 状态寄存器 (Sn_SR) */
static uint8_t sock_sr(uint8_t sn)
{
	uint8_t sr = SOCK_CLOSED;

	(void)getsockopt(sn, SO_STATUS, &sr);
	return sr;
}

/* 回收: close() (CR_CLOSE 确定性释放槽位, 不等 FIN/ACK 握手) + 回 FREE */
static void sock_reset(struct mb_sock *sk)
{
	(void)close(sk->sn);
	sk->state = MBSS_FREE;
	sk->rx_len = 0;
	sk->pending_len = 0;
}

/* 在空闲 socket 上开 LISTEN (socket() 阻塞模式 -- 不带 SF_IO_NONBLOCK,
 * 收发由 SO_RECVBUF/SO_SENDBUF 预探门控, 见文件头; port=502 隐式绑定;
 * 成功即置 MBSS_LISTEN, 失败留在 FREE 由下轮重试) */
static void sock_listen(struct mb_sock *sk)
{
	if (socket(sk->sn, Sn_MR_TCP, MODBUS_TCP_PORT, 0x00) ==
		    (int8_t)sk->sn &&
	    listen(sk->sn) == SOCK_OK) {
		sk->state = MBSS_LISTEN;
		sk->rx_len = 0;
		sk->pending_len = 0;
	} else {
		(void)close(sk->sn);
		sk->state = MBSS_FREE;
	}
}

/* 目标帧长: 6B 头 + length 钳制上限 256 (头 8B 到齐后才有意义,
 * 与 mbtcp_adu 的 MIN(len,256)-2 钳制及 Zephyr 的消费字节数一致) */
static uint16_t frame_need(const struct mb_sock *sk)
{
	uint16_t mbap_len = io_get_be16(&sk->rx[4]);

	if (mbap_len > 256u) {
		mbap_len = 256u;
	}
	return (uint16_t)(6u + mbap_len);
}

/* 单次发送尝试: 1 = 已发出; 0 = 瞬态不可发 (TX 空闲不足 / 前段 send
 * 未 ACK 返回 SOCK_BUSY -- 流水线主站背靠背请求下正常出现, 不是致命
 * 错, 误当致命会踢掉健康连接); -1 = 连接死 (SOCKERR_* 或状态异常,
 * 已回收) */
static int sock_send_try(struct mb_sock *sk, const uint8_t *buf, uint16_t len)
{
	uint16_t txfree = 0;

	(void)getsockopt(sk->sn, SO_SENDBUF, &txfree);
	if (txfree < len) {
		return 0; /* TX buffer 暂满 */
	}
	int32_t n = send(sk->sn, (uint8_t *)buf, len);
	if (n == (int32_t)len) {
		return 1;
	}
	if (n == SOCK_BUSY) {
		return 0; /* 前段 send 未确认: 下轮 poll 重试 */
	}
	sock_reset(sk); /* SOCKERR_* / 状态异常 */
	return -1;
}

/* 冲刷暂存应答 (每轮 DATA 态收包前先行调用): 瞬态不可发保留到下轮,
 * 返回 false = 连接已死 (已回收)。 */
static bool sock_flush(struct mb_sock *sk)
{
	if (sk->pending_len != 0) {
		int r = sock_send_try(sk, sk->tx_pending, sk->pending_len);

		if (r < 0) {
			return false;
		}
		if (r == 1) {
			sk->pending_len = 0;
		}
	}
	return true;
}

/* ESTABLISHED socket 收包: 累积到完整 ADU -> 处理 -> 单次 send 应答。
 * 断连/错误/半帧超时 -> 回收。 */
static void sock_recv(struct mb_sock *sk, uint8_t srv_unit)
{
	for (;;) {
		uint16_t want = (sk->rx_len < 8u) ? 8u : frame_need(sk);
		uint16_t room = (uint16_t)(want - sk->rx_len);
		uint16_t avail = 0;
		int32_t n;

		/* 只在确有数据时调 recv (上游 recv 对非阻塞 socket 在查
		 * RSR 前就返回 SOCK_BUSY, 见文件头; 阻塞模式 + RSR>0 则
		 * 立即返回, 不挂任务) */
		(void)getsockopt(sk->sn, SO_RECVBUF, &avail);
		if (avail == 0u) {
			if (sk->rx_len > 0 &&
			    (xTaskGetTickCount() - sk->t_last) >
				    pdMS_TO_TICKS(MB_TCP_FRAME_TMO_MS)) {
				sock_reset(sk); /* 半帧 500ms 无进展 */
			}
			return;
		}

		n = recv(sk->sn, &sk->rx[sk->rx_len],
			 (avail < room) ? avail : room);
		if (n < 0) {
			sock_reset(sk); /* ioLibrary 多数错误路径已内部 close */
			return;
		}
		if (n == 0) {
			return; /* 防御: 阻塞模式 + RSR>0 不应出现 */
		}

		sk->t_last = xTaskGetTickCount();
		sk->rx_len = (uint16_t)(sk->rx_len + (uint16_t)n);

		if (sk->rx_len < 8u || sk->rx_len < frame_need(sk)) {
			continue; /* 帧未收满, 继续收 */
		}

		/* 完整 ADU: 处理 (静默/应答判定在 mbtcp_adu 内) */
		uint16_t adu_len = 0;
		int rsp = mbtcp_adu_process(sk->rx, sk->rx_len, mb_tx,
					    sizeof(mb_tx), &adu_len, srv_unit);

		sk->rx_len = 0; /* 下一帧 (流水线主站在同轮继续收) */
		if (rsp == 1) {
			int r = sock_send_try(sk, mb_tx, adu_len);

			if (r < 0) {
				return; /* 连接已回收 */
			}
			if (r == 0) {
				/* 瞬态不可发: 暂存 (跨 poll 存活), 下轮
				 * flush 成功前不收新帧 (单槽不变量) */
				memcpy(sk->tx_pending, mb_tx, adu_len);
				sk->pending_len = adu_len;
				return;
			}
		}
	}
}

/* ==================== 任务 ==================== */

static void mb_tcp_task(void *arg)
{
	uint8_t srv_unit = (uint8_t)get_holding_reg(HOLDING_SLAVE_ID_IDX);

	(void)arg;

	for (uint8_t i = 0; i < MB_TCP_SOCK_NUM; i++) {
		mb_socks[i].sn = (uint8_t)(SN_MB_BASE + i);
		mb_socks[i].state = MBSS_FREE;
		mb_socks[i].rx_len = 0;
		mb_socks[i].pending_len = 0;
		mb_socks[i].t_last = 0;
	}

	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(MB_TCP_POLL_MS));

		/* 链路断开: 暂停 accept 与收包 (对齐 Zephyr net_link_is_up
		 * 拒新连接; 已建连接由 keepalive/TCPTO 自然回收) */
		if (!w5500_link_up()) {
			continue;
		}

		uint8_t est = 0;
		bool have_listen = false;

		for (uint8_t i = 0; i < MB_TCP_SOCK_NUM; i++) {
			struct mb_sock *sk = &mb_socks[i];

			switch (sk->state) {
			case MBSS_LISTEN: {
				uint8_t sr = sock_sr(sk->sn);

				if (sr == SOCK_ESTABLISHED) {
					/* W5500 "accept": listen socket
					 * 自身转 ESTABLISHED。开 30s
					 * keepalive 后转数据态; 补 listen
					 * 由循环尾的维持逻辑统一处理 */
					uint8_t kp = MB_TCP_KPALVTR;

					(void)setsockopt(sk->sn,
							 SO_KEEPALIVEAUTO,
							 &kp);
					sk->state = MBSS_DATA;
					sk->rx_len = 0;
					sk->t_last = xTaskGetTickCount();
					LOG_INF("mbtcp: client on sn%u",
						sk->sn);
					est++;
				} else if (sr != SOCK_LISTEN) {
					sock_reset(sk); /* 意外关闭 (超时等) */
				} else {
					have_listen = true;
				}
				break;
			}
			case MBSS_DATA: {
				uint8_t sr = sock_sr(sk->sn);

				if (sr == SOCK_CLOSED) {
					sock_reset(sk);
					LOG_INF("mbtcp: closed sn%u", sk->sn);
					break;
				}
				est++;
				/* 暂存应答先冲刷再收新帧 (主站等应答才发
				 * 下一帧, 正常无排队; 暂存未清时跳过收包,
				 * 保证每 socket 至多 1 帧暂存) */
				if (sock_flush(sk) && sk->pending_len == 0) {
					sock_recv(sk, srv_unit);
				}
				break;
			}
			default:
				break; /* MBSS_FREE: 维持逻辑统一分配 */
			}
		}

		/* LISTEN 维持: 连接数未满才有 LISTEN (满 2 时空闲 socket
		 * 保持 FREE, 第 3 个主站连接被拒); 至多 1 个 LISTEN。
		 * 刚 accept 的连接在同一轮内立即补上新的 LISTEN。 */
		if (!have_listen && est < MB_TCP_MAX_EST) {
			for (uint8_t i = 0; i < MB_TCP_SOCK_NUM; i++) {
				if (mb_socks[i].state == MBSS_FREE) {
					sock_listen(&mb_socks[i]);
					break;
				}
			}
		}
	}
}

void mb_tcp_start(void)
{
	xTaskCreateStatic(mb_tcp_task, "mbtcp", 1024, NULL, 3,
			  mb_tcp_stack, &mb_tcp_tcb);
	LOG_INF("mbtcp: task started");
}
