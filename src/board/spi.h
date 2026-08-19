/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPI 外设实例 (HAL 句柄导出)。
 * SPI1: W25Q128 NOR (PA5 SCK / PA6 MISO / PA7 MOSI, 软件 CS PA4), 42 MHz。
 * SPI2: 任务 9 外设, 引脚届时定义。
 */

#ifndef BOARD_SPI_H
#define BOARD_SPI_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

extern SPI_HandleTypeDef hspi1;
extern SPI_HandleTypeDef hspi2;

void spi1_init(void);   /* 时钟 + GPIO + 主机全双工 模式0 8bit MSB /2=42MHz */
void spi2_init(void);   /* 外设配置就绪; 引脚在任务 9 接入 */

#ifdef __cplusplus
}
#endif

#endif /* BOARD_SPI_H */
