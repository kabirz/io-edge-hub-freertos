/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * W5500 网络层对外接口: ioLibrary 集成 (SPI2 轮询) + socket 池 + 链路
 * 监控任务。实现见 net/w5500.c; ioLibrary 原始驱动头为
 * deps/ioLibrary/Ethernet/W5500/w5500.h (路径限定包含, 与本文件不冲突,
 * 未限定 #include "w5500.h" 一律解析到本文件)。
 *
 * socket 布局: 0 固定给 UDP 配置通道; 1-3 固定给 Modbus TCP (Task 12);
 * 4-7 进空闲池, 二期 web/FTP 经 sn_alloc 集中领取。
 */

#ifndef APP_W5500_H
#define APP_W5500_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== socket 分配 ==================== */
#define SN_UDP_CFG   0  /* UDP 配置通道 (固定, 不进池) */
#define SN_MB_BASE   1  /* 1/2/3 归 Modbus TCP (固定, 不进池) */
#define SN_POOL_BASE 4  /* sn_alloc 分配下限 (4-7 空闲池) */

/* 初始化 W5500: SPI2 + RST PD0 复位时序 (低/高各 50ms) + ioLibrary
 * 回调注册 + wizchip_init (2KB x 8 socket 缓冲) + 静态网络信息 + PHY
 * 10/100 自协商, 成功后启动 500ms 链路监控任务。
 * 返回 0 成功 (幂等, 重复调用直接成功), -1 失败 (可重试)。 */
int w5500_net_init(const uint8_t mac[6], const uint8_t ip[4],
                   const uint8_t mask[4], const uint8_t gw[4]);

/* w5500_net_init 是否已成功 (未成功时链路查询恒 false) */
bool w5500_net_ready(void);

/* 链路状态: 监控任务 500ms 轮询 PHYCFGR.LNK 的缓存 */
bool w5500_link_up(void);

/* 从空闲池取一个 socket (编号 >= SN_POOL_BASE), 写入 *sn。
 * 返回 0 成功, -1 参数为 NULL 或池空。 */
int sn_alloc(uint8_t *sn);

/* 归还 socket 到空闲池 (超出池范围的编号忽略) */
void sn_free(uint8_t sn);

#ifdef __cplusplus
}
#endif

#endif /* APP_W5500_H */
