/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * FTP server (RFC 959, 单线程 select, 最多 3 客户端)
 * — Zephyr 版 src/ftp_server/ftpd.c 的 FreeRTOS/LwIP 移植:
 *   - PASV 被动 + PORT 主动数据连接 (EPSV/EPRT 扩展)
 *   - TYPE A (ASCII CR/LF 转换, 跨块 \r 合并) / TYPE I (二进制)
 *   - select 多路复用控制命令; RETR/STOR/LIST 传输时该会话独占
 *   - 120s 空闲超时; 路径规范化(.. 防护); LIST 标准 Unix ls -l
 *   - 命令: USER PASS SYST FEAT TYPE PWD CWD CDUP PORT PASV EPSV EPRT
 *            LIST NLST RETR STOR APPE DELE MKD RMD RNFR RNTO SIZE REST
 *            NOOP ALLO QUIT
 *   - 存储映射 littlefs 根 (history_fs()); 每次 lfs 操作单独持
 *     history_fs_lock (与采样落盘互斥, 长传输分块之间放锁)
 *   - Zephyr 差异: sscanf 全部换 strtoul/手工解析 (省 newlib scanf);
 *     ls -l 时间取 io_now_epoch (RTC); admin 之外的 anonymous 只读
 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

#include "lfs.h"

#include "ftp.h"
#include "history.h" /* history_fs/_lock/_unlock, history_sync */
#include "io_time.h" /* ls -l 时间 (RTC epoch) */
#include "log.h"

#define FTP_MAX_CLIENTS         3
#define FTP_SESSION_TIMEOUT_MS  (120u * 1000u)
/* 控制/数据连接 socket 超时: 防慢速/恶意客户端逐字节滴水冻结整个
 * FTP 线程 (单线程 select 多路复用, 一冻全冻) */
#define FTP_CTRL_TIMEOUT_MS     10000
#define FTP_DATA_TIMEOUT_MS     15000

struct ftp_session {
	int ctrl;
	bool authed;
	bool anon;
	int data_listen;   /* PASV 监听 */
	bool data_is_port; /* PORT 主动模式 */
	struct sockaddr_in port_addr;
	char cwd[64];
	bool type_ascii;
	uint32_t rest;
	char rename_from[128];
	bool rename_pending;
	bool pending_cr; /* ASCII 上传: 上一块末尾 \r 待与下一块 \n 合并 */
	uint32_t last_activity;
	char buf[FTP_BUF_SIZE];
};

static struct ftp_session sessions[FTP_MAX_CLIENTS];

/* 8KB: 主循环 line[512] + cmd_* fspath[512] + 日志链 newlib _svfprintf_r
 * 峰值实测 ~4.2KB (xTaskCreateStatic 深度参数误传 1024 时曾溢出到
 * 栈底之下踩坏 sessions), 8KB 留近一倍余量 */
static StackType_t ftp_stack[2048] __attribute__((aligned(8)));
static StaticTask_t ftp_tcb;

static uint32_t now_ms(void)
{
	return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}

static void ftp_send(int s, const char *msg)
{
	char buf[FTP_BUF_SIZE];
	int len = snprintf(buf, sizeof(buf), "%s\r\n", msg);

	(void)lwip_send(s, buf, len, 0);
}

static int ftp_sendf(int s, const char *fmt, ...)
{
	char buf[FTP_BUF_SIZE];
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (len < 0) {
		return 0;
	}
	if (len + 2 >= (int)sizeof(buf)) {
		len = sizeof(buf) - 3;
	}
	buf[len++] = '\r';
	buf[len++] = '\n';
	return lwip_send(s, buf, len, 0);
}

/* 数据连接整段发送 (SO_SNDTIMEO 内重试部分写) */
static int send_all(int s, const char *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		int n = lwip_send(s, buf + off, len - off, 0);

		if (n <= 0) {
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

static void set_sock_timeout(int s, int timeout_ms)
{
	struct timeval tv = {
		.tv_sec = timeout_ms / 1000,
		.tv_usec = (timeout_ms % 1000) * 1000,
	};

	(void)lwip_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	(void)lwip_setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* 规范化客户端路径: 绝对/相对 + . / .., 栈式防护 .. 越界 */
static void norm_path(char *out, size_t outlen, const char *cwd,
		      const char *input)
{
	char tmp[160];
	char parts[16][48];
	const char *base = (input[0] == '/') ? "" : cwd;
	const char *p;
	int n = 0;
	size_t pos = 0;

	snprintf(tmp, sizeof(tmp), "%s/%s", base, input);

	p = tmp;
	while (*p != '\0') {
		const char *start;
		size_t len;

		while (*p == '/') {
			p++;
		}
		if (*p == '\0') {
			break;
		}
		start = p;
		while (*p != '\0' && *p != '/') {
			p++;
		}
		len = (size_t)(p - start);

		if (len == 1 && start[0] == '.') {
			continue;
		}
		if (len == 2 && start[0] == '.' && start[1] == '.') {
			if (n > 0) {
				n--;
			}
			continue;
		}
		if (n < 16) {
			if (len > 47) {
				len = 47;
			}
			memcpy(parts[n], start, len);
			parts[n][len] = '\0';
			n++;
		}
	}

	if (pos < outlen - 1) {
		out[pos++] = '/';
	}
	for (int i = 0; i < n && pos < outlen - 1; i++) {
		size_t pl = strlen(parts[i]);

		if (pos + pl + 1 >= outlen) {
			break;
		}
		memcpy(out + pos, parts[i], pl);
		pos += pl;
		out[pos++] = '/';
	}
	if (pos > 1 && out[pos - 1] == '/') {
		pos--;
	}
	out[pos] = '\0';
}

/* littlefs 路径 = 规范化路径 (FTP_ROOT 为空, fs 以 / 为根) */
static void fs_path(char *out, size_t outlen, const char *cwd,
		    const char *client_path)
{
	norm_path(out, outlen, cwd, client_path);
}

static void get_local_ip(uint8_t *ip)
{
	memset(ip, 0, 4);
	if (netif_default != NULL) {
		memcpy(ip, &netif_default->ip_addr.addr, 4);
	}
}

/* RETR ASCII: \n -> \r\n (out 容量需 >= 2*len) */
static size_t ascii_crlf(char *out, const char *in, size_t len)
{
	size_t o = 0;

	for (size_t i = 0; i < len; i++) {
		if (in[i] == '\n') {
			out[o++] = '\r';
		}
		out[o++] = in[i];
	}
	return o;
}

/* STOR ASCII: \r\n -> \n (原地缩短); pending_cr 跨块合并末尾 \r */
static size_t ascii_strip_cr(char *buf, size_t len, bool *pending_cr)
{
	size_t o = 0;
	size_t start = 0;

	if (*pending_cr && len > 0 && buf[0] == '\n') {
		start = 1;
	}
	*pending_cr = false;

	for (size_t i = start; i < len; i++) {
		if (buf[i] == '\r' && i + 1 < len && buf[i + 1] == '\n') {
			continue;
		}
		buf[o++] = buf[i];
	}
	if (len > start && buf[len - 1] == '\r') {
		if (o > 0) {
			o--;
		}
		*pending_cr = true;
	}
	return o;
}

static const char *const ftp_months[] = {"Jan", "Feb", "Mar", "Apr",
					 "May", "Jun", "Jul", "Aug",
					 "Sep", "Oct", "Nov", "Dec"};

/* 历史文件名 data_MMDD_HHMM[SS].raw -> 真实创建时间 (lfs 无 mtime);
 * 对齐 Zephyr sscanf("%2u%2u_%2u%2u"): 尾部多余数字 (秒) 忽略 */
static bool parse_hist_time(const char *name, int *mon, int *day, int *hour,
			    int *min)
{
	unsigned v[4]; /* MM DD HH MM 各 2 位十进制 */
	const char *s = name + 5;

	if (strncmp(name, "data_", 5) != 0) {
		return false;
	}
	for (int i = 0; i < 4; i++) {
		char *end;
		unsigned long x;

		if (i == 2) { /* MMDD 与 HHMM 之间跳过分隔符 */
			while (*s == '_') {
				s++;
			}
		}
		if (!isdigit((unsigned char)s[0]) ||
		    !isdigit((unsigned char)s[1])) {
			return false;
		}
		x = strtoul(s, &end, 10);
		(void)end;
		v[i] = (unsigned)x;
		s += 2;
	}
	if (v[0] < 1 || v[0] > 12 || v[1] < 1 || v[1] > 31 || v[2] > 23 ||
	    v[3] > 59) {
		return false;
	}
	*mon = (int)v[0];
	*day = (int)v[1];
	*hour = (int)v[2];
	*min = (int)v[3];
	return true;
}

/* ls -l 时间字段 "Mon DD HH:MM": 历史文件取真实时间, 其他用当前 RTC */
static void format_ls_time(char *out, size_t len, const char *name)
{
	int mon, day, hour, minute;

	if (parse_hist_time(name, &mon, &day, &hour, &minute)) {
		snprintf(out, len, "%s %2d %02d:%02d", ftp_months[mon - 1],
			 day, hour, minute);
		return;
	}
	{
		time_t t = (time_t)io_now_epoch();
		struct tm lt;

		if (gmtime_r(&t, &lt) == NULL) {
			snprintf(out, len, "%s %2d %02d:%02d", ftp_months[0],
				 1, 0, 0);
			return;
		}
		snprintf(out, len, "%s %2d %02d:%02d", ftp_months[lt.tm_mon],
			 lt.tm_mday, lt.tm_hour, lt.tm_min);
	}
}

static void cmd_pasv(struct ftp_session *s)
{
	struct sockaddr_in addr;
	socklen_t alen;
	uint8_t ip[4];
	uint16_t port;

	if (s->data_listen >= 0) {
		lwip_close(s->data_listen);
	}
	s->data_is_port = false;
	s->data_listen = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s->data_listen < 0) {
		ftp_send(s->ctrl, "425 Cannot open passive connection");
		return;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = PP_HTONL(INADDR_ANY);
	addr.sin_port = 0;
	if (lwip_bind(s->data_listen, (struct sockaddr *)&addr,
		      sizeof(addr)) < 0 ||
	    lwip_listen(s->data_listen, 1) < 0) {
		lwip_close(s->data_listen);
		s->data_listen = -1;
		ftp_send(s->ctrl, "425 Passive bind failed");
		return;
	}

	alen = sizeof(addr);
	(void)lwip_getsockname(s->data_listen, (struct sockaddr *)&addr, &alen);
	port = ntohs(addr.sin_port);
	get_local_ip(ip);
	ftp_sendf(s->ctrl,
		  "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)", ip[0],
		  ip[1], ip[2], ip[3], (port >> 8) & 0xFF, port & 0xFF);
}

/* EPSV: 扩展被动模式 (RFC 2428), 回 229 (|||port|) */
static void cmd_epsv(struct ftp_session *s)
{
	struct sockaddr_in addr;
	socklen_t alen;

	if (s->data_listen >= 0) {
		lwip_close(s->data_listen);
	}
	s->data_is_port = false;
	s->data_listen = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s->data_listen < 0) {
		ftp_send(s->ctrl, "425 Cannot open passive connection");
		return;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = PP_HTONL(INADDR_ANY);
	addr.sin_port = 0;
	if (lwip_bind(s->data_listen, (struct sockaddr *)&addr,
		      sizeof(addr)) < 0 ||
	    lwip_listen(s->data_listen, 1) < 0) {
		lwip_close(s->data_listen);
		s->data_listen = -1;
		ftp_send(s->ctrl, "425 Passive bind failed");
		return;
	}
	alen = sizeof(addr);
	(void)lwip_getsockname(s->data_listen, (struct sockaddr *)&addr, &alen);
	ftp_sendf(s->ctrl, "229 Entering Extended Passive Mode (|||%u|)",
		  ntohs(addr.sin_port));
}

/* PORT h1,h2,h3,h4,p1,p2 -> 主动模式目标地址 */
static void cmd_port(struct ftp_session *s, const char *arg)
{
	unsigned long v[6];
	uint8_t ip[4];

	for (int i = 0; i < 6; i++) {
		char *end;

		v[i] = strtoul(arg, &end, 10);
		if (end == arg || v[i] > 255 ||
		    (i < 5 && *end != ',')) {
			ftp_send(s->ctrl, "501 Syntax error in parameters");
			return;
		}
		arg = end + 1;
	}
	if (s->data_listen >= 0) {
		lwip_close(s->data_listen);
		s->data_listen = -1;
	}
	ip[0] = (uint8_t)v[0];
	ip[1] = (uint8_t)v[1];
	ip[2] = (uint8_t)v[2];
	ip[3] = (uint8_t)v[3];
	memset(&s->port_addr, 0, sizeof(s->port_addr));
	s->port_addr.sin_family = AF_INET;
	memcpy(&s->port_addr.sin_addr.s_addr, ip, 4);
	s->port_addr.sin_port = htons((uint16_t)((v[4] << 8) | v[5]));
	s->data_is_port = true;
	ftp_send(s->ctrl, "200 PORT command successful");
}

/* EPRT |1|ipv4|port| -> 扩展主动模式 (RFC 2428, 仅支持 IPv4) */
static void cmd_eprt(struct ftp_session *s, const char *arg)
{
	ip4_addr_t addr4;
	const char *p1;
	const char *p2;
	unsigned long proto;
	unsigned long port;
	char ipstr[32];
	size_t n;

	/* |1|a.b.c.d|port| */
	if (arg[0] != '|') {
		goto bad;
	}
	p1 = strchr(arg + 1, '|');
	if (p1 == NULL) {
		goto bad;
	}
	p2 = strchr(p1 + 1, '|');
	if (p2 == NULL) {
		goto bad;
	}
	proto = strtoul(arg + 1, NULL, 10);
	port = strtoul(p2 + 1, NULL, 10);
	n = (size_t)(p2 - p1 - 1);
	if (proto != 1 || port == 0 || port > 0xFFFF || n == 0 ||
	    n >= sizeof(ipstr)) {
		goto bad;
	}
	memcpy(ipstr, p1 + 1, n);
	ipstr[n] = '\0';
	if (!ip4addr_aton(ipstr, &addr4)) {
		ftp_send(s->ctrl, "501 Bad address");
		return;
	}
	if (s->data_listen >= 0) {
		lwip_close(s->data_listen);
		s->data_listen = -1;
	}
	memset(&s->port_addr, 0, sizeof(s->port_addr));
	s->port_addr.sin_family = AF_INET;
	memcpy(&s->port_addr.sin_addr.s_addr, &addr4.addr, 4);
	s->port_addr.sin_port = htons((uint16_t)port);
	s->data_is_port = true;
	ftp_send(s->ctrl, "200 EPRT command successful");
	return;
bad:
	ftp_send(s->ctrl, "522 Network protocol not supported, use (1)");
}

/* 建立数据连接: PASV (select+accept) 或 PORT (connect) */
static int open_data(struct ftp_session *s)
{
	if (s->data_is_port) {
		int d;

		s->data_is_port = false;
		d = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (d < 0) {
			return -1;
		}
		if (lwip_connect(d, (struct sockaddr *)&s->port_addr,
				 sizeof(s->port_addr)) < 0) {
			lwip_close(d);
			return -1;
		}
		set_sock_timeout(d, FTP_DATA_TIMEOUT_MS);
		return d;
	}

	if (s->data_listen < 0) {
		return -1;
	}
	{
		/* select 等数据连接 (SO_RCVTIMEO 对 accept 不生效):
		 * 客户端 PASV 后不连数据端口时防永久阻塞单线程 FTP */
		fd_set rfds;
		struct timeval tv = {
			.tv_sec = FTP_DATA_TIMEOUT_MS / 1000,
			.tv_usec = (FTP_DATA_TIMEOUT_MS % 1000) * 1000,
		};
		int d;

		FD_ZERO(&rfds);
		FD_SET(s->data_listen, &rfds);
		if (lwip_select(s->data_listen + 1, &rfds, NULL, NULL, &tv) <=
			    0 ||
		    !FD_ISSET(s->data_listen, &rfds)) {
			lwip_close(s->data_listen);
			s->data_listen = -1;
			return -1;
		}
		d = lwip_accept(s->data_listen, NULL, NULL);
		lwip_close(s->data_listen);
		s->data_listen = -1;
		if (d >= 0) {
			set_sock_timeout(d, FTP_DATA_TIMEOUT_MS);
		}
		return d;
	}
}

static void drop_data_listen(struct ftp_session *s)
{
	if (s->data_listen >= 0) {
		lwip_close(s->data_listen);
		s->data_listen = -1;
	}
	s->data_is_port = false;
}

/* LIST (long) / NLST (short); littlefs 目录遍历整段持锁 */
static void cmd_list(struct ftp_session *s, const char *path, bool long_fmt)
{
	char fspath[FTP_BUF_SIZE];
	struct lfs_info st;

	fs_path(fspath, sizeof(fspath), s->cwd, path);

	history_fs_lock();
	int have = (history_fs() != NULL) &&
		   (lfs_stat(history_fs(), fspath, &st) == 0);
	history_fs_unlock();

	if (!have) {
		ftp_send(s->ctrl, "550 No such file or directory");
		drop_data_listen(s);
		return;
	}

	int data = open_data(s);

	if (data < 0) {
		ftp_send(s->ctrl, "425 No data connection");
		return;
	}

	if (st.type == LFS_TYPE_REG) {
		const char *base = strrchr(path, '/');
		char tbuf[24];
		int len;

		base = base ? base + 1 : path;
		ftp_send(s->ctrl, "150 Here comes the file listing");
		format_ls_time(tbuf, sizeof(tbuf), base);
		len = long_fmt
			  ? snprintf(s->buf, sizeof(s->buf),
				     "-rw-r--r-- 1 owner group %10u %s %s\r\n",
				     (unsigned)st.size, tbuf, base)
			  : snprintf(s->buf, sizeof(s->buf), "%s\r\n", base);
		if (len > 0) {
			(void)send_all(data, s->buf, (size_t)len);
		}
		lwip_close(data);
		ftp_send(s->ctrl, "226 Transfer complete");
		return;
	}

	ftp_send(s->ctrl, "150 Here comes the directory listing");
	history_fs_lock();
	if (history_fs() != NULL) {
		lfs_dir_t dir;

		if (lfs_dir_open(history_fs(), &dir, fspath) == 0) {
			struct lfs_info info;

			while (lfs_dir_read(history_fs(), &dir, &info) > 0) {
				char tbuf[24];
				int len;

				if (strcmp(info.name, ".") == 0 ||
				    strcmp(info.name, "..") == 0) {
					continue;
				}
				format_ls_time(tbuf, sizeof(tbuf), info.name);
				len = long_fmt
					  ? snprintf(s->buf, sizeof(s->buf),
						     "%s 1 owner group %10u %s %s\r\n",
						     info.type == LFS_TYPE_DIR
							 ? "drwxr-xr-x"
							 : "-rw-r--r--",
						     (unsigned)info.size, tbuf,
						     info.name)
					  : snprintf(s->buf, sizeof(s->buf),
						     "%s\r\n", info.name);
				if (len > 0) {
					/* 整段持锁下发送: 数据 socket
					 * SNDTIMEO 兜底, 慢客户端最长
					 * 阻塞采样落盘 15s */
					if (send_all(data, s->buf,
						     (size_t)len) != 0) {
						break;
					}
				}
			}
			lfs_dir_close(history_fs(), &dir);
		}
	}
	history_fs_unlock();
	lwip_close(data);
	ftp_send(s->ctrl, "226 Directory send OK");
}

static void cmd_retr(struct ftp_session *s, const char *path)
{
	char fspath[FTP_BUF_SIZE];
	char in[FTP_BUF_SIZE / 2];
	lfs_file_t fp;
	bool opened = false;
	lfs_soff_t size = 0;

	if (!s->authed) {
		ftp_send(s->ctrl, "530 Not logged in");
		drop_data_listen(s);
		return;
	}

	fs_path(fspath, sizeof(fspath), s->cwd, path);

	history_fs_lock();
	if (history_fs() != NULL &&
	    lfs_file_open(history_fs(), &fp, fspath, LFS_O_RDONLY) == 0) {
		opened = true;
		size = lfs_file_size(history_fs(), &fp);
		if (size < 0) {
			(void)lfs_file_close(history_fs(), &fp);
			opened = false;
		}
	}
	if (opened && s->rest > 0 &&
	    lfs_file_seek(history_fs(), &fp, (lfs_soff_t)s->rest,
			  LFS_SEEK_SET) < 0) {
		(void)lfs_file_close(history_fs(), &fp);
		opened = false;
	}
	history_fs_unlock();

	if (!opened) {
		ftp_send(s->ctrl, "550 Failed to open file");
		s->rest = 0;
		drop_data_listen(s);
		return;
	}

	ftp_send(s->ctrl, "150 Opening data connection");

	int data = open_data(s);

	if (data < 0) {
		history_fs_lock();
		(void)lfs_file_close(history_fs(), &fp);
		history_fs_unlock();
		ftp_send(s->ctrl, "425 No data connection");
		s->rest = 0;
		return;
	}

	for (;;) {
		lfs_ssize_t n;

		history_fs_lock();
		n = history_fs() ? lfs_file_read(history_fs(), &fp, in,
						 sizeof(in))
				 : -1;
		history_fs_unlock();
		if (n <= 0) {
			break;
		}
		if (s->type_ascii) {
			size_t send_len = ascii_crlf(s->buf, in, (size_t)n);

			if (send_all(data, s->buf, send_len) != 0) {
				break;
			}
		} else if (send_all(data, in, (size_t)n) != 0) {
			break;
		}
	}
	history_fs_lock();
	(void)lfs_file_close(history_fs(), &fp);
	history_fs_unlock();
	lwip_close(data);
	s->rest = 0;
	ftp_send(s->ctrl, "226 Transfer complete");
}

static void cmd_stor(struct ftp_session *s, const char *path, bool is_appe)
{
	char fspath[FTP_BUF_SIZE];
	lfs_file_t fp;
	bool opened = false;
	uint32_t rest = s->rest;
	int flags;

	if (!s->authed || s->anon) {
		ftp_send(s->ctrl, "530 Permission denied");
		drop_data_listen(s);
		return;
	}

	fs_path(fspath, sizeof(fspath), s->cwd, path);

	/* STOR (无 REST): 截断旧文件全新写; REST 续传: 定位偏移;
	 * APPE: 追加到文件末尾 */
	flags = LFS_O_WRONLY | LFS_O_CREAT;
	if (is_appe) {
		flags |= LFS_O_APPEND;
	} else if (rest == 0) {
		flags |= LFS_O_TRUNC;
	}

	history_fs_lock();
	if (history_fs() != NULL &&
	    lfs_file_open(history_fs(), &fp, fspath, flags) == 0) {
		opened = true;
		if (is_appe) {
			(void)lfs_file_seek(history_fs(), &fp, 0,
					    LFS_SEEK_END);
		} else if (rest > 0) {
			(void)lfs_file_seek(history_fs(), &fp,
					    (lfs_soff_t)rest, LFS_SEEK_SET);
		}
	}
	history_fs_unlock();

	if (!opened) {
		ftp_send(s->ctrl, "550 Failed to open file");
		drop_data_listen(s);
		return;
	}

	ftp_send(s->ctrl, "150 Ok to send data");

	int data = open_data(s);

	if (data < 0) {
		history_fs_lock();
		(void)lfs_file_close(history_fs(), &fp);
		history_fs_unlock();
		ftp_send(s->ctrl, "425 No data connection");
		s->rest = 0;
		return;
	}

	s->pending_cr = false;
	for (;;) {
		int n = lwip_recv(data, s->buf, sizeof(s->buf), 0);

		if (n <= 0) {
			break;
		}
		size_t wlen = s->type_ascii
				      ? ascii_strip_cr(s->buf, (size_t)n,
						       &s->pending_cr)
				      : (size_t)n;

		if (wlen > 0) {
			history_fs_lock();
			lfs_ssize_t wr =
				history_fs()
					? lfs_file_write(history_fs(), &fp,
							 s->buf, wlen)
					: -1;

			history_fs_unlock();
			if (wr != (lfs_ssize_t)wlen) {
				LOG_WRN("ftp: STOR write failed rc=%d len=%u",
					(int)wr, (unsigned)wlen);
				break;
			}
		}
	}
	history_fs_lock();
	(void)lfs_file_close(history_fs(), &fp);
	history_fs_unlock();
	lwip_close(data);
	s->rest = 0;
	ftp_send(s->ctrl, "226 Transfer complete");
}

/* 处理一条命令 (数据命令在此阻塞传输, 独占) */
static void handle_command(struct ftp_session *s, char *line)
{
	char *sp = strchr(line, ' ');
	char cmd[8] = {0};
	char *arg = sp ? sp + 1 : (char *)"";

	if (sp) {
		size_t clen = (size_t)(sp - line);

		if (clen >= sizeof(cmd)) {
			clen = sizeof(cmd) - 1;
		}
		memcpy(cmd, line, clen);
		cmd[clen] = '\0';
	} else {
		strncpy(cmd, line, sizeof(cmd) - 1);
	}
	for (char *p = cmd; *p != '\0'; p++) {
		if (*p >= 'a' && *p <= 'z') {
			*p -= 'a' - 'A';
		}
	}

	s->last_activity = now_ms();

	if (strcmp(cmd, "USER") == 0) {
		s->anon = strcmp(arg, "anonymous") == 0 ||
			  strcmp(arg, "ftp") == 0;
		s->rename_pending = false;
		ftp_send(s->ctrl, "331 Please specify the password");
	} else if (strcmp(cmd, "PASS") == 0) {
		if (s->anon || strcmp(arg, FTP_PASS) == 0) {
			s->authed = true;
			ftp_send(s->ctrl, "230 Login successful");
		} else {
			s->authed = false;
			ftp_send(s->ctrl, "530 Login incorrect");
		}
	} else if (strcmp(cmd, "SYST") == 0) {
		ftp_send(s->ctrl, "215 UNIX Type: L8");
	} else if (strcmp(cmd, "FEAT") == 0) {
		ftp_send(s->ctrl,
			 "211-Features:\r\n SIZE\r\n PASV\r\n EPSV\r\n PORT"
			 "\r\n EPRT\r\n REST STREAM\r\n TYPE A;I\r\n NLST"
			 "\r\n MKD\r\n RMD\r\n211 END");
	} else if (strcmp(cmd, "TYPE") == 0) {
		s->type_ascii = (arg[0] == 'A' || arg[0] == 'a');
		ftp_send(s->ctrl, "200 Type set");
	} else if (strcmp(cmd, "PWD") == 0) {
		ftp_sendf(s->ctrl, "257 \"%s\" is the current directory",
			  s->cwd);
	} else if (strcmp(cmd, "CWD") == 0) {
		norm_path(s->cwd, sizeof(s->cwd), s->cwd, arg);
		ftp_send(s->ctrl, "250 Directory successfully changed");
	} else if (strcmp(cmd, "CDUP") == 0) {
		norm_path(s->cwd, sizeof(s->cwd), s->cwd, "..");
		ftp_send(s->ctrl, "250 Directory successfully changed");
	} else if (strcmp(cmd, "PASV") == 0) {
		cmd_pasv(s);
	} else if (strcmp(cmd, "EPSV") == 0) {
		cmd_epsv(s);
	} else if (strcmp(cmd, "PORT") == 0) {
		cmd_port(s, arg);
	} else if (strcmp(cmd, "EPRT") == 0) {
		cmd_eprt(s, arg);
	} else if (strcmp(cmd, "LIST") == 0 || strcmp(cmd, "NLST") == 0) {
		history_sync();
		cmd_list(s, arg, strcmp(cmd, "LIST") == 0);
	} else if (strcmp(cmd, "RETR") == 0) {
		history_sync();
		cmd_retr(s, arg);
	} else if (strcmp(cmd, "STOR") == 0 || strcmp(cmd, "APPE") == 0) {
		cmd_stor(s, arg, strcmp(cmd, "APPE") == 0);
	} else if (strcmp(cmd, "DELE") == 0) {
		char fspath[FTP_BUF_SIZE];
		int rc = -1;

		fs_path(fspath, sizeof(fspath), s->cwd, arg);
		if (s->authed && !s->anon) {
			history_fs_lock();
			rc = history_fs() ? lfs_remove(history_fs(), fspath)
					  : -1;
			history_fs_unlock();
		}
		ftp_send(s->ctrl, rc == 0 ? "250 Delete OK"
					  : "550 Delete failed");
	} else if (strcmp(cmd, "MKD") == 0 || strcmp(cmd, "XMKD") == 0) {
		char fspath[FTP_BUF_SIZE];
		int rc = -1;

		fs_path(fspath, sizeof(fspath), s->cwd, arg);
		if (s->authed && !s->anon) {
			history_fs_lock();
			rc = history_fs() ? lfs_mkdir(history_fs(), fspath)
					  : -1;
			history_fs_unlock();
		}
		if (rc == 0) {
			ftp_sendf(s->ctrl, "257 \"%s\" created", arg);
		} else {
			ftp_send(s->ctrl, "550 Cannot create directory");
		}
	} else if (strcmp(cmd, "RMD") == 0 || strcmp(cmd, "XRMD") == 0) {
		char fspath[FTP_BUF_SIZE];
		int rc = -1;

		fs_path(fspath, sizeof(fspath), s->cwd, arg);
		if (s->authed && !s->anon) {
			history_fs_lock();
			rc = history_fs() ? lfs_remove(history_fs(), fspath)
					  : -1;
			history_fs_unlock();
		}
		ftp_send(s->ctrl,
			 rc == 0 ? "250 Remove OK" : "550 Cannot remove");
	} else if (strcmp(cmd, "SIZE") == 0) {
		char fspath[FTP_BUF_SIZE];
		struct lfs_info st;
		bool ok = false;

		fs_path(fspath, sizeof(fspath), s->cwd, arg);
		history_fs_lock();
		ok = history_fs() &&
		     lfs_stat(history_fs(), fspath, &st) == 0 &&
		     st.type == LFS_TYPE_REG;
		history_fs_unlock();
		if (ok) {
			ftp_sendf(s->ctrl, "213 %u", (unsigned)st.size);
		} else {
			ftp_send(s->ctrl, "550 Not found");
		}
	} else if (strcmp(cmd, "REST") == 0) {
		s->rest = (uint32_t)strtoul(arg, NULL, 10);
		ftp_sendf(s->ctrl, "350 Restart position accepted (%lu)",
			  (unsigned long)s->rest);
	} else if (strcmp(cmd, "RNFR") == 0) {
		char fspath[FTP_BUF_SIZE];
		struct lfs_info st;
		bool ok = false;

		fs_path(fspath, sizeof(fspath), s->cwd, arg);
		if (s->authed && !s->anon) {
			history_fs_lock();
			ok = history_fs() &&
			     lfs_stat(history_fs(), fspath, &st) == 0;
			history_fs_unlock();
		}
		if (!ok) {
			ftp_send(s->ctrl, "550 No such file");
		} else {
			strncpy(s->rename_from, fspath,
				sizeof(s->rename_from) - 1);
			s->rename_pending = true;
			ftp_send(s->ctrl, "350 Ready for RNTO");
		}
	} else if (strcmp(cmd, "RNTO") == 0) {
		char fspath[FTP_BUF_SIZE];
		int rc = -1;

		fs_path(fspath, sizeof(fspath), s->cwd, arg);
		if (s->rename_pending && s->authed && !s->anon) {
			history_fs_lock();
			rc = history_fs()
				     ? lfs_rename(history_fs(),
						  s->rename_from, fspath)
				     : -1;
			history_fs_unlock();
		}
		s->rename_pending = false;
		ftp_send(s->ctrl, rc == 0 ? "250 Rename successful"
					  : "550 Rename failed");
	} else if (strcmp(cmd, "QUIT") == 0) {
		ftp_send(s->ctrl, "221 Goodbye");
		lwip_close(s->ctrl);
		s->ctrl = -1;
	} else if (strcmp(cmd, "NOOP") == 0 || strcmp(cmd, "ALLO") == 0) {
		ftp_send(s->ctrl, "200 OK");
	} else {
		ftp_send(s->ctrl, "502 Command not implemented");
	}
}

/* 读取一行命令 (select 已确认可读)。
 * 返回: >0 行长度, 0 对端关闭, <0 超时 (errno=EWOULDBLOCK)。 */
static int recv_line(int s, char *buf, int maxlen)
{
	int total = 0;

	while (total < maxlen - 1) {
		int n = lwip_recv(s, buf + total, 1, 0);

		if (n <= 0) {
			return (n < 0 && errno == EWOULDBLOCK) ? -1 : 0;
		}
		if (buf[total] == '\n') {
			break;
		}
		total++;
	}
	buf[total] = '\0';
	while (total > 0 &&
	       (buf[total - 1] == '\r' || buf[total - 1] == '\n')) {
		buf[--total] = '\0';
	}
	return total;
}

static void close_session(struct ftp_session *s)
{
	if (s->ctrl >= 0) {
		lwip_close(s->ctrl);
	}
	if (s->data_listen >= 0) {
		lwip_close(s->data_listen);
	}
	memset(s, 0, sizeof(*s));
	s->ctrl = -1;
	s->data_listen = -1;
	strcpy(s->cwd, "/");
}

static void ftp_task(void *arg)
{
	int serv;
	struct sockaddr_in addr;

	(void)arg;

	for (int i = 0; i < FTP_MAX_CLIENTS; i++) {
		sessions[i].ctrl = -1;
		sessions[i].data_listen = -1;
		strcpy(sessions[i].cwd, "/");
	}

	serv = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (serv < 0) {
		LOG_ERR("FTP socket failed");
		return;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = PP_HTONL(INADDR_ANY);
	addr.sin_port = htons(FTP_CTRL_PORT);
	if (lwip_bind(serv, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    lwip_listen(serv, FTP_MAX_CLIENTS) < 0) {
		LOG_ERR("FTP bind/listen failed");
		return;
	}

	LOG_INF("FTP server on port %d (root %s%s, max %d clients, "
		"single-thread)",
		FTP_CTRL_PORT, FTP_ROOT, "", FTP_MAX_CLIENTS);

	for (;;) {
		fd_set rfds;
		int maxfd = serv;
		int n;

		FD_ZERO(&rfds);
		FD_SET(serv, &rfds);
		for (int i = 0; i < FTP_MAX_CLIENTS; i++) {
			if (sessions[i].ctrl >= 0) {
				FD_SET(sessions[i].ctrl, &rfds);
				if (sessions[i].ctrl > maxfd) {
					maxfd = sessions[i].ctrl;
				}
			}
		}

		{
			struct timeval tv = {.tv_sec = 1, .tv_usec = 0};

			n = lwip_select(maxfd + 1, &rfds, NULL, NULL, &tv);
		}

		if (n > 0) {
			if (FD_ISSET(serv, &rfds)) {
				int c = lwip_accept(serv, NULL, NULL);

				if (c >= 0) {
					int slot = -1;

					for (int i = 0; i < FTP_MAX_CLIENTS;
					     i++) {
						if (sessions[i].ctrl < 0) {
							slot = i;
							break;
						}
					}
					if (slot < 0) {
						static const char busy[] =
							"421 Too many users"
							"\r\n";

						(void)lwip_send(c, busy,
								sizeof(busy) -
									1, 0);
						lwip_close(c);
					} else {
						memset(&sessions[slot], 0,
						       sizeof(sessions[slot]));
						sessions[slot].ctrl = c;
						sessions[slot].data_listen =
							-1;
						sessions[slot].last_activity =
							now_ms();
						strcpy(sessions[slot].cwd,
						       "/");
						set_sock_timeout(
							c,
							FTP_CTRL_TIMEOUT_MS);
						ftp_send(c,
							 "220 io-edge-hub "
							 "FTP service ready");
					}
				}
			}
			for (int i = 0; i < FTP_MAX_CLIENTS; i++) {
				if (sessions[i].ctrl >= 0 &&
				    FD_ISSET(sessions[i].ctrl, &rfds)) {
					char line[FTP_BUF_SIZE];
					int rl = recv_line(sessions[i].ctrl,
							   line, sizeof(line));

					if (rl > 0) {
						handle_command(&sessions[i],
							       line);
					} else if (rl == 0) {
						LOG_INF("FTP client %d gone",
							i);
						close_session(&sessions[i]);
					}
					/* rl < 0: 超时, 保留会话等下次 */
				}
			}
		}

		/* 空闲超时检查 */
		{
			uint32_t now = now_ms();

			for (int i = 0; i < FTP_MAX_CLIENTS; i++) {
				if (sessions[i].ctrl >= 0 &&
				    (now - sessions[i].last_activity) >
					    FTP_SESSION_TIMEOUT_MS) {
					LOG_INF("FTP client %d timeout", i);
					close_session(&sessions[i]);
				}
			}
		}
	}
}

void ftpd_start(void)
{
	xTaskCreateStatic(ftp_task, "ftp",
			  sizeof(ftp_stack) / sizeof(ftp_stack[0]), NULL, 3,
			  ftp_stack, &ftp_tcb);
}
