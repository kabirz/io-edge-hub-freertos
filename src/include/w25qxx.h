/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * W25Q128 NOR flash (16 MiB) 驱动 — SPI1 @ PA5/PA6/PA7, 软件 CS PA4,
 * 42 MHz (APB2 84 MHz / 2), SPI 模式 0。target-only (HAL SPI)。
 */

#ifndef W25QXX_H
#define W25QXX_H

#include "io_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 SPI1 + 校验 JEDEC ID == 0xEF4018 (W25Q128)。返回 0 / -1。 */
int w25qxx_init(void);

/* io_flash 后端 (16 MiB 全片地址空间): read 任意长度, write 单页内 <=256B,
 * erase 按 addr/len 对齐映射 4K/32K/64K 命令并轮询完成 (期间喂狗)。 */
const struct io_flash *w25qxx_flash(void);

#ifdef __cplusplus
}
#endif

#endif /* W25QXX_H */
