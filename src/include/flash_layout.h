/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 全局 flash 分区布局 (boot 与 app 共用, 与 Zephyr 版 io-edge-hub
 * 板级 DTS 分区一一对齐):
 *
 *   内部 flash 512KB @0x08000000:
 *     [0x000000, 0x010000)  mcuboot (boot 引导, 64KB)
 *     [0x010000, 0x080000)  slot0   (主应用, 448KB)
 *   外部 W25Q128 16MiB @SPI1:
 *     [0x000000, 0x070000)  slot1   (升级镜像暂存, 448KB)
 *     [0x070000, 0x0E0000)  scratch (SWAP_SCRATCH 交换区, 448KB)
 *     [0x0E0000, 0x0F0000)  storage (config A/B 双槽, 64KB;
 *                             Zephyr 版为 settings/FCB 分区)
 *     [0x0F0000, 0x1000000) littlefs (历史记录, ~15.06MB)
 */

#ifndef FLASH_LAYOUT_H
#define FLASH_LAYOUT_H

#include <stdint.h>

/* ---- 内部 flash ---- */
#define INT_FLASH_BASE   0x08000000u
#define INT_FLASH_SIZE   0x00080000u /* 512KB */

#define BOOT_OFFSET      0x00000000u
#define BOOT_SIZE        0x00010000u /* 64KB */
#define BOOT_ADDR        (INT_FLASH_BASE + BOOT_OFFSET)

#define SLOT0_OFFSET     0x00010000u
#define SLOT0_SIZE       0x00070000u /* 448KB */
#define SLOT0_ADDR       (INT_FLASH_BASE + SLOT0_OFFSET)
#define APP_ADDR         SLOT0_ADDR  /* app 链接基址 = slot0 */

/* ---- 外部 W25Q128 (io_flash 绝对偏移) ---- */
#define SLOT1_OFFSET     0x00000000u
#define SLOT1_SIZE       0x00070000u /* 448KB */

#define SCRATCH_OFFSET   0x00070000u
#define SCRATCH_SIZE     0x00070000u /* 448KB */

#define STORAGE_OFFSET   0x000E0000u
#define STORAGE_SIZE     0x00010000u /* 64KB */

#define LFS_OFFSET       0x000F0000u
#define LFS_SIZE         (0x01000000u - LFS_OFFSET)

#endif /* FLASH_LAYOUT_H */
