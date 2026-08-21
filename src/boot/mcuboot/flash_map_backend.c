/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * flash_map_backend 实现: boot 域 (bootutil) 与 app 域 (boot_request_
 * upgrade 写 slot1 trailer) 共用。
 *
 * 两个 flash 设备:
 *   device 0 = STM32F407 片内 flash (fa_off 为绝对地址, 内存映射读 +
 *              intflash 擦写; 扇区非均匀 64K/128K)
 *   device 1 = W25Q128 SPI NOR (fa_off 为片内偏移; 4KB 均匀扇区,
 *              写入按 256B 页拆分)
 */

#include <string.h>

#include "flash_map_backend/flash_map_backend.h"
#include "sysflash/sysflash.h"

#include "flash_layout.h"
#include "intflash.h"
#include "io_flash.h"
#include "w25qxx.h"

/* slot0 起始 (绝对地址); device 1 各区起始 = 片内偏移 */
#define AREA_PRIMARY_OFF  SLOT0_ADDR
#define AREA_SECOND_OFF   SLOT1_OFFSET
#define AREA_SCRATCH_OFF  SCRATCH_OFFSET

static const struct flash_area areas[] = {
    [PRIMARY_ID] = {
        .fa_id = PRIMARY_ID,
        .fa_device_id = 0,
        .fa_off = AREA_PRIMARY_OFF,
        .fa_size = SLOT0_SIZE,
    },
    [SECONDARY_ID] = {
        .fa_id = SECONDARY_ID,
        .fa_device_id = 1,
        .fa_off = AREA_SECOND_OFF,
        .fa_size = SLOT1_SIZE,
    },
    [SCRATCH_ID] = {
        .fa_id = SCRATCH_ID,
        .fa_device_id = 1,
        .fa_off = AREA_SCRATCH_OFF,
        .fa_size = SCRATCH_SIZE,
    },
};

#define AREA_N (sizeof(areas) / sizeof(areas[0]))

int flash_area_open(uint8_t id, const struct flash_area **fapp)
{
    if (id >= AREA_N || fapp == NULL) {
        return -1;
    }
    *fapp = &areas[id];
    return 0;
}

void flash_area_close(const struct flash_area *fap)
{
    (void)fap; /* 静态区域表, 无需关闭 */
}

int flash_area_read(const struct flash_area *fap, uint32_t off, void *dst,
                    uint32_t len)
{
    if (off + len > fap->fa_size) {
        return -1;
    }
    if (fap->fa_device_id == 0) {
        memcpy(dst, (const void *)(fap->fa_off + off), len);
        return 0;
    }
    return w25qxx_flash()->read(fap->fa_off + off, dst, len);
}

int flash_area_write(const struct flash_area *fap, uint32_t off,
                     const void *src, uint32_t len)
{
    const uint8_t *p = src;
    uint32_t addr;

    if (off + len > fap->fa_size) {
        return -1;
    }
    addr = fap->fa_off + off;

    if (fap->fa_device_id == 0) {
        return intflash_write(addr, p, len);
    }
    /* W25Q: io_flash 约束单次 <=256B 且不跨页, 按页拆分 */
    while (len > 0) {
        uint32_t chunk = 256u - (addr % 256u);

        if (chunk > len) {
            chunk = len;
        }
        if (w25qxx_flash()->write(addr, p, chunk) != 0) {
            return -1;
        }
        addr += chunk;
        p += chunk;
        len -= chunk;
    }
    return 0;
}

int flash_area_erase(const struct flash_area *fap, uint32_t off, uint32_t len)
{
    if (off + len > fap->fa_size) {
        return -1;
    }
    if (fap->fa_device_id == 0) {
        return intflash_erase(fap->fa_off + off, len);
    }
    return w25qxx_flash()->erase(fap->fa_off + off, len);
}

uint32_t flash_area_align(const struct flash_area *fap)
{
    /* 片内按字节编程, NOR 按字节页编程: 最小写粒度 1
     * (trailer 尺寸由 BOOT_MAX_ALIGN=8 兜底) */
    (void)fap;
    return 1;
}

uint8_t flash_area_erased_val(const struct flash_area *fap)
{
    (void)fap;
    return 0xFFu;
}

int flash_area_get_sectors(int fa_id, uint32_t *count,
                           struct flash_sector *sectors)
{
    uint32_t i, n, total = 0;

    if (fa_id < 0 || (uint32_t)fa_id >= AREA_N || sectors == NULL) {
        return -1;
    }

    if (fa_id == PRIMARY_ID) {
        /* slot0 跨内部扇区 4 (64KB) + 5..7 (128KB x3) = 8 个 */
        static const uint32_t sz[] = {0x10000u, 0x20000u, 0x20000u, 0x20000u};
        uint32_t off = 0;

        n = sizeof(sz) / sizeof(sz[0]);
        for (i = 0; i < n; i++) {
            sectors[i].fs_off = off;
            sectors[i].fs_size = sz[i];
            off += sz[i];
        }
        total = off;
    } else {
        /* 外部 NOR 4KB 均匀扇区 */
        n = areas[fa_id].fa_size / 4096u;
        for (i = 0; i < n; i++) {
            sectors[i].fs_off = i * 4096u;
            sectors[i].fs_size = 4096u;
        }
        total = areas[fa_id].fa_size;
    }

    (void)total;
    *count = n;
    return 0;
}

int flash_area_id_from_image_slot(int slot)
{
    return slot == 0 ? PRIMARY_ID : SECONDARY_ID;
}

int flash_area_id_from_multi_image_slot(int image_index, int slot)
{
    (void)image_index; /* 单镜像 */
    return flash_area_id_from_image_slot(slot);
}

int flash_area_id_to_multi_image_slot(int image_index, int area_id)
{
    (void)image_index;
    switch (area_id) {
    case PRIMARY_ID:
        return 0;
    case SECONDARY_ID:
        return 1;
    default:
        return -1;
    }
}
