/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * flash 分区 ID 映射 (分区表见 src/include/flash_layout.h):
 *   slot0  -> 内部 flash 0x08010000 (448KB)
 *   slot1  -> W25Q128 0x000000 (448KB)
 *   scratch-> W25Q128 0x070000 (448KB)
 */

#ifndef __SYSFLASH_H__
#define __SYSFLASH_H__

#define PRIMARY_ID   0
#define SECONDARY_ID 1
#define SCRATCH_ID   2

#define FLASH_AREA_IMAGE_PRIMARY(x)   PRIMARY_ID
#define FLASH_AREA_IMAGE_SECONDARY(x) SECONDARY_ID
#define FLASH_AREA_IMAGE_SCRATCH      SCRATCH_ID

#endif /* __SYSFLASH_H__ */
