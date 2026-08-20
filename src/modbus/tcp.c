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
 *   - 非阻塞 IO (SF_IO_NONBLOCK, 100ms 轮询; 等价 ctlsocket 的
 *     CS_SET_IOMODE) : recv 无数据返回 SOCK_BUSY(0), >0 为数据, <0 为
 *     断连/错误 (ioLibrary 内部已 close, 此处再 close 幂等兜底)。
 *   - 帧累积: 收满 6+MIN(MBAP length,256) 字节 (与 Zephyr 的消费字节数
 *     一致, 缓冲按此上限 262B 静态分配) 即为完整 ADU; 半帧 500ms 无进展
 *     -> 断开回收 (防恶意主站声明大长度后挂死连接)。
 *   - 完整帧 -> mbtcp_adu_process (proto 校验/unit 改写/广播抑制/
 *     诊断计数都在其中) -> 应答 MBAP+PDU 已合并为单缓冲, 单次 send()
 *     (拆两段 send 会被部分上位机按"一段=一帧"误解析)。
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

/* LOG 占位 (Task 13 替换为真实日志) */
#define LOG_INF(...) do {} while (0)
#define LOG_WRN(...) do {} while (0)

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
	uint16_t rx_len;    /* 已累积字节 */
	TickType_t t_last;  /* 最近一次收到字节的 tick (半帧超时基准) */
	uint8_t rx[MBTCP_ADU_RX_MAX]; /* 6B 头 + length 钳制上限 256 = 262B */
};

static struct mb_sock mb_socks[MB_TCP_SOCK_NUM];
static uint8_t mb_tx[MBTCP_ADU_TX_MAX]; /* 单任务串行使用, 无需 per-socket */

/* 任务 (prio 3, 栈 1024 字 -- 对齐 Zephyr 版 2048B 预算) */
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
}

/* 在空闲 socket 上开 LISTEN (socket() 带 SF_IO_NONBLOCK, port=502 隐式
 * 绑定; 成功即置 MBSS_LISTEN, 失败留在 FREE 由下轮重试) */
static void sock_listen(struct mb_sock *sk)
{
	if (socket(sk->sn, Sn_MR_TCP, MODBUS_TCP_PORT, SF_IO_NONBLOCK) ==
		    (int8_t)sk->sn &&
	    listen(sk->sn) == SOCK_OK) {
		sk->state = MBSS_LISTEN;
		sk->rx_len = 0;
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

/* ESTABLISHED socket 收包: 累积到完整 ADU -> 处理 -> 单次 send 应答。
 * 断连/错误/半帧超时 -> 回收。 */
static void sock_recv(struct mb_sock *sk, uint8_t srv_unit)
{
	for (;;) {
		uint16_t want = (sk->rx_len < 8u) ? 8u : frame_need(sk);
		int32_t n = recv(sk->sn, &sk->rx[sk->rx_len],
				 (uint16_t)(want - sk->rx_len));

		if (n < 0) {
			sock_reset(sk); /* ioLibrary 多数错误路径已内部 close */
			return;
		}
		if (n == 0) { /* SOCK_BUSY: 无数据 */
			if (sk->rx_len > 0 &&
			    (xTaskGetTickCount() - sk->t_last) >
				    pdMS_TO_TICKS(MB_TCP_FRAME_TMO_MS)) {
				sock_reset(sk); /* 半帧 500ms 无进展 */
			}
			return;
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
			if (send(sk->sn, mb_tx, adu_len) !=
			    (int32_t)adu_len) {
				sock_reset(sk); /* 发送失败 = 连接已死 */
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
				sock_recv(sk, srv_unit);
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
