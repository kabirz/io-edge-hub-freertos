/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * W25Q128 (16 MiB) SPI NOR 驱动。target-only: HAL SPI 轮询模式。
 * 每个操作: CS 拉低 -> 命令 (+24 位大端地址) -> 数据 -> CS 拉高。
 * 写/擦前 WREN; 擦/写后轮询状态寄存器 busy 位 (带硬超时, 轮询中喂狗)。
 */

#include "w25qxx.h"
#include "spi.h"
#include "io_watchdog.h"
#include "main.h"

#define W25Q_CMD_JEDEC_ID  0x9Fu
#define W25Q_CMD_WRITE_EN  0x06u   /* WREN */
#define W25Q_CMD_WRITE_DI  0x04u   /* WRDI */
#define W25Q_CMD_PAGE_PROG 0x02u
#define W25Q_CMD_READ_DATA 0x03u
#define W25Q_CMD_READ_SR1  0x05u
#define W25Q_CMD_SECTOR_ER 0x20u   /* 4 KiB */
#define W25Q_CMD_BLOCK_ER32 0x52u  /* 32 KiB */
#define W25Q_CMD_BLOCK_ER64 0xD8u  /* 64 KiB */

#define W25Q_SR1_BUSY      0x01u

#define W25Q_CHIP_SIZE     0x1000000u   /* 16 MiB */
#define W25Q_PAGE_SIZE     256u
#define W25Q_SECTOR_SIZE   4096u

#define W25Q_JEDEC_ID      0xEF4018u    /* 厂商 EF (Winbond) + 0x4018 (128Mbit) */

/* 擦除硬超时 (ms): 手册 max 4K=400 / 32K=1600 / 64K=2000, 留 5-8x 裕量 */
#define W25Q_TMO_4K        2000u
#define W25Q_TMO_32K       8000u
#define W25Q_TMO_64K       16000u
#define W25Q_TMO_PROG      50u          /* tPP typ 0.4ms / max 3ms */
#define W25Q_TMO_XFER_MS   100u         /* 单次 SPI 传输 HAL 超时 */

static void cs_low(void)  { HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET); }
static void cs_high(void) { HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET); }

static int spi_tx(const uint8_t *d, uint16_t n)
{
    return HAL_SPI_Transmit(&hspi1, (uint8_t *)d, n, W25Q_TMO_XFER_MS) == HAL_OK ? 0 : -1;
}

static int spi_rx(uint8_t *d, uint16_t n)
{
    return HAL_SPI_Receive(&hspi1, d, n, W25Q_TMO_XFER_MS) == HAL_OK ? 0 : -1;
}

/* CS 内: 命令 + 24 位大端地址 */
static int spi_cmd_addr(uint8_t cmd, uint32_t addr)
{
    uint8_t b[4] = { cmd, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
    return spi_tx(b, 4);
}

static int wren(void)
{
    uint8_t cmd = W25Q_CMD_WRITE_EN;
    int r;
    cs_low();
    r = spi_tx(&cmd, 1);
    cs_high();
    return r;
}

/* 轮询状态寄存器 busy 位; 每 1ms 一次, 轮询中喂狗 (长擦除不能让 IWDG 复位) */
static int wait_not_busy(uint32_t timeout_ms)
{
    uint8_t cmd = W25Q_CMD_READ_SR1, sr = W25Q_SR1_BUSY;

    for (uint32_t t = 0; t <= timeout_ms; t++) {
        watchdog_feed();
        cs_low();
        if (spi_tx(&cmd, 1) != 0 || spi_rx(&sr, 1) != 0) { cs_high(); return -1; }
        cs_high();
        if ((sr & W25Q_SR1_BUSY) == 0) return 0;
        HAL_Delay(1);
    }
    return -1;
}

static int w25_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (len == 0) return 0;
    if (addr >= W25Q_CHIP_SIZE || W25Q_CHIP_SIZE - addr < len || buf == 0) return -1;
    while (len > 0) {
        uint32_t chunk = len > 0xFFFFu ? 0xFFFFu : len;   /* HAL 单次 <= 65535 */
        cs_low();
        if (spi_cmd_addr(W25Q_CMD_READ_DATA, addr) != 0) { cs_high(); return -1; }
        if (spi_rx(buf, (uint16_t)chunk) != 0)            { cs_high(); return -1; }
        cs_high();
        addr += chunk;
        buf += chunk;
        len -= chunk;
    }
    return 0;
}

static int w25_write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    if (len == 0) return 0;
    if (len > W25Q_PAGE_SIZE) return -1;                       /* io_flash 约束 */
    if ((addr % W25Q_PAGE_SIZE) + len > W25Q_PAGE_SIZE) return -1; /* 不跨页 */
    if (addr >= W25Q_CHIP_SIZE || W25Q_CHIP_SIZE - addr < len || buf == 0) return -1;
    if (wren() != 0) return -1;
    cs_low();
    if (spi_cmd_addr(W25Q_CMD_PAGE_PROG, addr) != 0 ||
        spi_tx(buf, (uint16_t)len) != 0) {
        cs_high();
        return -1;
    }
    cs_high();
    return wait_not_busy(W25Q_TMO_PROG);
}

static int erase_one(uint32_t addr, uint8_t cmd, uint32_t timeout_ms)
{
    if (wren() != 0) return -1;
    cs_low();
    if (spi_cmd_addr(cmd, addr) != 0) { cs_high(); return -1; }
    cs_high();
    return wait_not_busy(timeout_ms);
}

static int w25_erase(uint32_t addr, uint32_t len)
{
    if (len == 0 || addr % W25Q_SECTOR_SIZE != 0 || len % W25Q_SECTOR_SIZE != 0)
        return -1;
    if (addr >= W25Q_CHIP_SIZE || W25Q_CHIP_SIZE - addr < len) return -1;

    watchdog_feed();   /* 入口喂一次: littlefs 每块回调, 全片格式化分钟级 */
    while (len > 0) {
        uint32_t chunk, tmo;
        uint8_t cmd;
        if ((addr % 0x10000u) == 0 && len >= 0x10000u) {       /* 64 KiB 对齐 */
            chunk = 0x10000u; cmd = W25Q_CMD_BLOCK_ER64; tmo = W25Q_TMO_64K;
        } else if ((addr % 0x8000u) == 0 && len >= 0x8000u) {  /* 32 KiB 对齐 */
            chunk = 0x8000u;  cmd = W25Q_CMD_BLOCK_ER32; tmo = W25Q_TMO_32K;
        } else {                                               /* 4 KiB 兜底 */
            chunk = 0x1000u;  cmd = W25Q_CMD_SECTOR_ER; tmo = W25Q_TMO_4K;
        }
        if (erase_one(addr, cmd, tmo) != 0) return -1;
        addr += chunk;
        len -= chunk;
    }
    watchdog_feed();
    return 0;
}

static const struct io_flash w25_backend = { w25_read, w25_erase, w25_write };

int w25qxx_init(void)
{
    uint8_t cmd = W25Q_CMD_JEDEC_ID;
    uint8_t id[3] = { 0, 0, 0 };

    spi1_init();
    cs_low();
    if (spi_tx(&cmd, 1) != 0 || spi_rx(id, 3) != 0) { cs_high(); return -1; }
    cs_high();
    return ((uint32_t)id[0] << 16 | (uint32_t)id[1] << 8 | id[2]) == W25Q_JEDEC_ID
               ? 0 : -1;
}

const struct io_flash *w25qxx_flash(void)
{
    return &w25_backend;
}
