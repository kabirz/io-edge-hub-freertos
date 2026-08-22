/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * littlefs 移植层实现 (host 可测: 仅依赖 io_flash + littlefs + watchdog 原型)。
 */

#include "lfs_port.h"
#include "config_store.h"
#include "io_watchdog.h"
#include <stdint.h>

/* fw 构建的 heap_4 适配实现 (littlefs 源文件编译带
 * LFS_MALLOC=lfs_heap_alloc, 见 include/lfs_heap.h); host 测试不带
 * LFS_HEAP_FW, 本段不参与编译, 照常用系统 malloc */
#ifdef LFS_HEAP_FW
#include "FreeRTOS.h" /* pvPortMalloc / vPortFree (heap_4) */
#include "lfs_heap.h"

void *lfs_heap_alloc(size_t size)
{
    return pvPortMalloc(size);
}

void lfs_heap_free(void *p)
{
    if (p != NULL) {
        vPortFree(p);
    }
}
#endif

#define PORT_BLOCK_SIZE 4096u
#define PORT_READ_SIZE  16u
#define PORT_PROG_SIZE  16u
#define PORT_CACHE_SIZE 1024u
#define PORT_LOOKAHEAD  32u
#define PORT_CYCLES     512

#ifdef LFS_PORT_TEST_SMALL
/* host 假件只有 1 MiB: 分区缩到 256 KiB (offset 仍为真实 LFS_OFFSET) */
#define PORT_SIZE 0x40000u
#else
#define PORT_SIZE LFS_SIZE
#endif

#define PORT_BLOCKS (PORT_SIZE / PORT_BLOCK_SIZE)

static struct lfs_config port_cfg;                       /* 挂载期间须有效 */
static uint8_t port_read_buf[PORT_CACHE_SIZE];           /* 静态缓冲, 免 malloc */
static uint8_t port_prog_buf[PORT_CACHE_SIZE];
static uint8_t port_lookahead_buf[PORT_LOOKAHEAD];

static uint32_t block_addr(lfs_block_t block, lfs_off_t off)
{
    return LFS_OFFSET + (uint32_t)block * PORT_BLOCK_SIZE + off;
}

/* NOR 页 256B: littlefs 的 prog 只保证 prog_size 对齐, 这里按页切分,
 * 满足 io_flash "len<=256 且不跨页" 的调用约束 */
static int lf_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off,
                   const void *buffer, lfs_size_t size)
{
    const struct io_flash *f = c->context;
    const uint8_t *p = buffer;
    uint32_t addr;

    if (block >= PORT_BLOCKS || off > PORT_BLOCK_SIZE || size > PORT_BLOCK_SIZE - off)
        return LFS_ERR_IO;
    addr = block_addr(block, off);
    while (size > 0) {
        uint32_t chunk = 256u - (addr % 256u);
        if (chunk > size) chunk = size;
        if (f->write(addr, p, chunk) != 0) return LFS_ERR_IO;
        addr += chunk;
        p += chunk;
        size -= chunk;
    }
    return 0;
}

static int lf_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off,
                   void *buffer, lfs_size_t size)
{
    const struct io_flash *f = c->context;

    if (block >= PORT_BLOCKS || off > PORT_BLOCK_SIZE || size > PORT_BLOCK_SIZE - off)
        return LFS_ERR_IO;
    return f->read(block_addr(block, off), buffer, size) == 0 ? 0 : LFS_ERR_IO;
}

static int lf_erase(const struct lfs_config *c, lfs_block_t block)
{
    const struct io_flash *f = c->context;

    /* 每次擦除喂狗: 格式化全分区在真 NOR 上可达分钟级, 不能让 IWDG 中途复位
     * 造成文件系统半损坏 */
    watchdog_feed();
    if (block >= PORT_BLOCKS) return LFS_ERR_IO;
    return f->erase(block_addr(block, 0), PORT_BLOCK_SIZE) == 0 ? 0 : LFS_ERR_IO;
}

static int lf_sync(const struct lfs_config *c)
{
    (void)c;    /* io_flash 写入即落芯片, 无缓存需刷 */
    return 0;
}

int lfs_port_mount(lfs_t *lfs, const struct io_flash *flash)
{
    int rc;

    if (lfs == 0 || flash == 0) return -1;

    port_cfg.context = (void *)flash;
    port_cfg.read = lf_read;
    port_cfg.prog = lf_prog;
    port_cfg.erase = lf_erase;
    port_cfg.sync = lf_sync;
    port_cfg.read_size = PORT_READ_SIZE;
    port_cfg.prog_size = PORT_PROG_SIZE;
    port_cfg.block_size = PORT_BLOCK_SIZE;
    port_cfg.block_count = PORT_BLOCKS;
    port_cfg.block_cycles = PORT_CYCLES;
    port_cfg.cache_size = PORT_CACHE_SIZE;
    port_cfg.lookahead_size = PORT_LOOKAHEAD;
    port_cfg.read_buffer = port_read_buf;
    port_cfg.prog_buffer = port_prog_buf;
    port_cfg.lookahead_buffer = port_lookahead_buf;

    rc = lfs_mount(lfs, &port_cfg);
    if (rc != 0) {
        /* 任何挂载失败都格式化一次: 脏分区/半擦除分区返回的不只是
         * ENODEV, 逐个判断会漏场景 */
        watchdog_feed();
        rc = lfs_format(lfs, &port_cfg);
        watchdog_feed();
        if (rc == 0)
            rc = lfs_mount(lfs, &port_cfg);
    }
    return rc == 0 ? 0 : -1;
}
