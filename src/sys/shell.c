/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 调试 shell (USART1, 与日志同一串口) — Zephyr 版 SHELL 的最小对应物。
 *
 * 接收路径: 寄存器级 RXNE 中断 -> 环形缓冲 -> shell 任务行编辑
 * (回显/退格/CRLF 收敛/左右移动光标/插入删除/上下翻历史/Tab 补全)。
 * 不走 HAL_UART_Receive_IT: HAL 的单把 huart Lock 会被日志任务
 * 长时间持有, ISR 里重挂接收遇到 HAL_BUSY 即断流; 寄存器级 RX
 * 与 TX 互不干扰。
 *
 * 输出经 log_line/log_raw: 与日志同一把锁, 行级不交织。
 *
 * 命令集:
 *   help / tasks / reboot / io <子命令>   (Tab 补全, 上下文感知)
 * io 子命令对齐 Zephyr 版 src/shell.c, 写路径复用 io_write_holding /
 * io_write_do_bit, 与 Modbus/Web(HTTP/WS)/UDP 副作用一致:
 *   io                  -- IO/配置总览
 *   io info             -- 版本/MAC/IP/链路/RS485/CAN 基本信息
 *   io di               -- DI1-16 状态
 *   io do / io do set n v
 *   io ai               -- AI1-4 工程量 (0.01mA / 0.01V)
 *   io rs485 [baud n|sid n]
 *   io can [id n|bps n]
 *   io ip a.b.c.d       -- 静态 IP (保存+重启生效)
 *   io reg [a [v]]      -- 寄存器全量 dump / 单读 / 单写
 *   io save             -- 参数持久化 (config_store)
 *   io factory          -- 恢复出厂 (擦参数区 + 延迟重启)
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"

#include "main.h"          /* huart1 / derive_mac_from_uid */
#include "log.h"           /* log_line / log_raw */
#include "shell.h"
#include "init.h"          /* 寄存器模型 + io_write_* */
#include "io_time.h"       /* io_now_epoch */
#include "io_hooks.h"      /* set_reboot_status */
#include "config_store.h"  /* config_store_erase_all */
#include "fw_version.h"    /* FW_VERSION_* / FW_GIT_VERSION */
#include "w5500.h"         /* w5500_link_up */

#define SH_RING_MAX  128u  /* ISR->任务 环形缓冲 */
#define SH_LINE_MAX  96u   /* 行缓冲 (含 NUL) */
#define SH_ARG_MAX   6     /* "io do set 3 1" 最深 5 词 */
#define SH_HIST_MAX  8u    /* 历史命令条数 */
#define SH_TASK_PRIO 1
#define SH_TASK_STACK 640  /* 字 = 2560B: snprintf/编辑 memmove/重绘缓冲 */

static StaticTask_t sh_tcb;
static StackType_t sh_stack[SH_TASK_STACK];
static TaskHandle_t sh_task;

static volatile uint8_t sh_ring[SH_RING_MAX];
static volatile uint8_t sh_head; /* ISR 写 */
static uint8_t sh_tail;         /* 任务读 */

/* ==================== RX: 寄存器级中断 ==================== */

/* 强符号覆盖启动文件弱 Default_Handler。SR 读 + DR 读序列顺带清
 * ORE/FE/NE/PE (含 RXNE 未置位时的 ORE), 无需 HAL 错误回调。 */
void USART1_IRQHandler(void)
{
	uint32_t sr = USART1->SR;
	uint8_t c;
	uint8_t next;
	BaseType_t woken = pdFALSE;

	if ((sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE)) != 0) {
		(void)USART1->DR;
		sr = USART1->SR; /* 错误标志清后重读, RXNE 可能随之置位 */
	}
	if ((sr & USART_SR_RXNE) != 0) {
		c = (uint8_t)USART1->DR;
		next = (uint8_t)((sh_head + 1u) % SH_RING_MAX);
		if (next != sh_tail) { /* 满则丢弃 (人类输入远达不到) */
			sh_ring[sh_head] = c;
			sh_head = next;
		}
		if (sh_task != NULL) {
			vTaskNotifyGiveFromISR(sh_task, &woken);
		}
	}
	portYIELD_FROM_ISR(woken);
}

static uint8_t sh_getchar(void)
{
	uint8_t c;

	while (sh_tail == sh_head) {
		(void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
	}
	c = sh_ring[sh_tail];
	sh_tail = (uint8_t)((sh_tail + 1u) % SH_RING_MAX);
	return c;
}

/* ==================== 行组装 ==================== */

static void sh_prompt(void)
{
	log_raw("io> ", 4);
}

/* ==================== 历史命令 ==================== */

static char sh_hist[SH_HIST_MAX][SH_LINE_MAX];
static uint8_t sh_hist_len;    /* 已存条数 (<= SH_HIST_MAX) */
static uint8_t sh_hist_newest; /* 最新条下标 (环形) */

/* 执行后存一条; 与最新条相同则跳过 (连续重复去重) */
static void hist_store(const char *line)
{
	if (sh_hist_len != 0u &&
	    strcmp(sh_hist[sh_hist_newest], line) == 0) {
		return;
	}
	sh_hist_newest = (uint8_t)((sh_hist_newest + 1u) % SH_HIST_MAX);
	/* line 已按 SH_LINE_MAX-1 截断为 NUL 终止串 */
	strcpy(sh_hist[sh_hist_newest], line);
	if (sh_hist_len < SH_HIST_MAX) {
		sh_hist_len++;
	}
}

/* nav: 0=最新, sh_hist_len-1=最旧; 越界返回 NULL */
static const char *hist_get(uint8_t nav)
{
	if (nav >= sh_hist_len) {
		return NULL;
	}
	return sh_hist[(uint8_t)((sh_hist_newest + SH_HIST_MAX - nav) %
				 SH_HIST_MAX)];
}

/* 整行重绘: \r + prompt + 行 + 清行尾, 光标回退到 pos。
 * 行中编辑 (插入/中段删除/历史召回) 后调用; 终端须支持 ANSI 序列
 * (\x1b[K / \x1b[nD), PuTTY/SecureCRT/串口工具通用 */
static void sh_redraw(const char *line, uint16_t n, uint16_t pos)
{
	char buf[SH_LINE_MAX + 16];
	int m = snprintf(buf, sizeof(buf), "\rio> %.*s\x1b[K", (int)n, line);

	if (m > 0 && pos < n) {
		m += snprintf(buf + m, sizeof(buf) - (size_t)m, "\x1b[%uD",
			      (unsigned)(n - pos));
	}
	if (m > 0) {
		log_raw(buf, (uint16_t)m);
	}
}

static int sh_split(char *line, char *argv[], int max)
{
	int argc = 0;
	char *p = line;

	while (*p != '\0' && argc < max) {
		while (*p == ' ' || *p == '\t') {
			*p++ = '\0';
		}
		if (*p == '\0') {
			break;
		}
		argv[argc++] = p;
		while (*p != ' ' && *p != '\t' && *p != '\0') {
			p++;
		}
	}
	*p = '\0';
	return argc;
}

static bool parse_ul(const char *s, unsigned long *out)
{
	char *end;
	unsigned long v;

	if (s == NULL || *s == '\0') {
		return false;
	}
	v = strtoul(s, &end, 0); /* base 0: 十进制/0x 前缀都收 */
	if (*end != '\0') {
		return false;
	}
	*out = v;
	return true;
}

static bool parse_ip(const char *s, uint8_t ip[4])
{
	const char *p = s;
	unsigned long v;
	int i;

	for (i = 0; i < 4; i++) {
		char *end;

		v = strtoul(p, &end, 10);
		if (end == p || v > 255 ||
		    (i < 3 && *end != '.') || (i == 3 && *end != '\0')) {
			return false;
		}
		ip[i] = (uint8_t)v;
		p = end + 1;
	}
	return true;
}

/* ==================== 顶层命令: help / tasks / reboot ==================== */

static void cmd_help(void)
{
	log_line("commands:");
	log_line("  help    this help");
	log_line("  tasks   task list (state / priority / min stack)");
	log_line("  reboot  graceful reboot (history sync + ~3s)");
	log_line("  io      io-edge-hub debug commands ('io help')");
}

static char task_state_char(eTaskState s)
{
	switch (s) {
	case eRunning:
		return 'X';
	case eReady:
		return 'R';
	case eBlocked:
		return 'B';
	case eSuspended:
		return 'S';
	case eDeleted:
		return 'D';
	default:
		return '?';
	}
}

static void cmd_tasks(void)
{
	UBaseType_t n = uxTaskGetNumberOfTasks();
	TaskStatus_t *st = pvPortMalloc(n * sizeof(TaskStatus_t));
	UBaseType_t m;
	UBaseType_t i;

	if (st == NULL) {
		log_line("tasks: out of memory");
		return;
	}
	m = uxTaskGetSystemState(st, n, NULL);
	log_line("%-16s st  prio  stack  num", "task");
	for (i = 0; i < m; i++) {
		log_line("%-16s %c   %-4u  %-5u  %u",
			 st[i].pcTaskName, task_state_char(st[i].eCurrentState),
			 (unsigned)st[i].uxCurrentPriority,
			 (unsigned)st[i].usStackHighWaterMark,
			 (unsigned)st[i].xTaskNumber);
	}
	vPortFree(st);
	log_line("st: X=running R=ready B=blocked S=suspended; "
		 "stack = min free (words)");
}

static void cmd_reboot(void)
{
	log_line("rebooting (history sync + ~3s)...");
	set_reboot_status(true); /* 心跳任务轮询 -> history_sync -> 冷重启 */
}

/* ==================== io 子命令 (对齐 Zephyr src/shell.c) ==================== */

static void cmd_io_info(void)
{
	uint8_t mac[6];
	char time_str[20] = "1970-01-01 00:00:00";
	struct tm tm;
	time_t lt;

	derive_mac_from_uid(mac);

	log_line("version : v%d.%d.%d_%s", FW_VERSION_MAJOR, FW_VERSION_MINOR,
		 FW_VERSION_PATCH, FW_GIT_VERSION);
	log_line("build   : %s %s", __DATE__, __TIME__);
	log_line("board   : %s", "io_edge_f407vet6");
	log_line("mac     : %02x:%02x:%02x:%02x:%02x:%02x",
		 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	log_line("ip      : %u.%u.%u.%u/24",
		 io_read_holding(HOLDING_IP_OCTET1_IDX),
		 io_read_holding(HOLDING_IP_OCTET2_IDX),
		 io_read_holding(HOLDING_IP_OCTET3_IDX),
		 io_read_holding(HOLDING_IP_OCTET4_IDX));
	log_line("link    : %s", w5500_link_up() ? "up" : "down");
	log_line("rs485   : %u bps, slave id %u (8N1)",
		 io_read_holding(HOLDING_RS485_BAUDRATE_IDX),
		 io_read_holding(HOLDING_SLAVE_ID_IDX));
	log_line("can     : id 0x%03x, %u kbit/s",
		 io_read_holding(HOLDING_CAN_ID_IDX),
		 io_read_holding(HOLDING_CAN_BAUDRATE_IDX));
	log_line("uptime  : %u s",
		 (unsigned)(xTaskGetTickCount() / configTICK_RATE_HZ));

	/* RTC 存 UTC, 显示时加时区偏移 (对齐 Zephyr 版 +8 习惯) */
	lt = (time_t)io_now_epoch() + 8LL * 3600;
	if (gmtime_r(&lt, &tm) != NULL && tm.tm_year + 1900 >= 2020) {
		(void)strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S",
			       &tm);
	}
	log_line("time    : %s (%u)", time_str, (unsigned)io_now_epoch());
}

static void cmd_io_di(void)
{
	uint16_t di = get_input_reg(INPUT_DI_IDX);
	int row;

	log_line("DI: 0x%04x (enable: 0x%04x)", di,
		 get_holding_reg(HOLDING_DI_ENABLE_IDX));
	for (row = 0; row < DI_NUM; row += 8) {
		log_line("DI%-2d-%-2d : %u %u %u %u %u %u %u %u", row + 1, row + 8,
			 (di >> (row + 0)) & 1, (di >> (row + 1)) & 1,
			 (di >> (row + 2)) & 1, (di >> (row + 3)) & 1,
			 (di >> (row + 4)) & 1, (di >> (row + 5)) & 1,
			 (di >> (row + 6)) & 1, (di >> (row + 7)) & 1);
	}
}

static void cmd_io_do(void)
{
	uint16_t do_v = get_holding_reg(HOLDING_DO_IDX);
	int row;

	log_line("DO: 0x%02x", do_v & 0xFF);
	for (row = 0; row < DO_NUM; row += 8) {
		log_line("DO%-2d-%-2d : %u %u %u %u %u %u %u %u", row + 1, row + 8,
			 (do_v >> (row + 0)) & 1, (do_v >> (row + 1)) & 1,
			 (do_v >> (row + 2)) & 1, (do_v >> (row + 3)) & 1,
			 (do_v >> (row + 4)) & 1, (do_v >> (row + 5)) & 1,
			 (do_v >> (row + 6)) & 1, (do_v >> (row + 7)) & 1);
	}
}

/* io do set <ch 1-8> <0|1> */
static void cmd_io_do_set(char *ch_s, char *val_s)
{
	unsigned long ch;
	unsigned long val;

	if (!parse_ul(ch_s, &ch) || ch < 1 || ch > DO_NUM) {
		log_line("invalid channel: %s (1-%d)",
			 ch_s != NULL ? ch_s : "", DO_NUM);
		return;
	}
	if (!parse_ul(val_s, &val) || val > 1) {
		log_line("invalid value: %s (0/1)",
			 val_s != NULL ? val_s : "");
		return;
	}
	if (io_write_do_bit((uint16_t)(ch - 1), val != 0) != 0) {
		log_line("write failed");
		return;
	}
	log_line("DO%lu = %lu (DO: 0x%02x)", ch, val,
		 get_holding_reg(HOLDING_DO_IDX) & 0xFF);
}

static void cmd_io_ai(void)
{
	static const char *unit[AI_NUM] = {"mA", "mA", "V", "V"};
	uint16_t raw;
	int i;

	log_line("AI enable: 0x%01x",
		 get_holding_reg(HOLDING_AI_ENABLE_IDX) & 0x0F);
	for (i = 0; i < AI_NUM; i++) {
		raw = get_input_reg(INPUT_AI0_IDX + i);
		/* input_reg 存 0.01mA (AI1/2) / 0.01V (AI3/4), 展开为工程量 */
		log_line("AI%d: %5u.%02u %s (raw %u)", i + 1, raw / 100,
			 raw % 100, unit[i], raw);
	}
}

/* io reg [addr [value]]: 无参全量 / 单读 / 单写 */
static void cmd_io_reg(int argc, char **argv)
{
	char row[96];
	unsigned long addr;
	unsigned long val;
	int i;
	int n;

	if (argc == 0) {
		log_line("holding registers (%d):",
			 MODBUS_HOLDING_REGISTER_NUMBERS);
		for (i = 0; i < MODBUS_HOLDING_REGISTER_NUMBERS; i += 6) {
			n = 0;
			row[0] = '\0';
			for (int j = i;
			     j < i + 6 && j < MODBUS_HOLDING_REGISTER_NUMBERS;
			     j++) {
				n += snprintf(row + n, sizeof(row) - (size_t)n,
					      "%s0x%02x=%u", (j % 6) ? " " : "",
					      j, io_read_holding((uint16_t)j));
			}
			log_line("%s", row);
		}
		log_line("input registers (%d):",
			 MODBUS_INPUT_REGISTER_NUMBERS);
		for (i = 0; i < MODBUS_INPUT_REGISTER_NUMBERS; i += 6) {
			n = 0;
			row[0] = '\0';
			for (int j = i;
			     j < i + 6 && j < MODBUS_INPUT_REGISTER_NUMBERS;
			     j++) {
				n += snprintf(row + n, sizeof(row) - (size_t)n,
					      "%s0x%02x=%u", (j % 6) ? " " : "",
					      j, get_input_reg((uint16_t)j));
			}
			log_line("%s", row);
		}
		return;
	}

	if (!parse_ul(argv[0], &addr) ||
	    addr >= MODBUS_HOLDING_REGISTER_NUMBERS) {
		log_line("invalid addr: %s", argv[0]);
		return;
	}
	if (argc == 1) {
		log_line("holding[0x%02lx] = %u", addr,
			 io_read_holding((uint16_t)addr));
		return;
	}

	if (!parse_ul(argv[1], &val) || val > 0xFFFF) {
		log_line("invalid value: %s (0-65535)", argv[1]);
		return;
	}
	if (io_write_holding((uint16_t)addr, (uint16_t)val) != 0) {
		log_line("write failed");
		return;
	}
	log_line("holding[0x%02lx] = %u", addr,
		 io_read_holding((uint16_t)addr));
}

static void cmd_io_help(void)
{
	log_line("io                     IO/config overview");
	log_line("io info                version / mac / ip / link / uptime");
	log_line("io di                  DI1-16 status");
	log_line("io do [set ch 0|1]     DO1-8 status / control");
	log_line("io ai                  AI1-4 values (mA / V)");
	log_line("io rs485 [baud n|sid n]");
	log_line("io can [id n|bps n]");
	log_line("io ip a.b.c.d          static ip (saved)");
	log_line("io reg [addr [value]]  register dump / read / write");
	log_line("io save                persist parameters");
	log_line("io factory             factory reset + reboot");
}

static void io_dispatch(int argc, char **argv)
{
	unsigned long v;
	uint8_t ip[4];

	if (argc == 0) {
		log_line("DI: 0x%04x  DO: 0x%02x  AI: %u %u %u %u",
			 get_input_reg(INPUT_DI_IDX),
			 get_holding_reg(HOLDING_DO_IDX) & 0xFF,
			 get_input_reg(INPUT_AI0_IDX), get_input_reg(INPUT_AI0_IDX + 1),
			 get_input_reg(INPUT_AI0_IDX + 2), get_input_reg(INPUT_AI0_IDX + 3));
		log_line("rs485: %u bps sid %u | can: 0x%03x %u kbit/s",
			 get_holding_reg(HOLDING_RS485_BAUDRATE_IDX),
			 get_holding_reg(HOLDING_SLAVE_ID_IDX),
			 get_holding_reg(HOLDING_CAN_ID_IDX),
			 get_holding_reg(HOLDING_CAN_BAUDRATE_IDX));
		log_line("('io help' for subcommands)");
		return;
	}

	if (strcmp(argv[0], "help") == 0) {
		cmd_io_help();
	} else if (strcmp(argv[0], "info") == 0) {
		cmd_io_info();
	} else if (strcmp(argv[0], "di") == 0) {
		cmd_io_di();
	} else if (strcmp(argv[0], "do") == 0) {
		if (argc >= 4 && strcmp(argv[1], "set") == 0) {
			cmd_io_do_set(argv[2], argv[3]);
		} else {
			cmd_io_do();
		}
	} else if (strcmp(argv[0], "ai") == 0) {
		cmd_io_ai();
	} else if (strcmp(argv[0], "rs485") == 0) {
		if (argc >= 3 && strcmp(argv[1], "baud") == 0 &&
		    parse_ul(argv[2], &v) && v >= 1200 && v <= 115200) {
			(void)io_write_holding(HOLDING_RS485_BAUDRATE_IDX,
					       (uint16_t)v);
			log_line("rs485 baud -> %u (reboot to apply, "
				 "'io save' to persist)",
				 io_read_holding(HOLDING_RS485_BAUDRATE_IDX));
		} else if (argc >= 3 && strcmp(argv[1], "sid") == 0 &&
			   parse_ul(argv[2], &v) && v >= 1 && v <= 247) {
			(void)io_write_holding(HOLDING_SLAVE_ID_IDX,
					       (uint16_t)v);
			log_line("slave id -> %u (reboot to apply, "
				 "'io save' to persist)",
				 io_read_holding(HOLDING_SLAVE_ID_IDX));
		} else {
			log_line("rs485: %u bps, slave id %u (8N1)",
				 io_read_holding(HOLDING_RS485_BAUDRATE_IDX),
				 io_read_holding(HOLDING_SLAVE_ID_IDX));
			log_line("(changes take effect after reboot)");
		}
	} else if (strcmp(argv[0], "can") == 0) {
		if (argc >= 3 && strcmp(argv[1], "id") == 0 &&
		    parse_ul(argv[2], &v) && v >= 1 && v <= 0x7FF) {
			(void)io_write_holding(HOLDING_CAN_ID_IDX,
					       (uint16_t)v);
			log_line("can id -> 0x%03x (reboot to apply, "
				 "'io save' to persist)",
				 io_read_holding(HOLDING_CAN_ID_IDX));
		} else if (argc >= 3 && strcmp(argv[1], "bps") == 0 &&
			   parse_ul(argv[2], &v) &&
			   (v == 50 || v == 100 || v == 125 || v == 250 ||
			    v == 500 || v == 800 || v == 1000)) {
			(void)io_write_holding(HOLDING_CAN_BAUDRATE_IDX,
					       (uint16_t)v);
			log_line("can bps -> %u kbit/s (reboot to apply, "
				 "'io save' to persist)",
				 io_read_holding(HOLDING_CAN_BAUDRATE_IDX));
		} else {
			log_line("can: id 0x%03x, %u kbit/s",
				 io_read_holding(HOLDING_CAN_ID_IDX),
				 io_read_holding(HOLDING_CAN_BAUDRATE_IDX));
			log_line("(changes take effect after reboot)");
		}
	} else if (strcmp(argv[0], "ip") == 0) {
		if (argc < 2 || !parse_ip(argv[1], ip) ||
		    !ip_addr_valid(ip[0], ip[1], ip[2], ip[3])) {
			log_line("invalid ip: %s",
				 argc >= 2 ? argv[1] : "");
			return;
		}
		(void)io_write_holding(HOLDING_IP_OCTET1_IDX, ip[0]);
		(void)io_write_holding(HOLDING_IP_OCTET2_IDX, ip[1]);
		(void)io_write_holding(HOLDING_IP_OCTET3_IDX, ip[2]);
		(void)io_write_holding(HOLDING_IP_OCTET4_IDX, ip[3]);
		holding_reg_save();
		log_line("ip -> %u.%u.%u.%u (saved, reboot to apply)",
			 ip[0], ip[1], ip[2], ip[3]);
	} else if (strcmp(argv[0], "reg") == 0) {
		cmd_io_reg(argc - 1, argv + 1);
	} else if (strcmp(argv[0], "save") == 0) {
		holding_reg_save();
		log_line("parameters saved");
	} else if (strcmp(argv[0], "factory") == 0) {
		config_store_erase_all();
		set_reboot_status(true);
		log_line("factory reset done, rebooting "
			 "(defaults after reboot)");
	} else {
		log_line("unknown io command: %s ('io help')", argv[0]);
	}
}

static void sh_dispatch(char *line)
{
	char *argv[SH_ARG_MAX];
	int argc = sh_split(line, argv, SH_ARG_MAX);

	if (argc == 0) {
		return;
	}
	if (strcmp(argv[0], "help") == 0) {
		cmd_help();
	} else if (strcmp(argv[0], "tasks") == 0 ||
		   strcmp(argv[0], "ps") == 0) {
		cmd_tasks();
	} else if (strcmp(argv[0], "reboot") == 0) {
		cmd_reboot();
	} else if (strcmp(argv[0], "io") == 0) {
		io_dispatch(argc - 1, argv + 1);
	} else {
		log_line("unknown command: %s (help)", argv[0]);
	}
}

/* ==================== 任务 ==================== */

/* ==================== Tab 补全 (命令树) ==================== */

struct sh_cmd {
	const char *name;
	const struct sh_cmd *sub; /* 子命令表, NULL = 叶子 (后接参数) */
};

static const struct sh_cmd io_do_cmds[] = {
	{"set", NULL},
	{NULL, NULL},
};

static const struct sh_cmd io_rs485_cmds[] = {
	{"baud", NULL},
	{"sid", NULL},
	{NULL, NULL},
};

static const struct sh_cmd io_can_cmds[] = {
	{"id", NULL},
	{"bps", NULL},
	{NULL, NULL},
};

static const struct sh_cmd io_cmds[] = {
	{"help", NULL},   {"info", NULL},  {"di", NULL},
	{"do", io_do_cmds}, {"ai", NULL},  {"rs485", io_rs485_cmds},
	{"can", io_can_cmds}, {"ip", NULL}, {"reg", NULL},
	{"save", NULL},   {"factory", NULL},
	{NULL, NULL},
};

static const struct sh_cmd root_cmds[] = {
	{"help", NULL}, {"tasks", NULL}, {"ps", NULL}, {"reboot", NULL},
	{"io", io_cmds},
	{NULL, NULL},
};

/* 沿命令树解析已键入的完整词, 返回末词所在的候选表 (NULL = 参数区/
 * 未知命令, 不补全); last/len 输出行尾未完成词 (行尾空格则 len=0) */
static const struct sh_cmd *complete_level(const char *line,
					   const char **last, size_t *len)
{
	const struct sh_cmd *tbl = root_cmds;
	const char *p = line;

	while (*p != '\0') {
		while (*p == ' ' || *p == '\t') {
			p++;
		}
		if (*p == '\0') {
			break; /* 行尾空白: 末词为空 */
		}

		{
			const char *w = p;

			while (*p != '\0' && *p != ' ' && *p != '\t') {
				p++;
			}
			if (*p == '\0') { /* 未完成末词 */
				*last = w;
				*len = (size_t)(p - w);
				return tbl;
			}

			/* 完整词: 命令树下降, 不在树上的词视为参数 */
			if (tbl != NULL) {
				const struct sh_cmd *c;

				for (c = tbl; c->name != NULL; c++) {
					if (strlen(c->name) == (size_t)(p - w) &&
					    strncmp(c->name, w, (size_t)(p - w)) == 0) {
						tbl = c->sub;
						break;
					}
				}
				if (c->name == NULL) {
					tbl = NULL;
				}
			}
		}
	}

	*last = p; /* 空行/行尾空白 */
	*len = 0;
	return tbl;
}

/* Tab: 唯一匹配补全 (+空格); 多匹配延伸公共前缀并列出候选 */
static void sh_complete(char *line, uint16_t *n)
{
	const struct sh_cmd *tbl;
	const struct sh_cmd *cand[16];
	const char *last;
	const char *ext;
	size_t len;
	size_t lcp;
	int ncand = 0;
	int i;

	tbl = complete_level(line, &last, &len);
	if (tbl == NULL) {
		return; /* 参数区: 不补全 */
	}

	for (const struct sh_cmd *c = tbl; c->name != NULL; c++) {
		if (strncmp(c->name, last, len) == 0 && ncand < 16) {
			cand[ncand++] = c;
		}
	}
	if (ncand == 0) {
		return;
	}

	if (ncand == 1) {
		uint16_t old_n = *n;

		ext = &cand[0]->name[len]; /* 缺失后缀 (可能为空) */
		while (*ext != '\0' && *n + 2u < SH_LINE_MAX) {
			line[(*n)++] = *ext++;
		}
		line[(*n)++] = ' '; /* 补全的词后接参数/空格 */
		line[*n] = '\0';
		log_raw(&line[old_n], (uint16_t)(*n - old_n));
		return;
	}

	/* 多匹配: 先延伸最长公共前缀 */
	lcp = strlen(cand[0]->name);
	for (i = 1; i < ncand; i++) {
		size_t j = 0;

		while (j < lcp && cand[0]->name[j] == cand[i]->name[j]) {
			j++;
		}
		lcp = j;
	}
	while (len < lcp && *n + 1u < SH_LINE_MAX) {
		line[(*n)++] = cand[0]->name[len++];
	}
	line[*n] = '\0';

	/* 列出候选并重印当前行 */
	log_raw("\r\n", 2);
	{
		char row[128];
		int m = 0;

		row[0] = '\0';
		for (i = 0; i < ncand; i++) {
			m += snprintf(row + m, sizeof(row) - (size_t)m, "%s%s",
				      i ? "  " : "", cand[i]->name);
		}
		log_line("%s", row);
	}
	sh_prompt();
	log_raw(line, (uint16_t)*n);
}

static void sh_task_fn(void *arg)
{
	char line[SH_LINE_MAX];
	char draft[SH_LINE_MAX]; /* 历史浏览中暂存的未提交行 */
	uint16_t n = 0;
	uint16_t pos = 0;    /* 光标 (0..n) */
	uint8_t esc = 0;     /* ANSI 转义状态: 0=无 1=ESC 2='['/'O' 后 */
	int8_t nav = -1;     /* 历史浏览偏移, -1=不在浏览 */
	bool prev_cr = false;

	(void)arg;

	/* line 全程保持 NUL 终止 (FreeRTOS 建任务时栈填充 0xA5 而非清零,
	 * dispatch 后也留有旧词残留; complete_level 依赖 C 字符串边界) */
	line[0] = '\0';

	log_line("");
	log_line("shell ready (help for commands)");
	sh_prompt();

	for (;;) {
		uint8_t c = sh_getchar();
		bool arrow = false;

		/* ANSI 转义序列: ESC [ <param> <A-D>. 参数位 (数字/';')
		 * 吞掉 -- 覆盖 ESC[3~ (Del) 与 ESC[1;5D (Ctrl+方向) 等;
		 * '~' 结尾的功能键整段忽略。'O' 为应用模式方向键前缀 */
		if (esc != 0) {
			if (esc == 1u && (c == '[' || c == 'O')) {
				esc = 2;
				continue;
			}
			if (esc == 2u &&
			    ((c >= '0' && c <= '9') || c == ';')) {
				continue;
			}
			arrow = (c >= 'A' && c <= 'D');
			esc = 0;
			if (!arrow) {
				continue; /* '~' 等功能键尾: 忽略 */
			}
		} else if (c == 0x1B) {
			esc = 1;
			continue;
		}

		if (arrow) {
			if (c == 'C') { /* 右 */
				if (pos < n) {
					pos++;
					log_raw("\x1b[C", 3);
				}
			} else if (c == 'D') { /* 左 */
				if (pos > 0) {
					pos--;
					log_raw("\x1b[D", 3);
				}
			} else if (c == 'A') { /* 上: 往旧翻 */
				const char *h;

				if (nav < 0) {
					strcpy(draft, line); /* 暂存草稿 */
					nav = 0;
				} else if (nav + 1 < (int8_t)sh_hist_len) {
					nav++;
				}
				h = hist_get((uint8_t)nav);
				if (h != NULL) {
					strcpy(line, h);
					n = (uint16_t)strlen(h);
					pos = n;
					sh_redraw(line, n, pos);
				}
			} else { /* 下: 往新翻, 翻出最新回草稿 */
				const char *h;

				if (nav < 0) {
					continue;
				}
				nav--;
				h = (nav < 0) ? draft : hist_get((uint8_t)nav);
				if (h != NULL) {
					strcpy(line, h);
					n = (uint16_t)strlen(h);
					pos = n;
					sh_redraw(line, n, pos);
				}
			}
			continue;
		}

		if (c == '\n' && prev_cr) { /* CRLF 序列的 LF 不再触发 */
			prev_cr = false;
			continue;
		}
		prev_cr = false;
		if (c == '\r' || c == '\n') {
			prev_cr = (c == '\r');
			log_raw("\r\n", 2);
			if (n != 0) {
				hist_store(line);
				nav = -1;
				sh_dispatch(line);
				n = 0;
				pos = 0;
				line[0] = '\0';
			}
			sh_prompt();
		} else if (c == 0x08 || c == 0x7F) { /* BS / DEL 删光标前 */
			if (pos > 0) {
				memmove(&line[pos - 1u], &line[pos],
					(size_t)(n - pos));
				n--;
				pos--;
				line[n] = '\0';
				if (pos == n) {
					log_raw("\b \b", 3); /* 行尾快路径 */
				} else {
					sh_redraw(line, n, pos);
				}
			}
		} else if (c == '\t') { /* Tab 补全 (补全词取行尾) */
			if (pos < n) { /* 光标在行中: 先跳到行尾 */
				char csi[8];
				int m = snprintf(csi, sizeof(csi),
						 "\x1b[%uC", n - pos);

				if (m > 0) {
					log_raw(csi, (uint16_t)m);
				}
				pos = n;
			}
			sh_complete(line, &n);
			pos = n;
		} else if (c >= 0x20 && c < 0x7F && n + 2u < sizeof(line)) {
			if (pos == n) { /* 追加 (常见路径) */
				line[n++] = (char)c;
				pos = n;
				line[n] = '\0';
				log_raw((const char *)&c, 1);
			} else { /* 行中插入: 后段右移 + 整行重绘 */
				memmove(&line[pos + 1u], &line[pos],
					(size_t)(n - pos));
				line[pos++] = (char)c;
				n++;
				line[n] = '\0';
				sh_redraw(line, n, pos);
			}
		}
		/* 其余控制字符忽略 */
	}
}

void shell_start(void)
{
	/* log_init 已完成 UART 初始化 (TX/RX 引脚 + 115200 8N1) */
	sh_task = xTaskCreateStatic(sh_task_fn, "sh", SH_TASK_STACK, NULL,
				    SH_TASK_PRIO, sh_stack, &sh_tcb);
	if (sh_task == NULL) {
		LOG_ERR("shell task create failed");
		return;
	}

	/* RXNE 中断 (仅 RX; TX 保持日志轮询路径)。优先级 7: 数值大于
	 * configMAX_SYSCALL_INTERRUPT_PRIORITY(5), 允许调用 FromISR API */
	HAL_NVIC_SetPriority(USART1_IRQn, 7, 0);
	HAL_NVIC_EnableIRQ(USART1_IRQn);
	USART1->CR1 |= USART_CR1_RXNEIE;
}
