#include "test_util.h"
#include "fake_flash.h"
#include "config_store.h"
#include "lfs_port.h"
#include "init.h"
#include "history_file.h"

#include <time.h>

/*
 * Task 8: history recorder pure file core (history_file.c), host tests on the
 * RAM NOR fake via lfs_port + deps/littlefs. The FreeRTOS shell (history.c:
 * queue/task/mutex) is target-only and not exercised here.
 *
 * Locked parity facts (Zephyr src/history/history.c):
 *  - file name data_MMDD_HHMMSS.raw at lfs root, epoch+8h -> gmtime
 *  - DI record 10 bytes, AI record 16 bytes (packed struct, written by type)
 *  - rotation when the current file reaches HIST_FILE_MAX (test override 4096)
 *  - keep at most HIST_MAX_FILES (10) data_* files, drop lexicographically
 *    smallest, only checked when a brand-new file is created at size 0
 *  - close keeps his_cur_name: next write appends the same file (no new file)
 *  - boot (init) resume: append the lexicographically largest data_* file if
 *    still < HIST_FILE_MAX, else create a new one
 */

/* watchdog stub: lfs_port feeds it on every erase */
static int wd_fed;
void watchdog_feed(void) { wd_fed++; }

/* time injection: hist_file_set_clock is host-test-only */
static time_t fake_now;
static time_t fake_clock(void) { return fake_now; }

/*
 * 1767294245 = 2026-01-01 19:04:05 UTC -> +8h = 2026-01-02 03:04:05
 * file name data_0102_030405.raw (verified with gmtime)
 */
#define T1 1767294245L
#define NAME_T1 "data_0102_030405.raw"
/* +10s later clock: rotation must open data_0102_030415.raw */
#define T2 1767294255L
#define NAME_T2 "data_0102_030415.raw"

static lfs_t lfs;

static void fresh_mount(void)
{
    fake_flash_reset();
    TEST_EQ_INT(lfs_port_mount(&lfs, fake_flash_get()), 0);
    hist_file_set_clock(fake_clock);
    hist_file_init(&lfs);
}

/* count regular data_* files at the lfs root */
static int count_data_files(void)
{
    lfs_dir_t dir;
    struct lfs_info info;
    int n = 0;

    TEST_EQ_INT(lfs_dir_open(&lfs, &dir, "/"), 0);
    while (lfs_dir_read(&lfs, &dir, &info) > 0) {
        if (info.type == LFS_TYPE_REG && strncmp(info.name, "data_", 5) == 0)
            n++;
    }
    lfs_dir_close(&lfs, &dir);
    return n;
}

/* size of a root file, or -1 if missing */
static int file_size(const char *name)
{
    struct lfs_info info;

    if (lfs_stat(&lfs, name, &info) != 0)
        return -1;
    return (int)info.size;
}

/* pre-seed a root file of `size` bytes (0xAA fill) directly via littlefs */
static void make_file(const char *name, lfs_size_t size)
{
    lfs_file_t f;
    uint8_t b = 0xAA;

    TEST_EQ_INT(lfs_file_open(&lfs, &f, name, LFS_O_WRONLY | LFS_O_CREAT), 0);
    for (lfs_size_t i = 0; i < size; i++)
        TEST_EQ_INT(lfs_file_write(&lfs, &f, &b, 1), 1);
    TEST_EQ_INT(lfs_file_close(&lfs, &f), 0);
}

static void read_file(const char *name, uint8_t *buf, lfs_size_t len)
{
    lfs_file_t f;

    TEST_EQ_INT(lfs_file_open(&lfs, &f, name, LFS_O_RDONLY), 0);
    TEST_EQ_INT(lfs_file_read(&lfs, &f, buf, len), (int)len);
    TEST_EQ_INT(lfs_file_read(&lfs, &f, buf, 1), 0); /* exactly len bytes */
    TEST_EQ_INT(lfs_file_close(&lfs, &f), 0);
}

static void write_di(uint32_t ts, uint16_t en, uint16_t val)
{
    struct his_data d;

    memset(&d, 0, sizeof d);
    d.type = DI_TYPE;
    d.timestamps = ts;
    d.di.di_en_status = en;
    d.di.di_value = val;
    TEST_EQ_INT(hist_file_write(&d), 0);
}

int main(void)
{
    /* ---- 1. one DI record: 10 bytes, byte-exact little-endian layout ---- */
    {
        uint8_t expect[10] = {
            0x01, 0x00,                         /* type = DI_TYPE LE */
            0x44, 0x33, 0x22, 0x11,             /* timestamps 0x11223344 LE */
            0xAA, 0x55,                         /* di_en_status 0x55AA LE */
            0x33, 0xCC,                         /* di_value 0xCC33 LE */
        };
        uint8_t got[10];

        fresh_mount();
        fake_now = T1;
        write_di(0x11223344u, 0x55AAu, 0xCC33u);
        /* littlefs 缓冲经 sync 提交后外部句柄才可见 (Zephyr FTP 读文件前
         * 先 history_sync() 的同一模式) */
        TEST_EQ_INT(hist_file_sync(), 0);
        TEST_EQ_INT(count_data_files(), 1);
        TEST_EQ_INT(file_size(NAME_T1), 10);
        read_file(NAME_T1, got, sizeof got);
        TEST_EQ_MEM(got, expect, sizeof got);
    }

    /* ---- 2. rotation at HIST_FILE_MAX (4096): new file, old kept ---- */
    {
        fresh_mount();
        fake_now = T1;
        /* 410 DI records x 10 B = 4100 B: record #410 crosses 4096 */
        for (int i = 0; i < 410; i++)
            write_di((uint32_t)i, 0, 0);
        hist_file_sync();
        TEST_EQ_INT(count_data_files(), 1);
        TEST_EQ_INT(file_size(NAME_T1), 4100);

        /* next write must rotate to a fresh file named after the new clock */
        fake_now = T2;
        write_di(1, 2, 3);
        hist_file_sync();
        TEST_EQ_INT(count_data_files(), 2);
        TEST_EQ_INT(file_size(NAME_T1), 4100); /* old file untouched */
        TEST_EQ_INT(file_size(NAME_T2), 10);
    }

    /* ---- 3. keep-10 cleanup: new file drops the oldest data_* files ---- */
    {
        fresh_mount();
        for (int i = 1; i <= 12; i++) {
            char name[24];

            snprintf(name, sizeof name, "data_0101_0000%02d.raw", i);
            make_file(name, (i == 12) ? 4096 : 0); /* only the largest is full */
        }
        TEST_EQ_INT(count_data_files(), 12);

        fake_now = T1; /* new file name > all data_0101_* names */
        write_di(7, 8, 9);
        hist_file_sync();
        /* cleanup trims the 12 committed files to 10 (removes the 2
         * lexicographically smallest); the brand-new file itself is not
         * yet committed when the scan runs (littlefs lazy dirent commit,
         * same underneath Zephyr) -> 11 files on disk afterwards */
        TEST_EQ_INT(count_data_files(), 11);                 /* 12 - 2 + 1 */
        TEST_EQ_INT(file_size("data_0101_000001.raw"), -1);  /* dropped */
        TEST_EQ_INT(file_size("data_0101_000002.raw"), -1);  /* dropped */
        TEST_EQ_INT(file_size("data_0101_000003.raw"), 0);   /* kept */
        TEST_EQ_INT(file_size("data_0101_000004.raw"), 0);   /* kept */
        TEST_EQ_INT(file_size("data_0101_000012.raw"), 4096);/* kept, still full */
        TEST_EQ_INT(file_size(NAME_T1), 10);                 /* the new file */
    }

    /* ---- 4. close -> next write appends the SAME file (no new file) ---- */
    {
        fresh_mount();
        fake_now = T1;
        write_di(1, 1, 1);
        hist_file_close();
        write_di(2, 2, 2);
        hist_file_sync();
        TEST_EQ_INT(count_data_files(), 1);          /* no second file */
        TEST_EQ_INT(file_size(NAME_T1), 20);         /* appended */
    }

    /* ---- 5. re-init (reboot sim): resume largest data_* file < max ---- */
    {
        fresh_mount();
        make_file("data_0101_000001.raw", 10);
        make_file("data_0101_000002.raw", 20); /* lexicographically largest */

        fake_now = T1;
        write_di(3, 3, 3);
        hist_file_sync();
        TEST_EQ_INT(count_data_files(), 2);              /* no new file */
        TEST_EQ_INT(file_size("data_0101_000002.raw"), 30); /* 20 + 10 */
        TEST_EQ_INT(file_size(NAME_T1), -1);             /* not created */
        TEST_EQ_INT(file_size("data_0101_000001.raw"), 10);
    }

    /* ---- 6. AI record: 16-byte layout ---- */
    {
        struct his_data d;
        uint8_t expect[16] = {
            0x02, 0x00,                         /* type = AI_TYPE LE */
            0xAA, 0x99, 0x88, 0x77,             /* timestamps 0x778899AA LE */
            0xEF, 0xBE,                         /* ai_en_status 0xBEEF LE */
            0x02, 0x01, 0x04, 0x03,             /* ai_value[0..1] LE */
            0x06, 0x05, 0x08, 0x07,             /* ai_value[2..3] LE */
        };
        uint8_t got[16];

        fresh_mount();
        fake_now = T1;
        memset(&d, 0, sizeof d);
        d.type = AI_TYPE;
        d.timestamps = 0x778899AAu;
        d.ai.ai_en_status = 0xBEEF;
        d.ai.ai_value[0] = 0x0102;
        d.ai.ai_value[1] = 0x0304;
        d.ai.ai_value[2] = 0x0506;
        d.ai.ai_value[3] = 0x0708;
        TEST_EQ_INT(hist_file_write(&d), 0);
        hist_file_sync();
        TEST_EQ_INT(count_data_files(), 1);
        TEST_EQ_INT(file_size(NAME_T1), 16);
        read_file(NAME_T1, got, sizeof got);
        TEST_EQ_MEM(got, expect, sizeof got);
    }

    TEST_MAIN_END();
}
