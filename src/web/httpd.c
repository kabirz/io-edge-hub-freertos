/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * HTTP/1.1 服务器 (端口 80): gzip SPA + REST API。
 * 回调全在 tcpip 线程; 响应按 tcp_sndbuf 分片 ACK 驱动发送 (在途
 * <= 4*MSS); 下载分块独立持 hist_lock, 不阻塞采样落盘。
 * /ws 升级握手在本文件识别, 会话移交 ws.c (单连接, 推送/命令/固件升级)。
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lwip/tcp.h"
#include "lwip/tcpip.h"

#include "init.h"
#include "io_hooks.h"
#include "io_time.h"
#include "history.h"

#include "web_json.h"
#include "web_cmds.h"
#include "ws.h"

#include "log.h"

#define HTTP_PORT        80u
#define HTTP_MAX_CONN    2u
#define HTTP_RX_BUF      640u
#define HTTP_HDR_MAX     256u  /* 下载头含 Content-Disposition ~170B */
#define HTTP_BODY_BUF    704u  /* 最大 JSON (info) ~640B */
#define HTTP_DL_CHUNK    512u
#define HTTP_RX_IDLE_MS  5000u  /* 请求半截超时 */
#define HTTP_TX_STALL_MS 10000u /* 响应无 ACK 超时 */
#define HTTP_IDLE_MS     60000u /* keep-alive 空闲超时 */

#define JSON_OK      "{\"ok\":true}"
#define JSON_BAD_REQ "{\"ok\":false,\"err\":\"bad request\"}"
#define JSON_NOT_FND "{\"ok\":false,\"err\":\"not found\"}"

/* ==================== 连接状态 ==================== */

enum resp_kind {
	RESP_NONE = 0, /* 空闲, 可收下一请求 */
	RESP_MEM,      /* hdr + 内存 body (JSON / 静态页) */
	RESP_FILE,     /* hdr + 历史文件流 */
};

struct http_conn {
    struct tcp_pcb *pcb;
    bool is_ws;               /* /ws 握手后: rx/feed 与推送移交 ws.c */
    /* rx 累积与解析 */
	uint16_t rx_len;
	bool hdr_done;
	uint16_t body_off;   /* body 在 rx 中的起点 */
	uint16_t content_len;
	bool cli_close;      /* 客户端 Connection: close */
	char rx[HTTP_RX_BUF];
	/* 响应 */
	uint8_t kind;
	bool keep_alive;
	uint32_t rsp_total;  /* hdr + body 总长 (文件早 EOF 时收缩) */
	uint32_t written;    /* 已 tcp_write */
	uint32_t acked;      /* 已 ACK */
	char hdr[HTTP_HDR_MAX];
	uint16_t hdr_len, hdr_sent;
	const uint8_t *body; /* RESP_MEM */
	uint32_t body_len, body_sent;
	char body_buf[HTTP_BODY_BUF];
	/* 文件下载 (RESP_FILE) */
	uint32_t dl_size, dl_sent;
	uint16_t dl_chunk_len, dl_chunk_off;
	uint8_t dl_chunk[HTTP_DL_CHUNK];
	/* 超时基准 */
	uint32_t t_rx, t_ack, t_idle;
};

static struct http_conn http_conns[HTTP_MAX_CONN];
static struct tcp_pcb *listen_pcb;
static struct http_conn *ws_conn; /* 被 ws 会话接管的连接 */

static uint32_t now_ms(void)
{
	return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}

static void http_process_rx(struct http_conn *c);

/* ==================== 响应构造 ==================== */

/* extra: 额外头域 (含行尾 \r\n, 可 NULL) */
static bool rsp_begin(struct http_conn *c, const char *status, const char *ctype,
		      uint32_t clen, bool keep, const char *extra)
{
	int n = snprintf(c->hdr, sizeof(c->hdr),
			 "HTTP/1.1 %s\r\n"
			 "Content-Type: %s\r\n"
			 "Content-Length: %lu\r\n"
			 "Connection: %s\r\n"
			 "%s"
			 "\r\n",
			 status, ctype, (unsigned long)clen,
			 keep ? "keep-alive" : "close", extra ? extra : "");

	if (n <= 0 || n >= (int)sizeof(c->hdr)) {
		return false;
	}
	c->hdr_len = (uint16_t)n;
	c->hdr_sent = 0;
	c->body_sent = 0;
	c->written = 0;
	c->acked = 0;
	c->keep_alive = keep && !c->cli_close;
	c->rsp_total = (uint32_t)n + clen;
	c->t_ack = now_ms();
	return true;
}

/* 内存响应 (body 指向 body_buf / 静态 gz) */
static void respond_mem(struct http_conn *c, const char *status, const char *ctype,
			const uint8_t *body, uint32_t len, bool keep,
			const char *extra){
	if (!rsp_begin(c, status, ctype, len, keep, extra)) {
		c->kind = RESP_NONE;
		return;
	}
	c->body = body;
	c->body_len = len;
	c->kind = RESP_MEM;
}

static void respond_json_ok(struct http_conn *c)
{
	respond_mem(c, "200 OK", "application/json", (const uint8_t *)JSON_OK,
		    sizeof(JSON_OK) - 1, true, NULL);
}

static void respond_json_err(struct http_conn *c, const char *status, const char *err)
{
	int n = snprintf(c->body_buf, sizeof(c->body_buf),
			 "{\"ok\":false,\"err\":\"%s\"}", err);

	if (n <= 0) {
		n = 0;
	}
	respond_mem(c, status, "application/json", (const uint8_t *)c->body_buf,
		    (uint32_t)n, true, NULL);
}

/* ==================== 发送泵 (ACK 驱动) ==================== */

static void conn_close(struct http_conn *c, const char *why)
{
	if (c->is_ws) {
		c->is_ws = false;
		if (ws_conn == c) {
			ws_conn = NULL;
		}
		ws_detach();
	}
	if (c->pcb != NULL) {
		tcp_arg(c->pcb, NULL);
		tcp_recv(c->pcb, NULL);
		tcp_sent(c->pcb, NULL);
		tcp_poll(c->pcb, NULL, 0);
		tcp_err(c->pcb, NULL);
		tcp_close(c->pcb);
		c->pcb = NULL;
		LOG_INF("httpd: closed (%s)", why);
	}
	if (c->kind == RESP_FILE) {
			history_web_close();
		}
		c->kind = RESP_NONE;
		c->rx_len = 0;
		c->hdr_done = false;
}

/* ws.c 会话关闭回调 (tcpip 线程): pcb 归 httpd, 由其收尾 */
static void ws_closed_cb(void)
{
	if (ws_conn != NULL) {
		struct http_conn *c = ws_conn;

		ws_conn = NULL;
		conn_close(c, "ws close");
	}
}

/* 文件下载: 头先行, 再分块填 4*MSS 在途窗口 */
static void pump_file(struct http_conn *c)
{
	u16_t mss = tcp_mss(c->pcb);

	while (c->hdr_sent < c->hdr_len) {
		u16_t snd = tcp_sndbuf(c->pcb);

		if (snd == 0) {
			break;
		}
		u16_t n = (uint16_t)(c->hdr_len - c->hdr_sent);

		if (n > snd) {
			n = snd;
		}
		err_t e = tcp_write(c->pcb, (const uint8_t *)c->hdr + c->hdr_sent, n,
				    TCP_WRITE_FLAG_COPY);

		if (e == ERR_OK) {
			c->hdr_sent += n;
			c->written += n;
		} else if (e == ERR_MEM) {
			break;
		} else {
			conn_close(c, "tcp_write");
			return;
		}
	}
	if (c->hdr_sent < c->hdr_len) {
		tcp_output(c->pcb);
		return; /* 头未发完, 数据缓后 */
	}

	while (c->written - c->acked < (uint32_t)(4u * mss)) {
		if (c->dl_chunk_off == c->dl_chunk_len) {
			if (c->dl_sent >= c->dl_size) {
				break;
			}
			uint32_t want = c->dl_size - c->dl_sent;

			if (want > HTTP_DL_CHUNK) {
				want = HTTP_DL_CHUNK;
			}
			int n = history_web_read(c->dl_chunk, (uint16_t)want);

			if (n < 0) {
				conn_close(c, "hist read");
				return;
			}
			if (n == 0) {
				/* EOF 早于声明长度: 收缩总长 */
				c->rsp_total = c->hdr_len + c->dl_sent;
				c->dl_size = c->dl_sent;
				break;
			}
			c->dl_chunk_len = (uint16_t)n;
			c->dl_chunk_off = 0;
			c->dl_sent += (uint32_t)n;
		}
		u16_t snd = tcp_sndbuf(c->pcb);
		u16_t n = c->dl_chunk_len - c->dl_chunk_off;

		if (snd == 0) {
			break;
		}
		if (n > snd) {
			n = snd;
		}
		err_t e = tcp_write(c->pcb, c->dl_chunk + c->dl_chunk_off, n,
				    TCP_WRITE_FLAG_COPY);

		if (e == ERR_OK) {
			c->dl_chunk_off += n;
			c->written += n;
		} else if (e == ERR_MEM) {
			break;
		} else {
			conn_close(c, "tcp_write");
			return;
		}
	}
	tcp_output(c->pcb);
}

static void conn_pump(struct http_conn *c)
{
	if (c->pcb == NULL || c->kind == RESP_NONE) {
		return;
	}

	if (c->kind == RESP_FILE) {
		pump_file(c);
		if (c->pcb == NULL) {
			return;
		}
	} else {
		while (c->hdr_sent < c->hdr_len || c->body_sent < c->body_len) {
			const uint8_t *src;
			uint32_t remain;

			if (c->hdr_sent < c->hdr_len) {
				src = (const uint8_t *)c->hdr + c->hdr_sent;
				remain = c->hdr_len - c->hdr_sent;
			} else {
				src = c->body + c->body_sent;
				remain = c->body_len - c->body_sent;
			}
			u16_t snd = tcp_sndbuf(c->pcb);

			if (snd == 0) {
				break;
			}
			u16_t n = (remain < (uint32_t)snd) ? (u16_t)remain : snd;
			err_t e = tcp_write(c->pcb, src, n, TCP_WRITE_FLAG_COPY);

			if (e == ERR_OK) {
				if (c->hdr_sent < c->hdr_len) {
					c->hdr_sent += n;
				} else {
					c->body_sent += n;
				}
				c->written += n;
			} else if (e == ERR_MEM) {
				break;
			} else {
				conn_close(c, "tcp_write");
				return;
			}
		}
		tcp_output(c->pcb);
	}

	/* 全部写出且确认完毕: 响应完成 (keep-alive 或关闭) */
	if (c->written >= c->rsp_total && c->acked >= c->rsp_total) {
		bool keep = c->keep_alive;

		if (c->kind == RESP_FILE) {
			history_web_close();
		}
		c->kind = RESP_NONE;
		c->rx_len = 0;
		c->hdr_done = false;
		if (!keep) {
			conn_close(c, "done");
			return;
		}
		c->t_idle = now_ms();
		if (c->rx_len > 0) { /* 流水线残留 (少见) */
			http_process_rx(c);
		}
	}
}

/* ==================== 路由 ==================== */

static const uint8_t index_html_gz[] = {
#include "web_index_gz.h"
};

static void dispatch(struct http_conn *c, const char *method, const char *path,
		     const char *query, const char *body, uint16_t body_len)
{
	bool is_get = strcmp(method, "GET") == 0;
	bool is_post = strcmp(method, "POST") == 0;

	if (is_get && strcmp(path, "/") == 0) {
		respond_mem(c, "200 OK", "text/html", index_html_gz,
			    WEB_INDEX_GZ_SIZE, true, "Content-Encoding: gzip\r\n");
		return;
	}
	if (is_get && strcmp(path, "/api/info") == 0) {
		int n = web_build_info_json(c->body_buf, sizeof(c->body_buf));

		respond_mem(c, "200 OK", "application/json",
			    (const uint8_t *)c->body_buf, (uint32_t)n, true, NULL);
		return;
	}
	if (is_get && strcmp(path, "/api/io") == 0) {
		int n = web_build_io_json(c->body_buf, sizeof(c->body_buf));

		respond_mem(c, "200 OK", "application/json",
			    (const uint8_t *)c->body_buf, (uint32_t)n, true, NULL);
		return;
	}
	if (is_get && strcmp(path, "/api/regs") == 0) {
		int n = web_build_regs_json(c->body_buf, sizeof(c->body_buf));

		respond_mem(c, "200 OK", "application/json",
			    (const uint8_t *)c->body_buf, (uint32_t)n, true, NULL);
		return;
	}
	if (is_get && strcmp(path, "/api/history") == 0) {
		int n = history_web_list_json(c->body_buf, sizeof(c->body_buf));

		if (n < 0) {
			/* fs 未挂载: 回空列表 */
			static const char empty[] = "{\"files\":[]}";

			respond_mem(c, "200 OK", "application/json",
				    (const uint8_t *)empty, sizeof(empty) - 1, true, NULL);
			return;
		}
		respond_mem(c, "200 OK", "application/json",
			    (const uint8_t *)c->body_buf, (uint32_t)n, true, NULL);
		return;
	}
	if (is_get && strcmp(path, "/api/history/download") == 0) {
		char name[32];
		int32_t size;

		if (query == NULL || !url_query_get(query, "name", name, sizeof(name)) ||
		    !history_web_name_valid(name) ||
		    (size = history_web_open(name)) < 0) {
			history_web_close();
			respond_json_err(c, "400 Bad Request", "invalid file name");
			return;
		}
		char extra[96];

		snprintf(extra, sizeof(extra),
			 "Content-Disposition: attachment; filename=\"%s\"\r\n",
			 name);
		if (rsp_begin(c, "200 OK", "application/octet-stream",
			      (uint32_t)size, false, extra)) {
			c->dl_size = (uint32_t)size;
			c->dl_sent = 0;
			c->dl_chunk_len = 0;
			c->dl_chunk_off = 0;
			c->kind = RESP_FILE;
		} else {
			/* 头缓冲溢出: 关掉刚打开的文件, 回 500 (防句柄泄漏) */
			history_web_close();
			respond_json_err(c, "500 Internal Server Error", "server error");
		}
		return;
	}
	if (is_post && strcmp(path, "/api/do") == 0) {
		int32_t index = 0, value = 0;

		if (json_get_i32(body, body_len, "index", &index) &&
		    json_get_i32(body, body_len, "value", &value) &&
		    web_cmd_exec_do(index, value) == 0) {
			respond_json_ok(c);
		} else {
			respond_json_err(c, "400 Bad Request", "invalid index/value");
		}
		return;
	}
	if (is_post && strcmp(path, "/api/reg") == 0) {
		int32_t addr = -1, value = 0;

		if (json_get_i32(body, body_len, "addr", &addr) &&
		    json_get_i32(body, body_len, "value", &value) &&
		    web_cmd_exec_reg(addr, value) == 0) {
			respond_json_ok(c);
		} else {
			respond_json_err(c, "400 Bad Request", "invalid addr/value");
		}
		return;
	}
	if (is_post && strcmp(path, "/api/time") == 0) {
		int32_t ts = 0;

		if (json_get_i32(body, body_len, "ts", &ts) && set_timestamp((time_t)ts)) {
			respond_json_ok(c);
		} else {
			respond_json_err(c, "400 Bad Request", "invalid timestamp");
		}
		return;
	}
	if (is_post && strcmp(path, "/api/save") == 0) {
		holding_reg_save();
		respond_json_ok(c);
		return;
	}
	if (is_post && strcmp(path, "/api/reboot") == 0) {
		set_reboot_status(true);
		LOG_INF("web reboot requested");
		respond_json_ok(c);
		return;
	}
	if (is_post && strcmp(path, "/api/cfg") == 0) {
		const char *err = NULL;

		if (web_cmd_exec_cfg(body, body_len, &err) == 0) {
			respond_json_ok(c);
		} else {
			respond_json_err(c, "400 Bad Request", err ? err : "bad request");
		}
		return;
	}
	if (is_post && strcmp(path, "/api/history/delete") == 0) {
		char name[32];

		if (json_get_str(body, body_len, "name", name, sizeof(name)) &&
		    history_web_name_valid(name) && history_web_remove(name) == 0) {
			LOG_INF("history %s deleted (web)", name);
			respond_json_ok(c);
		} else {
			respond_json_err(c, "400 Bad Request", "delete failed");
		}
		return;
	}

	(void)query;
	respond_mem(c, "404 Not Found", "application/json",
		    (const uint8_t *)JSON_NOT_FND, sizeof(JSON_NOT_FND) - 1, true, NULL);
}

/* ==================== 请求解析 ==================== */

/* rx 中找 \r\n\r\n 起始下标, 无则 -1 */
static int find_hdr_end(const struct http_conn *c)
{
	for (uint16_t i = 0; i + 3 < c->rx_len; i++) {
		if (c->rx[i] == '\r' && c->rx[i + 1] == '\n' && c->rx[i + 2] == '\r' &&
		    c->rx[i + 3] == '\n') {
			return (int)i;
		}
	}
	return -1;
}

/* 头段内大小写不敏感查找 */
static const char *hdr_find(const char *line_start, uint16_t hdr_len, const char *key)
{
	size_t klen = strlen(key);

	for (uint16_t i = 0; i + klen < hdr_len; i++) {
		if (strncasecmp(&line_start[i], key, klen) == 0) {
			return &line_start[i + klen];
		}
	}
	return NULL;
}

/* 请求行拆分, 等价 sscanf("%7s %95s"): 跳过前导空白取两词, 各截断到
 * 目标容量; 返回匹配到的词数。手工解析避免拖入 newlib scanf (~11KB) */
static int split_req_line(const char *in, char *a, size_t asz, char *b,
			  size_t bsz)
{
	size_t n = 0;

	while (*in == ' ' || *in == '\t' || *in == '\r' || *in == '\n') {
		in++;
	}
	while (*in != '\0' && *in != ' ' && *in != '\t' && *in != '\r' &&
	       *in != '\n') {
		if (n < asz - 1) {
			a[n++] = *in;
		}
		in++;
	}
	a[n] = '\0';
	if (n == 0) {
		return 0;
	}
	while (*in == ' ' || *in == '\t' || *in == '\r' || *in == '\n') {
		in++;
	}
	n = 0;
	while (*in != '\0' && *in != ' ' && *in != '\t' && *in != '\r' &&
	       *in != '\n') {
		if (n < bsz - 1) {
			b[n++] = *in;
		}
		in++;
	}
	b[n] = '\0';
	return n > 0 ? 2 : 1;
}

static void http_process_rx(struct http_conn *c)
{
	for (;;) {
		if (c->kind != RESP_NONE) {
			return; /* 响应进行中, 新请求缓后 */
		}
		if (!c->hdr_done) {
			int he = find_hdr_end(c);

			if (he < 0) {
				if (c->rx_len >= HTTP_RX_BUF - 1) {
					respond_json_err(c, "400 Bad Request", "header too large");
					c->keep_alive = false;
					conn_pump(c);
					if (c->pcb != NULL) {
						conn_close(c, "hdr too large");
					}
				}
				return;
			}
			uint16_t hdr_len = (uint16_t)he;
			char method[8] = {0};
			char target[96] = {0};

			if (split_req_line(c->rx, method, sizeof(method),
					   target, sizeof(target)) != 2) {
				respond_json_err(c, "400 Bad Request", "bad request");
				conn_pump(c);
				if (c->pcb != NULL) {
					conn_close(c, "bad request line");
				}
				return;
			}
			const char *cl = hdr_find(c->rx, hdr_len, "Content-Length:");

			c->content_len = 0;
			c->cli_close = false;
			if (cl != NULL) {
				c->content_len = (uint16_t)strtoul(cl, NULL, 10);
			}
			if (hdr_find(c->rx, hdr_len, "Connection: close") != NULL) {
				c->cli_close = true;
			}
			c->body_off = (uint16_t)(he + 4);
			c->hdr_done = true;

			if (c->content_len > 128u) {
				respond_json_err(c, "400 Bad Request", "body too large");
				conn_pump(c);
				if (c->pcb != NULL) {
					conn_close(c, "body too large");
				}
				return;
			}
			if (strcmp(method, "GET") != 0) {
				continue; /* POST: 等 body 齐 */
			}

			char *q = strchr(target, '?');
			const char *query = NULL;

			if (q != NULL) {
				*q = '\0';
				query = q + 1;
			}

			/* /ws 升级: 会话移交 ws.c (单连接, 忙则 503) */
			if (strcmp(target, "/ws") == 0 &&
			    hdr_find(c->rx, hdr_len, "Upgrade: websocket") != NULL) {
				char resp[160];
				uint16_t rl = 0;

				if (ws_active() ||
				    !ws_handshake(c->rx, hdr_len, resp, sizeof(resp),
						  &rl)) {
					respond_json_err(c, "503 Service Unavailable",
							 "ws busy");
				} else {
					memcpy(c->hdr, resp, rl);
					c->hdr_len = rl;
					c->hdr_sent = 0;
					c->body = NULL;
					c->body_len = 0;
					c->written = 0;
					c->acked = 0;
					c->rsp_total = rl;
					c->kind = RESP_MEM;
					c->keep_alive = true;
					c->t_ack = now_ms();
					c->is_ws = true;
					ws_conn = c;
					/* 残余字节 = WS 帧 (客户端不等 101 ACK) */
					memmove(c->rx, c->rx + c->body_off,
						c->rx_len - c->body_off);
					c->rx_len -= c->body_off;
					ws_attach(c->pcb, (const uint8_t *)c->rx,
						  c->rx_len, ws_closed_cb);
					c->rx_len = 0;
					c->hdr_done = false;
					conn_pump(c);
					return;
				}
			}

			dispatch(c, method, target, query, NULL, 0);
			uint16_t used = c->body_off;

			memmove(c->rx, c->rx + used, c->rx_len - used);
			c->rx_len -= used;
			c->hdr_done = false;
			conn_pump(c);
			if (c->kind != RESP_NONE) {
				return;
			}
			continue;
		}
		if ((uint16_t)(c->rx_len - c->body_off) >= c->content_len) {
			char method[8] = {0};
			char target[96] = {0};

			(void)split_req_line(c->rx, method, sizeof(method),
					     target, sizeof(target));
			char *q = strchr(target, '?');
			const char *query = NULL;

			if (q != NULL) {
				*q = '\0';
				query = q + 1;
			}
			dispatch(c, method, target, query, &c->rx[c->body_off],
				 c->content_len);
			uint16_t used = (uint16_t)(c->body_off + c->content_len);

			memmove(c->rx, c->rx + used, c->rx_len - used);
			c->rx_len -= used;
			c->hdr_done = false;
			conn_pump(c);
			if (c->kind != RESP_NONE) {
				return;
			}
			continue;
		}
		return; /* body 未齐 */
	}
}

/* ==================== 连接回调 (tcpip 线程) ==================== */

static err_t http_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
	struct http_conn *c = (struct http_conn *)arg;

	if (p == NULL) {
		conn_close(c, "peer closed");
		return ERR_OK;
	}
	if (err != ERR_OK || c->pcb == NULL) {
		pbuf_free(p);
		return err;
	}

	/* WS 会话: 字节流直喂 ws.c 帧解析器 */
	if (c->is_ws) {
		tcp_recved(pcb, p->tot_len);
		for (struct pbuf *q = p; q != NULL; q = q->next) {
			ws_feed((const uint8_t *)q->payload, q->len);
		}
		pbuf_free(p);
		return ERR_OK;
	}

	uint16_t room = (uint16_t)(HTTP_RX_BUF - 1 - c->rx_len);

	if (p->tot_len > room) { /* 超长请求: 拒绝 */
		tcp_recved(pcb, p->tot_len);
		pbuf_free(p);
		respond_json_err(c, "400 Bad Request", "request too large");
		c->keep_alive = false;
		conn_pump(c);
		if (c->pcb != NULL) {
			conn_close(c, "rx overflow");
		}
		return ERR_OK;
	}

	for (struct pbuf *q = p; q != NULL; q = q->next) {
		memcpy(&c->rx[c->rx_len], q->payload, q->len);
		c->rx_len = (uint16_t)(c->rx_len + q->len);
	}
	tcp_recved(pcb, p->tot_len);
	pbuf_free(p);
	c->rx[c->rx_len] = '\0';
	c->t_rx = now_ms();

	http_process_rx(c);
	return ERR_OK;
}

static err_t http_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len)
{
	struct http_conn *c = (struct http_conn *)arg;

	(void)pcb;
	if (c == NULL || c->pcb == NULL) {
		return ERR_OK;
	}
	c->acked += len;
	c->t_ack = now_ms();
	if (c->is_ws) {
		return ERR_OK; /* 101 已 ACK 后无状态机要推进 */
	}
	conn_pump(c);
	return ERR_OK;
}

static err_t http_poll_cb(void *arg, struct tcp_pcb *pcb)
{
	struct http_conn *c = (struct http_conn *)arg;
	uint32_t now = now_ms();

	(void)pcb;
	if (c == NULL || c->pcb == NULL) {
		return ERR_OK;
	}
	if (c->is_ws) {
		ws_poll(); /* 推送节律由 poll (~1s) 驱动; 无空闲超时 */
		return ERR_OK;
	}
	if (c->kind != RESP_NONE) {
		if (now - c->t_ack > HTTP_TX_STALL_MS) {
			conn_close(c, "tx stall");
			return ERR_OK;
		}
	} else if (c->rx_len > 0 || c->hdr_done) {
		if (now - c->t_rx > HTTP_RX_IDLE_MS) {
			conn_close(c, "rx timeout");
			return ERR_OK;
		}
	} else if (now - c->t_rx > HTTP_IDLE_MS) {
		conn_close(c, "idle");
	}
	return ERR_OK;
}

static void http_err_cb(void *arg, err_t err)
{
	struct http_conn *c = (struct http_conn *)arg;

	LOG_WRN("httpd: conn err %d", (int)err);
	if (c != NULL) {
		/* pcb 已被栈释放 */
		if (c->is_ws) {
			c->is_ws = false;
			if (ws_conn == c) {
				ws_conn = NULL;
			}
			ws_detach();
		}
		if (c->kind == RESP_FILE) {
			history_web_close();
		}
		c->pcb = NULL;
		c->kind = RESP_NONE;
		c->rx_len = 0;
		c->hdr_done = false;
	}
}

static err_t http_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
	(void)arg;

	if (err != ERR_OK || newpcb == NULL) {
		return ERR_OK;
	}

	struct http_conn *c = NULL;
	for (uint8_t i = 0; i < HTTP_MAX_CONN; i++) {
		if (http_conns[i].pcb == NULL) {
			c = &http_conns[i];
			break;
		}
	}
	if (c == NULL) {
		tcp_abort(newpcb); /* 满 2 连接: 拒绝 (浏览器会重试) */
		return ERR_ABRT;
	}

	memset(c, 0, offsetof(struct http_conn, rx));
	c->pcb = newpcb;
	c->t_rx = c->t_ack = c->t_idle = now_ms();

	tcp_arg(newpcb, c);
	tcp_recv(newpcb, http_recv_cb);
	tcp_sent(newpcb, http_sent_cb);
	tcp_poll(newpcb, http_poll_cb, 2); /* 单位 ~0.5s */
	tcp_err(newpcb, http_err_cb);
	tcp_nagle_disable(newpcb);
	LOG_INF("httpd: client %s:%u", ipaddr_ntoa(&newpcb->remote_ip),
		(unsigned)newpcb->remote_port);
	return ERR_OK;
}

/* ==================== listener (tcpip 线程) ==================== */

static void http_listen_init(void *arg)
{
	(void)arg;

	struct tcp_pcb *lp = tcp_new();
	err_t e;

	if (lp == NULL) {
		LOG_ERR("httpd: tcp_new failed");
		return;
	}
	e = tcp_bind(lp, IP_ADDR_ANY, HTTP_PORT);
	if (e != ERR_OK) {
		LOG_ERR("httpd: bind %u failed (%d)", HTTP_PORT, (int)e);
		tcp_close(lp);
		return;
	}
	listen_pcb = tcp_listen_with_backlog(lp, 2);
	tcp_accept(listen_pcb, http_accept_cb);
	LOG_INF("httpd: listening on %u", HTTP_PORT);
}

void web_httpd_start(void)
{
	ws_init(); /* ws fw 工作任务 + op 队列 */
	tcpip_callback(http_listen_init, NULL); /* RAW API 须在 tcpip 线程 */
}
