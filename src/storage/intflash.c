/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * STM32F407 片内 flash 擦写实现 (HAL_FLASHEx_Erase / HAL_FLASH_Program)。
 * 长擦除期间逐扇区喂狗 (128KB 扇区擦除可达 ~2s, IWDG 30s 窗口需跨多扇区)。
 */

#include <stddef.h>

#include "main.h"

#ifdef BOOT_DOMAIN
#include "boot_uart.h"
#define INTFLASH_LOG(fmt, ...) boot_log(fmt, ##__VA_ARGS__)
#else
#include "log.h"
#define INTFLASH_LOG(fmt, ...) LOG_ERR(fmt, ##__VA_ARGS__)
#endif

#include "intflash.h"
#include "io_watchdog.h"

static const struct {
    uint32_t addr;
    uint32_t size;
    uint32_t sector; /* FLASH_SECTOR_x */
} f4_sector_tab[] = {
    {0x08000000u, 0x4000u, FLASH_SECTOR_0},
    {0x08004000u, 0x4000u, FLASH_SECTOR_1},
    {0x08008000u, 0x4000u, FLASH_SECTOR_2},
    {0x0800C000u, 0x4000u, FLASH_SECTOR_3},
    {0x08010000u, 0x10000u, FLASH_SECTOR_4},
    {0x08020000u, 0x20000u, FLASH_SECTOR_5},
    {0x08040000u, 0x20000u, FLASH_SECTOR_6},
    {0x08060000u, 0x20000u, FLASH_SECTOR_7},
    /* 512KB (VET6) 到 sector 7 为止; 8-11 为 1MB 型号 */
};

#define F4_SECTOR_N (sizeof(f4_sector_tab) / sizeof(f4_sector_tab[0]))

int intflash_erase(uint32_t addr, uint32_t len)
{
    uint32_t end;

    if (len == 0) {
        return 0;
    }
    len = (len + 0xFFFu) & ~0xFFFu; /* 调用方按镜像大小传入, 4KB 对齐 */
    end = addr + len;
    if (addr < f4_sector_tab[0].addr || end > 0x08080000u) {
        INTFLASH_LOG("intflash: erase range %08x +%x out of bounds",
                     (unsigned)addr, (unsigned)len);
        return -1;
    }

    if (HAL_FLASH_Unlock() != HAL_OK) {
        INTFLASH_LOG("intflash: HAL_FLASH_Unlock failed");
        return -1;
    }
    for (uint32_t i = 0; i < F4_SECTOR_N; i++) {
        const uint32_t s_addr = f4_sector_tab[i].addr;
        const uint32_t s_end = s_addr + f4_sector_tab[i].size;

        if (s_end <= addr || s_addr >= end) {
            continue; /* 无交集 */
        }
        FLASH_EraseInitTypeDef e = {
            .TypeErase = FLASH_TYPEERASE_SECTORS,
            .Banks = FLASH_BANK_1,
            .Sector = f4_sector_tab[i].sector,
            .NbSectors = 1,
            .VoltageRange = FLASH_VOLTAGE_RANGE_3, /* 2.7-3.6V, x32 并行 */
        };
        uint32_t err = 0;
        HAL_StatusTypeDef r;

        watchdog_feed();
        r = HAL_FLASHEx_Erase(&e, &err);
        if (r != HAL_OK) {
            /* SectorError 仅在失败时有意义 (成功哨兵 0xFFFFFFFF) */
            INTFLASH_LOG("intflash: erase sec%u %08x hal=%d err=%x sr=%x",
                         (unsigned)f4_sector_tab[i].sector,
                         (unsigned)s_addr, (int)r, (unsigned)err,
                         (unsigned)(FLASH->SR & 0xF3F0u));
            (void)HAL_FLASH_Lock();
            return -1;
        }
    }
    return HAL_FLASH_Lock() == HAL_OK ? 0 : -1;
}

int intflash_write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    if (buf == NULL && len > 0) {
        return -1;
    }

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return -1;
    }
    for (uint32_t off = 0; off < len;) {
        HAL_StatusTypeDef r;

        if (((addr + off) & 3u) == 0 && len - off >= 4) {
            uint32_t w = (uint32_t)buf[off] | (uint32_t)buf[off + 1] << 8 |
                         (uint32_t)buf[off + 2] << 16 |
                         (uint32_t)buf[off + 3] << 24;
            r = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + off, w);
            off += 4;
        } else {
            r = HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, addr + off,
                                  buf[off]);
            off += 1;
        }
        if (r != HAL_OK) {
            (void)HAL_FLASH_Lock();
            return -1;
        }
        if ((off & 0xFFu) == 0) {
            watchdog_feed();
        }
    }
    return HAL_FLASH_Lock() == HAL_OK ? 0 : -1;
}
