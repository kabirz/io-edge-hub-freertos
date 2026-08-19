/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPI 外设实例 (HAL 句柄导出)。
 * SPI1: W25Q128 NOR (PA5 SCK / PA6 MISO / PA7 MOSI, 软件 CS PA4), 42 MHz。
 * SPI2: W5500 以太网 (PB13 SCK / PB14 MISO / PB15 MOSI, 软件 CS PB12,
 * 21 MHz 模式 0 全双工轮询); W5500 复位脚 RST PD0 归 net/w5500.c 管理
 * (复位时序与驱动同文件)。
 */

#ifndef BOARD_SPI_H
#define BOARD_SPI_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

/* W5500 片选: spi2_init 配置, net/w5500.c 的 ioLibrary CS 回调逐操作驱动 */
#define SPI2_W5500_CS_PORT  GPIOB
#define SPI2_W5500_CS_PIN   GPIO_PIN_12

extern SPI_HandleTypeDef hspi1;
extern SPI_HandleTypeDef hspi2;

void spi1_init(void);   /* 时钟 + GPIO + 主机全双工 模式0 8bit MSB /2=42MHz */
void spi2_init(void);   /* 同上, /2=21MHz; 由 w5500_net_init 调用 */

#ifdef __cplusplus
}
#endif

#endif /* BOARD_SPI_H */
