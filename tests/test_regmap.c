/*
 * test_regmap.c - 寄存器模型 (src/modbus/regmap.c, Zephyr function.c 移植)
 * 主机测试。测试文件内自带 io_hooks.h 全部假件 (记录调用值/次数/顺序),
 * 链接 regmap.c + config_store.c + fake_flash.c。
 *
 * 版本通过 tests/CMakeLists 注入: FW_VERSION_MAJOR=0 MINOR=3 PATCH=0
 * => input_reg[0] == 0x0300。
 */
#include "test_util.h"
#include "init.h"
#include "io_hooks.h"
#include "config_store.h"
#include "fake_flash.h"
#include <time.h>

/* ==================== io_hooks 假件 ==================== */

static uint16_t fake_do_val;
static int fake_do_calls;
static bool fake_hist_en;
static int fake_hist_en_calls;
static int fake_sync_calls;
static time_t fake_ts_val;
static int fake_ts_calls;
static int fake_reboots;
static uint32_t fake_time;
static int fake_seq; /* 全局调用序号, 校验副作用顺序 */
static int seq_sync, seq_reboot;

void mb_set_do(uint16_t val)       { fake_do_val = val; fake_do_calls++; }
void history_enable_write(bool en) { fake_hist_en = en; fake_hist_en_calls++; }
void history_sync(void)            { fake_sync_calls++; seq_sync = ++fake_seq; }
bool set_timestamp(time_t t)        { fake_ts_val = t; fake_ts_calls++; return true; }
void io_reboot_cold(void)          { fake_reboots++; seq_reboot = ++fake_seq; }
uint32_t io_now_epoch(void)        { return fake_time; }
void io_lock(void) {}
void io_unlock(void) {}

static void fakes_reset(void)
{
    fake_do_val = 0; fake_do_calls = 0;
    fake_hist_en = false; fake_hist_en_calls = 0;
    fake_sync_calls = 0;
    fake_ts_val = 0; fake_ts_calls = 0;
    fake_reboots = 0;
    fake_time = 0;
    fake_seq = seq_sync = seq_reboot = 0;
}

/* 假 flash + config_store 复位 (寄存器数组是静态状态, 用例按序书写互不依赖
 * 其复位; 涉及持久化的用例先把寄存器摆到已知值) */
static void store_reset(void)
{
    fake_flash_reset();
    TEST_EQ_INT(config_store_init(fake_flash_get()), 0);
}

int main(void)
{
    bool st = false;
    struct io_cfg c;

    /* ---- 1. 默认值 (版本由 -DFW_VERSION_* 注入 0.3.0) ---- */
    TEST_EQ_INT(get_holding_reg(0x01), 0xFFFF);   /* DI 全使能 */
    TEST_EQ_INT(get_holding_reg(0x02), 0x000F);   /* AI 全使能 */
    TEST_EQ_INT(get_holding_reg(0x03), 200);
    TEST_EQ_INT(get_holding_reg(0x04), 200);
    TEST_EQ_INT(get_holding_reg(0x06), 0x0111);
    TEST_EQ_INT(get_holding_reg(0x07), 250);
    TEST_EQ_INT(get_holding_reg(0x08), 9600);
    TEST_EQ_INT(get_holding_reg(0x09), 1);
    TEST_EQ_INT(get_holding_reg(0x0A), 192);      /* 默认 192.168.12.101 */
    TEST_EQ_INT(get_holding_reg(0x0B), 168);
    TEST_EQ_INT(get_holding_reg(0x0C), 12);
    TEST_EQ_INT(get_holding_reg(0x0D), 101);
    TEST_EQ_INT(get_holding_reg(0x00), 0);        /* DO 默认全关 */
    TEST_EQ_INT(get_holding_reg(0x11), 0);
    TEST_EQ_INT(get_holding_reg(MODBUS_HOLDING_REGISTER_NUMBERS), 0); /* 越界 -> 0 */
    TEST_EQ_INT(get_input_reg(0), (0 << 12 | 3 << 8 | 0));
    TEST_EQ_INT(get_input_reg(MODBUS_INPUT_REGISTER_NUMBERS), 0);
    /* 越界写 -> -1 */
    TEST_EQ_INT(io_write_holding(18, 1), -1);
    TEST_EQ_INT(update_holding_reg(18, 1), -1);
    TEST_EQ_INT(update_input_reg(6, 1), -1);

    /* ---- 2. DO 写副作用: mb_set_do 被调 ---- */
    fakes_reset();
    TEST_EQ_INT(io_write_holding(0x00, 0x05), 0);
    TEST_EQ_INT(fake_do_val, 5);
    TEST_EQ_INT(fake_do_calls, 1);
    TEST_EQ_INT(get_holding_reg(0x00), 5);
    /* 同值写跳过全部副作用 */
    TEST_EQ_INT(io_write_holding(0x00, 0x05), 0);
    TEST_EQ_INT(fake_do_calls, 1);

    /* ---- 3. 同值写跳过: 0x11 默认已 0, 写 0 不触发重启 ---- */
    fakes_reset();
    TEST_EQ_INT(io_write_holding(0x11, 0), 0);
    TEST_EQ_INT(fake_reboots, 0);
    TEST_EQ_INT(fake_sync_calls, 0);

    /* ---- 4. 重启: 写 1 -> 先 history_sync 后 reboot, 寄存器回 0 ---- */
    TEST_EQ_INT(io_write_holding(0x11, 1), 0);
    TEST_EQ_INT(fake_reboots, 1);
    TEST_EQ_INT(fake_sync_calls, 1);
    TEST_ASSERT(seq_sync < seq_reboot);
    TEST_EQ_INT(get_holding_reg(0x11), 0);

    /* ---- 5. 0x10 参数保存: 持久化到 config_store, 寄存器回 0 ---- */
    fakes_reset();
    store_reset();
    TEST_EQ_INT(update_holding_reg(0x03, 77), 0); /* 无副作用改参 */
    TEST_EQ_INT(fake_do_calls, 0);
    TEST_EQ_INT(io_write_holding(0x10, 1), 0);
    TEST_EQ_INT(get_holding_reg(0x10), 0);
    config_store_init(fake_flash_get());          /* 从 flash 重读 */
    config_store_get(&c);
    TEST_EQ_INT(c.di_si, 77);

    /* ---- 6. 时间戳: 0x0E 写 hi、0x0F 写 lo -> set_timestamp(hi<<16|lo) ---- */
    fakes_reset();
    TEST_EQ_INT(io_write_holding(0x0E, 0x389D), 0); /* hi 段无副作用 */
    TEST_EQ_INT(fake_ts_calls, 0);
    TEST_EQ_INT(io_write_holding(0x0F, 0x2A80), 0);
    TEST_EQ_INT(fake_ts_calls, 1);
    TEST_EQ_INT((unsigned long long)fake_ts_val, 0x389D2A80ULL);
    TEST_EQ_INT(io_write_holding(0x0F, 0x2A80), 0); /* 同值跳过 */
    TEST_EQ_INT(fake_ts_calls, 1);

    /* ---- 7. 历史开关: 写 0x05 -> history_enable_write(reg != 0) ---- */
    fakes_reset();
    TEST_EQ_INT(io_write_holding(0x05, 1), 0);
    TEST_ASSERT(fake_hist_en == true);
    TEST_EQ_INT(fake_hist_en_calls, 1);
    TEST_EQ_INT(io_write_holding(0x05, 0), 0);
    TEST_ASSERT(fake_hist_en == false);

    /* ---- 8. 单 DO 位写 (FC05 语义, 读-改-写) ---- */
    fakes_reset();
    TEST_EQ_INT(update_holding_reg(0x00, 0), 0); /* 摆位: DO 清零且无副作用 */
    TEST_EQ_INT(fake_do_calls, 0);
    TEST_EQ_INT(io_write_do_bit(3, true), 0);
    TEST_EQ_INT(fake_do_val, 0x08);
    TEST_EQ_INT(get_holding_reg(0x00), 0x08);
    TEST_EQ_INT(io_write_do_bit(1, true), 0);
    TEST_EQ_INT(fake_do_val, 0x0A);
    TEST_EQ_INT(io_write_do_bit(3, false), 0);
    TEST_EQ_INT(fake_do_val, 0x02);
    TEST_EQ_INT(io_write_do_bit(9, true), -1);   /* bit >= DO_NUM */

    /* ---- 9. 实时时间读: 0x0E/0x0F 返回 io_now_epoch 高/低 16 位 ---- */
    fakes_reset();
    fake_time = 0x12345678u;
    TEST_EQ_INT(io_read_holding(0x0E), 0x1234);
    TEST_EQ_INT(io_read_holding(0x0F), 0x5678);
    update_holding_reg(0x0E, 0xDEAD);            /* 数组陈旧值不参与实时读 */
    TEST_EQ_INT(io_read_holding(0x0E), 0x1234);
    TEST_EQ_INT(io_read_holding(0x01), get_holding_reg(0x01)); /* 其余透传 */

    /* ---- 10. holding_reg_save/load 往返 + 非法 IP 导出跳过 ---- */
    store_reset();
    TEST_EQ_INT(update_holding_reg(0x09, 5), 0);
    TEST_EQ_INT(update_holding_reg(0x0A, 10), 0);
    TEST_EQ_INT(update_holding_reg(0x0B, 20), 0);
    TEST_EQ_INT(update_holding_reg(0x0C, 30), 0);
    TEST_EQ_INT(update_holding_reg(0x0D, 40), 0);
    holding_reg_save();
    /* 模拟重启: 重新 init config_store 后 load 恢复 */
    config_store_init(fake_flash_get());
    config_store_get(&c);
    TEST_EQ_INT(c.slave_id, 5);
    TEST_EQ_INT(c.ip[0], 10);
    TEST_EQ_INT(c.ip[1], 20);
    TEST_EQ_INT(c.ip[2], 30);
    TEST_EQ_INT(c.ip[3], 40);
    update_holding_reg(0x09, 9);                 /* 寄存器改乱后 load 恢复 */
    update_holding_reg(0x0A, 1);
    update_holding_reg(0x0B, 1);
    update_holding_reg(0x0C, 1);
    update_holding_reg(0x0D, 1);
    holding_reg_load();
    TEST_EQ_INT(get_holding_reg(0x09), 5);
    TEST_EQ_INT(get_holding_reg(0x0A), 10);
    TEST_EQ_INT(get_holding_reg(0x0B), 20);
    TEST_EQ_INT(get_holding_reg(0x0C), 30);
    TEST_EQ_INT(get_holding_reg(0x0D), 40);
    /* IP 非法 (0.0.0.1, 首段 0): save 导出跳过, 保留旧 cfg.ip */
    update_holding_reg(0x0A, 0);
    update_holding_reg(0x0B, 0);
    update_holding_reg(0x0C, 0);
    update_holding_reg(0x0D, 1);
    holding_reg_save();
    config_store_init(fake_flash_get());
    config_store_get(&c);
    TEST_EQ_INT(c.ip[0], 10);
    TEST_EQ_INT(c.ip[1], 20);
    TEST_EQ_INT(c.ip[2], 30);
    TEST_EQ_INT(c.ip[3], 40);

    /* ---- 11. coil (FC01) / discrete (FC02) 读 ---- */
    fakes_reset();
    TEST_EQ_INT(io_write_holding(0x00, 0x0A), 0); /* DO = 0b00001010 */
    TEST_EQ_INT(fake_do_val, 0x0A);
    TEST_EQ_INT(io_coil_rd(0, &st), 0);
    TEST_ASSERT(st == false);
    TEST_EQ_INT(io_coil_rd(1, &st), 0);
    TEST_ASSERT(st == true);
    TEST_EQ_INT(io_coil_rd(3, &st), 0);
    TEST_ASSERT(st == true);
    TEST_EQ_INT(io_coil_rd(7, &st), 0);
    TEST_ASSERT(st == false);
    TEST_EQ_INT(io_coil_rd(DO_NUM, &st), -1);
    /* discrete 从 input_reg[INPUT_DI_IDX] 取位 */
    TEST_EQ_INT(update_input_reg(INPUT_DI_IDX, 0x8001), 0);
    TEST_EQ_INT(io_discrete_rd(0, &st), 0);
    TEST_ASSERT(st == true);
    TEST_EQ_INT(io_discrete_rd(1, &st), 0);
    TEST_ASSERT(st == false);
    TEST_EQ_INT(io_discrete_rd(15, &st), 0);
    TEST_ASSERT(st == true);
    TEST_EQ_INT(io_discrete_rd(DI_NUM, &st), -1);

    TEST_MAIN_END();
}
