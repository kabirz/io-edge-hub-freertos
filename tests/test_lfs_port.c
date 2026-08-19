#include "test_util.h"
#include "fake_flash.h"
#include "config_store.h"
#include "lfs_port.h"

/*
 * Task 7: littlefs port layer (lfs_port.c), host tests on the RAM NOR fake.
 * deps/littlefs lfs.c + lfs_util.c are compiled straight into this target
 * (host-portable). The W25Qxx driver itself is target-only (HAL SPI) and is
 * not exercised here.
 *
 * Host build shrinks the littlefs partition to 256 KiB (LFS_PORT_TEST_SMALL)
 * so it fits inside the 1 MiB fake device; LFS_OFFSET stays the real one.
 */

/* watchdog fake: counts feeds. The Zephyr version feeds the watchdog around
 * mkfs and every erase; the port must keep that contract. */
static int wd_fed;
void watchdog_feed(void) { wd_fed++; }

static void fill_pattern(uint8_t *p, size_t n, unsigned seed)
{
    unsigned x = seed;
    for (size_t i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        p[i] = (uint8_t)(x >> 16);
    }
}

int main(void)
{
    lfs_t lfs;
    lfs_file_t f;
    struct lfs_info info;
    uint8_t wbuf[2048], rbuf[2048];

    /* ---- 1. fresh chip: mount -> auto-format -> create file, remount, read back ---- */
    fake_flash_reset();
    TEST_EQ_INT(lfs_port_mount(&lfs, fake_flash_get()), 0);

    /* 2 KiB file spans 8 NOR pages and exceeds the inline limit: exercises
     * the prog callback's 256-byte page splitting and block allocation */
    fill_pattern(wbuf, sizeof wbuf, 0xC0FFEEu);
    TEST_EQ_INT(lfs_file_open(&lfs, &f, "data.bin",
                              LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC), 0);
    TEST_EQ_INT(lfs_file_write(&lfs, &f, wbuf, sizeof wbuf), (int)sizeof wbuf);
    TEST_EQ_INT(lfs_file_close(&lfs, &f), 0);
    TEST_EQ_INT(lfs_unmount(&lfs), 0);

    /* clean remount must not erase anything (no watchdog feed from port) */
    wd_fed = 0;
    TEST_EQ_INT(lfs_port_mount(&lfs, fake_flash_get()), 0);
    TEST_EQ_INT(wd_fed, 0);
    TEST_EQ_INT(lfs_file_open(&lfs, &f, "data.bin", LFS_O_RDONLY), 0);
    TEST_EQ_INT(lfs_file_read(&lfs, &f, rbuf, sizeof rbuf), (int)sizeof wbuf);
    TEST_EQ_INT(lfs_file_read(&lfs, &f, rbuf, 1), 0);           /* EOF */
    TEST_EQ_MEM(rbuf, wbuf, sizeof wbuf);
    TEST_EQ_INT(lfs_file_close(&lfs, &f), 0);
    TEST_EQ_INT(lfs_unmount(&lfs), 0);

    /* ---- 2. dirty partition (power-loss sim): corrupt superblock metadata
     *          -> mount fails -> auto-format -> usable (data loss accepted) ---- */
    fake_flash_reset();
    TEST_EQ_INT(lfs_port_mount(&lfs, fake_flash_get()), 0);
    TEST_EQ_INT(lfs_file_open(&lfs, &f, "a.txt", LFS_O_WRONLY | LFS_O_CREAT), 0);
    TEST_EQ_INT(lfs_file_write(&lfs, &f, "AAA", 3), 3);
    TEST_EQ_INT(lfs_file_close(&lfs, &f), 0);
    /* no unmount: power cut. Flip metadata bytes (1->0 = torn-write debris)
     * inside BOTH superblock copies (littlefs blocks 0 and 1) */
    for (uint32_t off = 0x10; off <= 0x28; off += 3) {
        fake_flash_corrupt(LFS_OFFSET + 0u * 4096 + off);
        fake_flash_corrupt(LFS_OFFSET + 1u * 4096 + off);
    }
    TEST_EQ_INT(lfs_port_mount(&lfs, fake_flash_get()), 0);
    /* pre-corruption file is gone: proves a format happened, not a dirty mount */
    TEST_EQ_INT((int)lfs_stat(&lfs, "a.txt", &info), (int)LFS_ERR_NOENT);
    /* filesystem is usable after recovery */
    TEST_EQ_INT(lfs_file_open(&lfs, &f, "c.txt", LFS_O_WRONLY | LFS_O_CREAT), 0);
    TEST_EQ_INT(lfs_file_write(&lfs, &f, "CCC", 3), 3);
    TEST_EQ_INT(lfs_file_close(&lfs, &f), 0);
    TEST_EQ_INT(lfs_unmount(&lfs), 0);
    TEST_EQ_INT(lfs_port_mount(&lfs, fake_flash_get()), 0);
    TEST_EQ_INT(lfs_file_open(&lfs, &f, "c.txt", LFS_O_RDONLY), 0);
    TEST_EQ_INT(lfs_file_read(&lfs, &f, rbuf, 3), 3);
    TEST_EQ_MEM(rbuf, "CCC", 3);
    TEST_EQ_INT(lfs_file_close(&lfs, &f), 0);
    TEST_EQ_INT(lfs_unmount(&lfs), 0);

    /* ---- 3. erase feeds the watchdog (Zephyr parity: NOR erases can run
     *          for minutes during format; the port must feed while erasing) ---- */
    fake_flash_reset();
    wd_fed = 0;
    TEST_EQ_INT(lfs_port_mount(&lfs, fake_flash_get()), 0);
    TEST_ASSERT(wd_fed > 0);            /* format itself erases superblock pair */
    wd_fed = 0;                         /* now normal operation: new data block */
    TEST_EQ_INT(lfs_file_open(&lfs, &f, "w.bin", LFS_O_WRONLY | LFS_O_CREAT), 0);
    TEST_EQ_INT(lfs_file_write(&lfs, &f, wbuf, sizeof wbuf), (int)sizeof wbuf);
    TEST_EQ_INT(lfs_file_close(&lfs, &f), 0);
    TEST_ASSERT(wd_fed > 0);            /* every erase callback fed the dog */
    TEST_EQ_INT(lfs_unmount(&lfs), 0);

    TEST_MAIN_END();
}
