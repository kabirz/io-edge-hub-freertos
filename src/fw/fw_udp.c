/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP 固件升级通道 (Zephyr libs/udp_fw_upgrade 协议的 FreeRTOS 移植):
 *   0x01 FW_START [size LE32][keyhash 32B?] -> [01][status][v2_chunk LE16]
 *   0x02 FW_DATA  [data<=511]               -> [02][offset LE32]
 *   0x03 FW_END   [test u8][crc LE16]       -> [03][ok]
 * 全部命令经队列转入 fw worker 任务执行 (START 擦 slot1 可达 ~5s、
 * END 读回校验 ~0.5s, 不能阻塞 tcpip 线程), 应答经 tcpip_callback
 * 回 tcpip 线程 sendto。
 */

#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "main.h" /* NVIC_SystemReset */

#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "lwip/udp.h"

#include "bootutil/bootutil_public.h"
#include "fw_upg.h"
#include "fw_udp.h"

#include "log.h"

/* app 域 MCUboot assert 失败出口 (bootutil_public) */
void mcuboot_assert_fail(int line)
{
	LOG_ERR("mcuboot ASSERT L%d, rebooting", line);
	vTaskDelay(pdMS_TO_TICKS(200));
	log_flush(500); /* 复位前把异步日志刷出 */
	NVIC_SystemReset();
}

#define FW_Q_DEPTH 8u
#define FW_DATA_MAX 511u /* legacy 停等块 (对齐 Zephyr UDP_CHUNK_SIZE) */
#define FW_V2_CHUNK 1400u
/* DATA_V2 帧: [offset 4B][data <=1400] (不含 cmd 字节) */
#define FW_V2_MAX (4u + FW_V2_CHUNK)

enum fw_cmd {
	FW_CMD_START = 0x01,
	FW_CMD_DATA = 0x02,
	FW_CMD_END = 0x03,
	FW_CMD_DATA_V2 = 0x06,
};

struct fw_msg {
	uint8_t cmd;
	uint16_t dlen;
	ip_addr_t addr;
	uint16_t port;
	uint8_t data[FW_V2_MAX];
};

struct fw_reply {
	ip_addr_t addr;
	uint16_t port;
	uint8_t buf[8];
	uint8_t len;
};

static QueueHandle_t fw_q;
static StaticQueue_t fw_q_cb;
static uint8_t fw_q_buf[FW_Q_DEPTH * sizeof(struct fw_msg)];

/* 栈: fw_msg 局部变量 ~1.4KB (DATA_V2 帧) + fw_upg/擦除/日志调用链,
 * 512 字会溢出 (实测 START 处理后即刻复位), 取 1024 字 */
static StackType_t fw_stack[1024];
static StaticTask_t fw_tcb;

/* 应答上下文池: worker 填 -> tcpip_callback 消费 */
static struct fw_reply replies[2];
static volatile uint8_t reply_idx;

/* udp_task.c 提供: 已绑定的 8600 pcb */
extern struct udp_pcb *fw_udp_cfg_pcb(void);

/* ==================== 应答 (tcpip 线程) ==================== */

static void fw_send_reply_cb(void *arg)
{
	struct fw_reply *r = arg;
	struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, r->len, PBUF_RAM);

	if (p != NULL) {
		pbuf_take(p, r->buf, r->len);
		udp_sendto(fw_udp_cfg_pcb(), p, &r->addr, r->port);
		pbuf_free(p);
	}
}

static void fw_reply(const struct fw_msg *m, const uint8_t *buf, uint8_t len)
{
	struct fw_reply *r = &replies[reply_idx++ & 1u];

	r->addr = m->addr;
	r->port = m->port;
	memcpy(r->buf, buf, len);
	r->len = len;
	(void)tcpip_callback(fw_send_reply_cb, r);
}

/* ==================== worker 任务 ==================== */

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static void fw_task(void *arg)
{
	struct fw_msg m;

	(void)arg;
	for (;;) {
		if (xQueueReceive(fw_q, &m, portMAX_DELAY) != pdTRUE) {
			continue;
		}

		switch (m.cmd) {
		case FW_CMD_START: {
			/* m.data: [size LE32][keyhash 32B 可选] */
			uint32_t total = get_le32(m.data);
			const uint8_t *kh =
				m.dlen >= 4u + 32u ? &m.data[4] : NULL;
			uint8_t rep[6];
			int rc = fw_upg_start(total, kh);

			rep[0] = FW_CMD_START;
			rep[1] = rc == 0 ? 1u : (rc == -2 ? 2u : 0u);
			put_le16(&rep[2], FW_V2_CHUNK);
			LOG_INF("fwupg: start total=%u rc=%d",
				(unsigned)total, rc);
			fw_reply(&m, rep, 4);
			break;
		}

		case FW_CMD_DATA: {
			uint8_t rep[8];

			(void)fw_upg_write(m.data, m.dlen);
			rep[0] = FW_CMD_DATA;
			put_le32(&rep[1], fw_upg_received());
			fw_reply(&m, rep, 5);
			break;
		}

		case FW_CMD_DATA_V2: {
			/* m.data: [offset LE32][data <=1400]; 仅 offset 与已收
			 * 字节数一致才写入 (乱序/重复丢弃), 恒回当前期望 offset
			 * (上位机 go-back-N 重传) */
			uint8_t rep[5];

			if (m.dlen >= 5u &&
			    get_le32(m.data) == fw_upg_received()) {
				(void)fw_upg_write(&m.data[4], m.dlen - 4u);
			}
			rep[0] = FW_CMD_DATA_V2;
			put_le32(&rep[1], fw_upg_received());
			fw_reply(&m, rep, 5);
			break;
		}

		case FW_CMD_END: {
			/* m.data: [test u8][crc LE16] */
			uint8_t rep[2] = {FW_CMD_END, 0};
			uint16_t crc = (uint16_t)(m.data[1] |
						  (uint16_t)m.data[2] << 8);
			int permanent = m.data[0] == 0;

			if (fw_upg_finish(crc) == 0) {
				/* 永久升级: 写 slot1 trailer 请求下次启动换机 */
				if (permanent && boot_set_pending(1) == 0) {
					rep[1] = 1;
				} else if (!permanent &&
					   boot_set_pending(0) == 0) {
					rep[1] = 1;
				} else {
					rep[1] = 0;
				}
			}
			LOG_INF("fwupg: end crc=0x%04x ok=%u",
				(unsigned)crc, rep[1]);
			fw_reply(&m, rep, 2);
			break;
		}

		default:
			break;
		}
	}
}

/* ==================== 入口 (tcpip 回调线程调用) ==================== */

/* 返回 true = 命令已被固件通道消费 (应答异步), 调用方不再处理 */
bool fw_udp_cmd(const uint8_t *rx, uint16_t len, const ip_addr_t *src,
		uint16_t port)
{
	struct fw_msg m;

	if (len < 1u) {
		return false;
	}
	m.cmd = rx[0];
	m.port = port;
	m.addr = *src;
	m.dlen = 0;

	switch (m.cmd) {
	case FW_CMD_START:
		/* payload = cmd + [size 4B][keyhash 32B?] */
		if (len < 5u) {
			return false; /* 参数不足: 交由通用层静默 */
		}
		m.dlen = len - 1u > sizeof(m.data) ? sizeof(m.data)
						   : len - 1u;
		memcpy(m.data, &rx[1], m.dlen);
		break;
	case FW_CMD_DATA:
		if (len < 2u || len - 1u > FW_DATA_MAX) {
			return false;
		}
		m.dlen = len - 1u;
		memcpy(m.data, &rx[1], m.dlen);
		break;
	case FW_CMD_DATA_V2:
		/* [cmd][offset 4B][data <=1400] */
		if (len < 6u || len - 1u > FW_V2_MAX) {
			return false;
		}
		m.dlen = len - 1u;
		memcpy(m.data, &rx[1], m.dlen);
		break;
	case FW_CMD_END:
		if (len < 4u) {
			return false;
		}
		m.dlen = len - 1u;
		memcpy(m.data, &rx[1], m.dlen);
		break;
	default:
		return false; /* 0x04/0x05 走通用命令层 */
	}

	if (fw_q == NULL || xQueueSend(fw_q, &m, 0) != pdTRUE) {
		LOG_WRN("fwupg: queue full, cmd dropped");
	}
	return true;
}

void fw_udp_start(void)
{
	fw_q = xQueueCreateStatic(FW_Q_DEPTH, sizeof(struct fw_msg),
				  fw_q_buf, &fw_q_cb);
	xTaskCreateStatic(fw_task, "fw", 1024, NULL, 3, fw_stack, &fw_tcb);
}
