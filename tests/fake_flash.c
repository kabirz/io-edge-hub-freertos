#include "fake_flash.h"
#include <string.h>

#define FAKE_BASE   0x000000u   /* 全片 16 MiB: slot1 (0x0) + storage + littlefs */
#define FAKE_SIZE   0x1000000u
#define NOR_PAGE    256u
#define NOR_SECTOR  4096u

/* 16 MiB RAM fake (host 内存充裕; slot1 升级测试需覆盖 0x0 起) */
static uint8_t mem[FAKE_SIZE];

static int in_range(uint32_t addr, uint32_t len)
{
    return addr >= FAKE_BASE && addr - FAKE_BASE <= FAKE_SIZE - len && len <= FAKE_SIZE;
}

static int fake_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (!in_range(addr, len)) return -1;
    memcpy(buf, &mem[addr - FAKE_BASE], len);
    return 0;
}

static int fake_erase(uint32_t addr, uint32_t len)
{
    if (len == 0 || len % NOR_SECTOR != 0 || addr % NOR_SECTOR != 0) return -1;
    if (!in_range(addr, len)) return -1;
    memset(&mem[addr - FAKE_BASE], 0xFF, len);
    return 0;
}

static int fake_write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    if (len == 0 || len > NOR_PAGE) return -1;
    if ((addr % NOR_PAGE) + len > NOR_PAGE) return -1;   /* page crossing */
    if (!in_range(addr, len)) return -1;
    uint8_t *dst = &mem[addr - FAKE_BASE];
    for (uint32_t i = 0; i < len; i++) {
        if ((dst[i] & buf[i]) != buf[i]) return -1;      /* NOR: bits only go 1 -> 0 */
    }
    memcpy(dst, buf, len);
    return 0;
}

static const struct io_flash fake = { fake_read, fake_erase, fake_write };

void fake_flash_reset(void)
{
    memset(mem, 0xFF, sizeof mem);
}

void fake_flash_corrupt(uint32_t addr)
{
    if (in_range(addr, 1)) mem[addr - FAKE_BASE] ^= 0xFF;
}

const struct io_flash *fake_flash_get(void)
{
    return &fake;
}
