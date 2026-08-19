#ifndef FAKE_FLASH_H
#define FAKE_FLASH_H

#include "io_flash.h"

/*
 * RAM-backed NOR flash fake for host tests. Maps NOR offset
 * 0x100000..0x1FFFFF (config slots A/B + reduced littlefs partition)
 * onto an internal 1 MiB array. Enforces NOR semantics: erase sets
 * 0xFF, writes only clear bits (1 -> 0).
 */
void fake_flash_reset(void);                /* whole fake device to 0xFF */
void fake_flash_corrupt(uint32_t addr);     /* flip one byte */
const struct io_flash *fake_flash_get(void);

#endif /* FAKE_FLASH_H */
