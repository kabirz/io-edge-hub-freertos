/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * W5500 网络层 (Zephyr 版 W5500 驱动的 FreeRTOS 移植, target-only):
 *   - SPI2 轮询 (21 MHz 模式 0) 实现 ioLibrary 回调: CRIS 临界区 /
 *     CS 逐操作 / 字节读写 / 突发读写 (HAL_SPI_Transmit/Receive)
 *   - RST PD0 硬复位时序: 低 >= 50ms -> 高 -> 等 50ms 稳定
 *   - wizchip_init: 2KB x 8 socket 缓冲; 静态 netinfo (无 DHCP,
 *     dns=0.0.0.0); PHY 软件配置 10/100 全双工自协商
 *   - socket 池: 0-3 固定 (UDP 配置 + Modbus TCP), 4-7 空闲供二期
 *   - 500ms 链路监控任务 (prio 4, 栈 256 字): 上升沿 give net_link_sem;
 *     下降沿 DO 全灭 (update_holding_reg + mb_set_do, 对齐 Zephyr
 *     NET_EVENT_IF_DOWN 行为)
 *
 * 并发: 每次寄存器访问 (CS 拉低到拉高整段) 在 ioLibrary 的 CRIS 临界
 * 区内完成 (taskENTER_CRITICAL 屏蔽抢占, 无阻塞调用), Modbus 任务与
 * 监控任务共享 SPI2 安全; socket 池位图同样短临界区保护。
 */

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "main.h"
#include "spi.h"

#include "w5500.h"        /* 本模块对外接口 (include 路径唯一匹配) */
#include "wizchip_conf.h" /* ioLibrary; 内部路径限定包含 W5500/w5500.h */

#include "init.h"
#include "io_hooks.h"

/* LOG 占位 (Task 13 替换为真实日志) */
#define LOG_WRN(...) do {} while (0)
#define LOG_INF(...) do {} while (0)

/* ==================== W5500 板级引脚 / 时序 ==================== */
/* RST PD0: 输出, 空闲高, 低电平复位 (复位脚归本驱动管理, 与 SPI 总线
 * 引脚区分: 总线引脚归 board/spi.c) */
#define W5500_RST_PORT       GPIOD
#define W5500_RST_PIN        GPIO_PIN_0
#define W5500_RST_LOW_MS     50u   /* 复位脉宽 >= 50ms */
#define W5500_RST_SETTLE_MS  50u   /* 释放后稳定等待 */

/* 单次 HAL SPI 轮询超时: 2KB 突发 @21MHz 约 0.8ms, 10ms 裕量充足 */
#define W5500_SPI_TMO_MS     10u

/* W5500 版本寄存器恒为 0x04, 作为 SPI 通路自检 */
#define W5500_VERSIONR_VALUE 0x04u

/* ==================== 状态 ==================== */
static StaticSemaphore_t net_link_sem_cb;
SemaphoreHandle_t net_link_sem;

static StackType_t net_mon_stack[256];
static StaticTask_t net_mon_tcb;

static volatile bool net_ready; /* w5500_net_init 成功 */
static volatile bool link_up;   /* 监控任务缓存 */

/* ==================== socket 池 ==================== */
/* bit n = 1 -> socket n 空闲。0-3 永久预留 (SN_UDP_CFG / Modbus TCP),
 * sn_alloc 仅在 >= SN_POOL_BASE 中取。短临界区, 无阻塞。 */
static uint8_t sn_free_map = (uint8_t)~((1u << SN_POOL_BASE) - 1u);

int sn_alloc(uint8_t *sn)
{
	int ret = -1;

	if (sn == NULL) {
		return -1;
	}
	taskENTER_CRITICAL();
	for (uint8_t i = SN_POOL_BASE; i < _WIZCHIP_SOCK_NUM_; i++) {
		if (sn_free_map & (uint8_t)(1u << i)) {
			sn_free_map &= (uint8_t)~(uint8_t)(1u << i);
			*sn = i;
			ret = 0;
			break;
		}
	}
	taskEXIT_CRITICAL();
	return ret;
}

void sn_free(uint8_t sn)
{
	if (sn < SN_POOL_BASE || sn >= _WIZCHIP_SOCK_NUM_) {
		return; /* 预留段/越界: 归还无意义, 忽略 */
	}
	taskENTER_CRITICAL();
	sn_free_map |= (uint8_t)(1u << sn);
	taskEXIT_CRITICAL();
}

/* ==================== ioLibrary SPI 回调 (SPI2 轮询) ==================== */

/* CRIS: 每次 W5500 寄存器访问 (CS 拉低到拉高) 屏蔽抢占, 保证多任务
 * 共享 SPI2 时帧不被打断。调度器未启动时 (main 里先行 init) 单线程
 * 无并发, 直接跳过 -- 否则 uxCriticalNesting 魔数语义下 enter/exit
 * 不配对会永久掩蔽中断。 */
static void wiz_cris_enter(void)
{
	if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
		taskENTER_CRITICAL();
	}
}

static void wiz_cris_exit(void)
{
	if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
		taskEXIT_CRITICAL();
	}
}

static void wiz_cs_select(void)
{
	HAL_GPIO_WritePin(SPI2_W5500_CS_PORT, SPI2_W5500_CS_PIN, GPIO_PIN_RESET);
}

static void wiz_cs_deselect(void)
{
	HAL_GPIO_WritePin(SPI2_W5500_CS_PORT, SPI2_W5500_CS_PIN, GPIO_PIN_SET);
}

static uint8_t wiz_spi_readbyte(void)
{
	uint8_t v = 0;

	(void)HAL_SPI_Receive(&hspi2, &v, 1, W5500_SPI_TMO_MS);
	return v;
}

static void wiz_spi_writebyte(uint8_t wb)
{
	(void)HAL_SPI_Transmit(&hspi2, &wb, 1, W5500_SPI_TMO_MS);
}

/* 突发: 单 CS 帧内连续收/发 (W5500 VDM 模式, 一次帧头后不限长) */
static void wiz_spi_readburst(uint8_t *buf, uint16_t len)
{
	(void)HAL_SPI_Receive(&hspi2, buf, len, W5500_SPI_TMO_MS);
}

static void wiz_spi_writeburst(uint8_t *buf, uint16_t len)
{
	(void)HAL_SPI_Transmit(&hspi2, buf, len, W5500_SPI_TMO_MS);
}

/* ==================== 链路监控任务 ==================== */

static void net_mon_task(void *arg)
{
	bool prev_up = false;

	(void)arg;
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(500));

		bool up = net_ready &&
			  ((getPHYCFGR() & PHYCFGR_LNK_ON) != 0);

		if (up && !prev_up) {
			/* 上升沿: 通知 boot 等待点 (二值信号量, 重复
			 * give 无副作用; 断线重插同样给出) */
			(void)xSemaphoreGive(net_link_sem);
			LOG_INF("net link up");
		} else if (!up && prev_up) {
			/* 下降沿: DO 全灭 + 影子寄存器清零, 仅边沿触发
			 * 一次 (对齐 Zephyr NET_EVENT_IF_DOWN 行为;
			 * mb_set_do 为 weak 占位, Task 14 覆盖) */
			update_holding_reg(HOLDING_DO_IDX, 0);
			mb_set_do(0);
			LOG_WRN("net link down");
		}
		link_up = up;
		prev_up = up;
	}
}

/* ==================== 查询接口 ==================== */

bool w5500_net_ready(void)
{
	return net_ready;
}

bool w5500_link_up(void)
{
	return link_up;
}

/* ==================== 初始化 ==================== */

static void w5500_hw_reset(void)
{
	GPIO_InitTypeDef io = {0};

	io.Pin = W5500_RST_PIN;
	io.Mode = GPIO_MODE_OUTPUT_PP;
	io.Pull = GPIO_NOPULL;
	io.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(W5500_RST_PORT, &io);

	HAL_GPIO_WritePin(W5500_RST_PORT, W5500_RST_PIN, GPIO_PIN_RESET);
	HAL_Delay(W5500_RST_LOW_MS);
	HAL_GPIO_WritePin(W5500_RST_PORT, W5500_RST_PIN, GPIO_PIN_SET);
	HAL_Delay(W5500_RST_SETTLE_MS);
}

int w5500_net_init(const uint8_t mac[6], const uint8_t ip[4],
		   const uint8_t mask[4], const uint8_t gw[4])
{
	static bool started; /* 信号量只建一次, 保证 net_link_sem 恒有效 */
	uint8_t txsize[_WIZCHIP_SOCK_NUM_];
	uint8_t rxsize[_WIZCHIP_SOCK_NUM_];
	wiz_NetInfo ni = {0};
	wiz_PhyConf phy = {
		.by = PHY_CONFBY_SW,
		.mode = PHY_MODE_AUTONEGO,
		.speed = PHY_SPEED_100,   /* 自协商模式下为能力宣告, 不锁定 */
		.duplex = PHY_DUPLEX_FULL,
	};

	if (net_ready) {
		return 0; /* 幂等 */
	}

	/* 先建信号量再碰硬件: 即使初始化失败, boot 等待点拿到的也是
	 * 永不 give 的空信号量 (超时返回, 与链路断开同语义) */
	if (!started) {
		net_link_sem = xSemaphoreCreateBinaryStatic(&net_link_sem_cb);
		started = true;
	}

	spi2_init();       /* SPI2 外设 + PB12-15 引脚 (含 CS 空闲高) */
	w5500_hw_reset();  /* RST PD0: 低 50ms -> 高 50ms */

	/* ioLibrary 回调注册 (顺序无要求, 须在任何寄存器访问前) */
	reg_wizchip_cris_cbfunc(wiz_cris_enter, wiz_cris_exit);
	reg_wizchip_cs_cbfunc(wiz_cs_select, wiz_cs_deselect);
	reg_wizchip_spi_cbfunc(wiz_spi_readbyte, wiz_spi_writebyte);
	reg_wizchip_spiburst_cbfunc(wiz_spi_readburst, wiz_spi_writeburst);

	/* 2KB x 8 socket 缓冲 (上电默认即此, 显式写一遍确定态) */
	for (int i = 0; i < _WIZCHIP_SOCK_NUM_; i++) {
		txsize[i] = 2;
		rxsize[i] = 2;
	}
	if (wizchip_init(txsize, rxsize) != 0) {
		LOG_WRN("wizchip_init failed");
		return -1;
	}

	/* SPI 通路自检: 读错 (全 0 / 全 FF) 说明总线或芯片不在位 */
	if (getVERSIONR() != W5500_VERSIONR_VALUE) {
		LOG_WRN("W5500 VERSIONR mismatch");
		return -1;
	}

	/* PHY: 软件配置 + 自协商 (内部含 PHY 复位动作) */
	wizphy_setphyconf(&phy);

	/* 静态网络信息 (无 DHCP; dns 不用, 填 0.0.0.0) */
	for (int i = 0; i < 6; i++) {
		ni.mac[i] = mac[i];
	}
	for (int i = 0; i < 4; i++) {
		ni.ip[i] = ip[i];
		ni.sn[i] = mask[i];
		ni.gw[i] = gw[i];
		ni.dns[i] = 0;
	}
	ni.dhcp = NETINFO_STATIC;
	wizchip_setnetinfo(&ni);

	net_ready = true;

	/* 链路监控任务 (prio 4, 栈 256 字): init 成功后才启动 */
	xTaskCreateStatic(net_mon_task, "netmon", 256, NULL, 4,
			  net_mon_stack, &net_mon_tcb);
	LOG_INF("W5500 ready");
	return 0;
}
