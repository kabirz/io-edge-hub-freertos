/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modbus 寄存器管理 + 配置持久化 (直接映射 holding_reg[])
 * (io-edge-hub Zephyr 版 src/modbus/function.c 的 FreeRTOS 移植)
 *
 * holding_reg[] / input_reg[] 是唯一的参数与采样数据源:
 *   - config_store (A/B slots) 经 holding_reg_save/load 映射 holding_reg[]
 *     (对应 Zephyr settings/FCB 的 modbus/ 命名空间)
 *   - DI/AI 采样线程写入 input_reg[]
 *   - Modbus holding 写 (FC06/FC16) 经 io_write_holding 产生副作用
 *     (DO 输出 / 历史开关 / 设置时间 / 参数保存 / 重启)
 *
 * 与 Zephyr 版的系统性差异 (行为语义保持一致):
 *   - settings_save()/settings_load 回填   -> holding_reg_save()/holding_reg_load()
 *     经 struct io_cfg + config_store (10 键一一对应)
 *   - k_mutex_lock(reg_lock)               -> io_lock()/io_unlock()
 *   - sys_reboot(SYS_REBOOT_COLD)          -> io_reboot_cold() (reboot 前的
 *     k_msleep(100) 移入其实现, 此处不等待)
 *   - time(NULL)                           -> io_now_epoch()
 *   - APP_VERSION_*                        -> FW_VERSION_* (host 测试用 -D 注入)
 *   - 出错码 -ENOTSUP                       -> -1
 */

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "init.h"
#include "io_hooks.h"
#include "config_store.h"

#ifndef FW_VERSION_MAJOR /* host 测试经编译参数注入版本 */
#include "fw_version.h"
#endif

#include "log.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef BIT
#define BIT(n) (1UL << (n))
#endif
#define WRITE_BIT(var, bit, set) \
	((var) = (set) ? ((var) | BIT(bit)) : ((var) & ~BIT(bit)))

/* 寄存器并发保护: holding_reg[] 单字读写对齐时原子, 但 coil 的"读-改-写"
 * (FC05/15 写单个 DO 位) 与 holding_reg_save() 的全量导出读存在并发:
 *  - Modbus TCP/RTU 的 coil 写 (系统工作队列) 与 UDP handler 的 update
 *    并发写 DO 位会丢失更新;
 *  - CFG_SAVE 写回调或 UDP handler 调 holding_reg_save() 期间, 并发
 *    update_holding_reg 会让持久化到半更新状态 (如 IP 只写了前 2 字节)。
 * 用一把互斥锁覆盖这两类临界区 (单字 update/read 本身原子, 不加锁)。 */

/* ==================== 寄存器数组 (唯一数据源) ==================== */
static uint16_t holding_reg[MODBUS_HOLDING_REGISTER_NUMBERS] = {
	[HOLDING_DI_ENABLE_IDX] = 0xFFFF,    /* DI 全使能 */
	[HOLDING_AI_ENABLE_IDX] = 0x000F,    /* AI 全使能 */
	[HOLDING_DI_SAMPLE_MS_IDX] = 200,    /* DI 采样间隔 ms */
	[HOLDING_AI_SAMPLE_MS_IDX] = 200,    /* AI 采样间隔 ms */
	[HOLDING_CAN_ID_IDX] = 0x0111,       /* CAN ID */
	[HOLDING_CAN_BAUDRATE_IDX] = 250,    /* CAN 波特率 x1000, 加载后注入升级库生效 */
	[HOLDING_RS485_BAUDRATE_IDX] = 9600, /* RS485 波特率 */
	[HOLDING_SLAVE_ID_IDX] = 1,          /* Modbus Slave ID */
	[HOLDING_IP_OCTET1_IDX] = 192,       /* 默认 IP 192.168.12.101 */
	[HOLDING_IP_OCTET2_IDX] = 168,       [HOLDING_IP_OCTET3_IDX] = 12,
	[HOLDING_IP_OCTET4_IDX] = 101,
};

static uint16_t input_reg[MODBUS_INPUT_REGISTER_NUMBERS] = {
	/* 主/次版本 <16, 三段塞进 16 位: MAJOR<<12 | MINOR<<8 | PATCH */
	[INPUT_VER_IDX] = ((FW_VERSION_MAJOR << 12) | (FW_VERSION_MINOR << 8) | FW_VERSION_PATCH),
};

/* ==================== 寄存器访问接口 ==================== */
uint16_t get_holding_reg(uint16_t addr)
{
	if (addr >= ARRAY_SIZE(holding_reg)) {
		return 0;
	}
	return holding_reg[addr];
}

/* 读 holding 寄存器 (与 Modbus FC03 读回调同语义): 时间戳寄存器
 * (0x0E/0x0F) 返回实时系统时间, 其余返回数组值。供 Web /api/regs
 * 使用, 保证与 Modbus 主站读到的值一致 */
uint16_t io_read_holding(uint16_t addr)
{
	if (addr == HOLDING_TIMESTAMP_HI_IDX) {
		return (uint16_t)((uint32_t)io_now_epoch() >> 16);
	}
	if (addr == HOLDING_TIMESTAMP_LO_IDX) {
		return (uint16_t)(uint32_t)io_now_epoch();
	}
	return get_holding_reg(addr);
}

/* 内部设值 (无副作用), 供采样/UDP handler 使用 */
int update_holding_reg(uint16_t addr, uint16_t reg)
{
	if (addr >= ARRAY_SIZE(holding_reg)) {
		return -1;
	}
	holding_reg[addr] = reg;
	return 0;
}

uint16_t get_input_reg(uint16_t addr)
{
	if (addr >= ARRAY_SIZE(input_reg)) {
		return 0;
	}
	return input_reg[addr];
}

int update_input_reg(uint16_t addr, uint16_t reg)
{
	if (addr >= ARRAY_SIZE(input_reg)) {
		return -1;
	}
	input_reg[addr] = reg;
	return 0;
}

/* 写 holding 寄存器 (带副作用, 与 Modbus FC06/FC16 语义一致)。
 * 供 Modbus 写回调与 Web (HTTP/WS) 共用, 保证所有写入路径行为一致。
 * 同值写直接返回, 跳过全部副作用 (与 Zephyr 版一致)。 */
int io_write_holding(uint16_t addr, uint16_t reg)
{
	if (addr >= ARRAY_SIZE(holding_reg)) {
		return -1;
	}

	if (holding_reg[addr] == reg) {
		return 0;
	}

	holding_reg[addr] = reg;

	switch (addr) {
	case HOLDING_DO_IDX:
		/* DO 输出 + LED 联动 */
		mb_set_do(reg & 0xFF);
		break;
	case HOLDING_SLAVE_ID_IDX:
		/* RTU/TCP server 的 unit_id 在启动时固定, 需重启生效 */
		LOG_WRN("slave_id change requires reboot");
		break;
	case HOLDING_HISTORY_ENABLE_IDX:
		history_enable_write(reg != 0);
		break;
	case HOLDING_TIMESTAMP_LO_IDX:
		/* 写低16位时, 组合高低位设置 RTC 时间 */
		set_timestamp(
			(time_t)(((uint32_t)holding_reg[HOLDING_TIMESTAMP_HI_IDX] << 16) | reg));
		break;
	case HOLDING_CONFIG_SAVE_IDX:
		/* 写非0 → 全量保存参数到 config_store, 然后恢复为 0 */
		holding_reg[addr] = 0;
		holding_reg_save();
		break;
	case HOLDING_REBOOT_IDX:
		holding_reg[addr] = 0;
		if (reg) {
			history_sync();
			io_reboot_cold();
		}
		break;
	default:
		break;
	}
	return 0;
}

/* 单 DO 位写 (加锁读-改-写, 与 Modbus FC05 语义一致), 供 Web (HTTP/WS) 共用 */
int io_write_do_bit(uint16_t bit, bool state)
{
	uint16_t val;

	if (bit >= DO_NUM) {
		return -1;
	}
	io_lock();
	val = holding_reg[HOLDING_DO_IDX];
	WRITE_BIT(val, bit, state);
	mb_set_do(val & 0xFF);
	holding_reg[HOLDING_DO_IDX] = val & 0xFF;
	io_unlock();
	return 0;
}

/* Coil (FC01) 映射到 holding_reg[DO_IDX] 的位 */
int io_coil_rd(uint16_t addr, bool *state)
{
	if (addr >= DO_NUM) {
		return -1;
	}
	*state = (holding_reg[HOLDING_DO_IDX] & BIT(addr)) != 0;
	return 0;
}

/* Discrete Input (FC02) 映射到 input_reg[DI_IDX] 的位 */
int io_discrete_rd(uint16_t addr, bool *state)
{
	if (addr >= DI_NUM) {
		return -1;
	}
	*state = (input_reg[INPUT_DI_IDX] & BIT(addr)) != 0;
	return 0;
}

/* ==================== 配置持久化 (config_store, 对应 Zephyr settings/FCB) ==================== */

/* IP 合法性: 末字节非 0/0xff, 首字节非 0/127/组播(224-239)/保留(>=240) */
bool ip_addr_valid(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
	(void)b; /* 与 Zephyr 版一致: 仅校验首/末字节 */
	(void)c;
	if (d == 0 || d == 0xFF) {
		return false;
	}
	if (a == 0 || a == 127 || a >= 224) {
		return false;
	}
	return true;
}

/* 导出前校验 holding_reg 中的 IP */
static bool ip_is_valid_for_export(void)
{
	return ip_addr_valid((uint8_t)holding_reg[HOLDING_IP_OCTET1_IDX],
			     (uint8_t)holding_reg[HOLDING_IP_OCTET2_IDX],
			     (uint8_t)holding_reg[HOLDING_IP_OCTET3_IDX],
			     (uint8_t)holding_reg[HOLDING_IP_OCTET4_IDX]);
}

/* 触发全量保存 (供 UDP handler 改参数后持久化; CFG_SAVE 写回调也走这里)。
 * 加锁: 防止导出读全量 holding_reg 期间, 其他线程并发 update 写入导致
 * 持久化到半更新状态 (对应 Zephyr export 回调读 + settings_save 整体临界区)。
 * IP 非法时跳过导出 (对齐 Zephyr mb_handle_export), 保留旧 cfg.ip。 */
void holding_reg_save(void)
{
	struct io_cfg cfg;

	io_lock();
	config_store_get(&cfg); /* 旧值兜底: ip 非法时保留 */
	cfg.di_en = holding_reg[HOLDING_DI_ENABLE_IDX];
	cfg.ai_en = holding_reg[HOLDING_AI_ENABLE_IDX];
	cfg.di_si = holding_reg[HOLDING_DI_SAMPLE_MS_IDX];
	cfg.ai_si = holding_reg[HOLDING_AI_SAMPLE_MS_IDX];
	cfg.his = holding_reg[HOLDING_HISTORY_ENABLE_IDX];
	cfg.can_id = holding_reg[HOLDING_CAN_ID_IDX];
	cfg.can_bps = holding_reg[HOLDING_CAN_BAUDRATE_IDX];
	cfg.rs485_bps = holding_reg[HOLDING_RS485_BAUDRATE_IDX];
	cfg.slave_id = holding_reg[HOLDING_SLAVE_ID_IDX];
	if (ip_is_valid_for_export()) {
		for (int i = 0; i < 4; i++) {
			cfg.ip[i] = holding_reg[HOLDING_IP_OCTET1_IDX + i];
		}
	}
	config_store_save(&cfg);
	io_unlock();
}

/* config_store -> holding_reg 回填 (boot 时 config_store_init 之后调用,
 * 对应 Zephyr settings_load 的 mb_handle_set)。ip 仅当合法时回填。 */
void holding_reg_load(void)
{
	struct io_cfg cfg;

	config_store_get(&cfg);
	holding_reg[HOLDING_DI_ENABLE_IDX] = cfg.di_en;
	holding_reg[HOLDING_AI_ENABLE_IDX] = cfg.ai_en;
	holding_reg[HOLDING_DI_SAMPLE_MS_IDX] = cfg.di_si;
	holding_reg[HOLDING_AI_SAMPLE_MS_IDX] = cfg.ai_si;
	holding_reg[HOLDING_HISTORY_ENABLE_IDX] = cfg.his;
	holding_reg[HOLDING_CAN_ID_IDX] = cfg.can_id;
	holding_reg[HOLDING_CAN_BAUDRATE_IDX] = cfg.can_bps;
	holding_reg[HOLDING_RS485_BAUDRATE_IDX] = cfg.rs485_bps;
	holding_reg[HOLDING_SLAVE_ID_IDX] = cfg.slave_id;
	if (ip_addr_valid((uint8_t)cfg.ip[0], (uint8_t)cfg.ip[1],
			  (uint8_t)cfg.ip[2], (uint8_t)cfg.ip[3])) {
		for (int i = 0; i < 4; i++) {
			holding_reg[HOLDING_IP_OCTET1_IDX + i] = cfg.ip[i];
		}
	}
}
