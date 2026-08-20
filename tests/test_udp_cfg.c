/*
 * test_udp_cfg.c - UDP 配置协议命令层 (src/net/udp_cfg.c, Zephyr 版
 * applications/io-edge-hub/src/udp.c 移植) 主机测试。
 * 测试文件内自带 io_hooks.h 全部假件 (记录调用值/次数/顺序), 时间钩子
 * udp_now_ms 绑可控计数器; 链接 udp_cfg.c + regmap.c + config_store.c +
 * fake_flash.c。
 *
 * 0x19 确认步契约: 命令层只置 udp_cfg_reboot_pending() 标志并返回
 * 应答 (不刷不重启); fake_transport_post_reply() 模拟 udp_task.c 传输层
 * "sendto 之后" 的动作 (查标志 -> history_sync + io_reboot_cold)。
 *
 * 版本经 tests/CMakeLists 注入: FW_VERSION_MAJOR=0 MINOR=3 PATCH=0。
 */
#include "test_util.h"
#include "init.h"
#include "io_hooks.h"
#include "io_bytes.h"
#include "config_store.h"
#include "fake_flash.h"
#include "udp_cfg.h"
#include <time.h>

/* ==================== io_hooks 假件 ==================== */

static int fake_sync_calls;
static int fake_reboots;
static int fake_seq; /* 全局调用序号, 校验副作用顺序 */
static int seq_sync, seq_reboot;
static time_t fake_ts_val;
static int fake_ts_calls;
static uint32_t fake_ms; /* 可控毫秒时钟 (绑到 udp_now_ms) */

static uint32_t fake_now_ms(void) { return fake_ms; }

void mb_set_do(uint16_t val)              { (void)val; }
void history_enable_write(bool en)        { (void)en; }
void history_sync(void)                   { fake_sync_calls++; seq_sync = ++fake_seq; }
void io_reboot_cold(void)                 { fake_reboots++; seq_reboot = ++fake_seq; }
uint32_t io_now_epoch(void)               { return 0; }
void io_lock(void) {}
void io_unlock(void) {}

/* set_timestamp 假件带真实范围门 (io_hooks.h: 946684800..4102444800,
 * 即 2000-01-01..2100-01-01): 协议层只透传返回值, 范围拒绝在钩子内 */
bool set_timestamp(time_t t)
{
    fake_ts_val = t;
    fake_ts_calls++;
    return t >= 946684800 && t <= 4102444800;
}

static void fakes_reset(void)
{
    fake_sync_calls = 0;
    fake_reboots = 0;
    fake_seq = seq_sync = seq_reboot = 0;
    fake_ts_val = 0;
    fake_ts_calls = 0;
    fake_ms = 0;
}

/* 模拟传输层契约 (src/net/udp_task.c): 应答 sendto 上线之后查
 * udp_cfg_reboot_pending(), 为真才 history_sync + io_reboot_cold
 * (真实 io_reboot_cold 不返回; 顺序证据经 seq_sync/seq_reboot 校验) */
static void fake_transport_post_reply(void)
{
    if (udp_cfg_reboot_pending()) {
        history_sync();
        io_reboot_cold();
    }
}

/* 假 flash + config_store 复位 (holding_reg 数组是静态状态, 用例按序书写) */
static void store_reset(void)
{
    fake_flash_reset();
    TEST_EQ_INT(config_store_init(fake_flash_get()), 0);
}

int main(void)
{
    uint8_t rep[64];
    uint16_t rn;
    struct io_cfg c;

    udp_now_ms = fake_now_ms;
    store_reset();
    fakes_reset();

    /* ---- 0. GET_IP 默认值 (regmap 编译默认 192.168.12.101) ---- */
    rn = udp_app_cmd(0x11, NULL, 0, rep, sizeof(rep));
    TEST_EQ_INT(rn, 5);
    TEST_EQ_INT(rep[0], 0x11);
    TEST_EQ_INT(rep[1], 192);
    TEST_EQ_INT(rep[2], 168);
    TEST_EQ_INT(rep[3], 12);
    TEST_EQ_INT(rep[4], 101);

    /* ---- 1. SET_IP 合法: [0x10][01], 写 reg 0x0A-0x0D + 持久化 ---- */
    {
        uint8_t ip[4] = {10, 20, 30, 40};

        rn = udp_app_cmd(0x10, ip, 4, rep, sizeof(rep));
        TEST_EQ_INT(rn, 2);
        TEST_EQ_INT(rep[0], 0x10);
        TEST_EQ_INT(rep[1], 0x01);
        TEST_EQ_INT(get_holding_reg(0x0A), 10);
        TEST_EQ_INT(get_holding_reg(0x0B), 20);
        TEST_EQ_INT(get_holding_reg(0x0C), 30);
        TEST_EQ_INT(get_holding_reg(0x0D), 40);
        config_store_init(fake_flash_get()); /* flash 重读验证已落盘 */
        config_store_get(&c);
        TEST_EQ_INT(c.ip[0], 10);
        TEST_EQ_INT(c.ip[1], 20);
        TEST_EQ_INT(c.ip[2], 30);
        TEST_EQ_INT(c.ip[3], 40);
    }

    /* ---- 2. SET_IP 非法: 总是应答 ok=0, 寄存器不动 ---- */
    {
        uint8_t bad_d0[4] = {192, 168, 12, 0};   /* 末字节 0 (网络地址) */
        uint8_t bad_dff[4] = {192, 168, 12, 255}; /* 末字节 0xFF (广播) */
        uint8_t bad_lo[4] = {127, 0, 0, 1};       /* 环回 */
        uint8_t bad_mc[4] = {224, 0, 0, 1};       /* 组播 D 段 */
        uint8_t bad_resv[4] = {240, 0, 0, 1};     /* 保留 E 段 */

        rn = udp_app_cmd(0x10, bad_d0, 4, rep, sizeof(rep));
        TEST_EQ_INT(rn, 2);
        TEST_EQ_INT(rep[0], 0x10);
        TEST_EQ_INT(rep[1], 0x00);
        rn = udp_app_cmd(0x10, bad_dff, 4, rep, sizeof(rep));
        TEST_EQ_INT(rn, 2);
        TEST_EQ_INT(rep[1], 0x00);
        rn = udp_app_cmd(0x10, bad_lo, 4, rep, sizeof(rep));
        TEST_EQ_INT(rep[1], 0x00);
        rn = udp_app_cmd(0x10, bad_mc, 4, rep, sizeof(rep));
        TEST_EQ_INT(rep[1], 0x00);
        rn = udp_app_cmd(0x10, bad_resv, 4, rep, sizeof(rep));
        TEST_EQ_INT(rep[1], 0x00);
        /* len<4 也总是应答 ok=0, 不触碰 data */
        rn = udp_app_cmd(0x10, bad_d0, 3, rep, sizeof(rep));
        TEST_EQ_INT(rn, 2);
        TEST_EQ_INT(rep[1], 0x00);
        rn = udp_app_cmd(0x10, NULL, 0, rep, sizeof(rep));
        TEST_EQ_INT(rn, 2);
        TEST_EQ_INT(rep[1], 0x00);
        TEST_EQ_INT(get_holding_reg(0x0A), 10); /* 寄存器保持 1 组的值 */
        TEST_EQ_INT(get_holding_reg(0x0D), 40);
    }

    /* ---- 3. GET_IP 回读 1 组写入的 IP ---- */
    rn = udp_app_cmd(0x11, NULL, 0, rep, sizeof(rep));
    TEST_EQ_INT(rn, 5);
    TEST_EQ_INT(rep[0], 0x11);
    TEST_EQ_INT(rep[1], 10);
    TEST_EQ_INT(rep[2], 20);
    TEST_EQ_INT(rep[3], 30);
    TEST_EQ_INT(rep[4], 40);

    /* ---- 4. SET_MODBUS 合法: [0x12][01], 写 reg 0x09/0x08 + 持久化 ---- */
    {
        uint8_t mb[3] = {5, 0x4B, 0x00}; /* slave 5, baud 19200 BE16 */

        rn = udp_app_cmd(0x12, mb, 3, rep, sizeof(rep));
        TEST_EQ_INT(rn, 2);
        TEST_EQ_INT(rep[0], 0x12);
        TEST_EQ_INT(rep[1], 0x01);
        TEST_EQ_INT(get_holding_reg(0x09), 5);
        TEST_EQ_INT(get_holding_reg(0x08), 19200);
        config_store_init(fake_flash_get());
        config_store_get(&c);
        TEST_EQ_INT(c.slave_id, 5);
        TEST_EQ_INT(c.rs485_bps, 19200);
    }

    /* ---- 5. SET_MODBUS len<3: [0x12][00], 寄存器不动 ---- */
    {
        uint8_t mb[2] = {7, 0x4B};

        rn = udp_app_cmd(0x12, mb, 2, rep, sizeof(rep));
        TEST_EQ_INT(rn, 2);
        TEST_EQ_INT(rep[0], 0x12);
        TEST_EQ_INT(rep[1], 0x00);
        TEST_EQ_INT(get_holding_reg(0x09), 5);
        TEST_EQ_INT(get_holding_reg(0x08), 19200);
    }

    /* ---- 6. GET_MODBUS: [0x13][slave][baud BE16] ---- */
    rn = udp_app_cmd(0x13, NULL, 0, rep, sizeof(rep));
    TEST_EQ_INT(rn, 4);
    TEST_EQ_INT(rep[0], 0x13);
    TEST_EQ_INT(rep[1], 5);
    TEST_EQ_INT(rep[2], 0x4B);
    TEST_EQ_INT(rep[3], 0x00);

    /* ---- 7. SET_TIME 合法: BE32 解码, ok 跟随钩子返回 ---- */
    {
        uint8_t t[4];

        io_put_be32(1787184000u, t); /* 2026-08-19 */
        rn = udp_app_cmd(0x14, t, 4, rep, sizeof(rep));
        TEST_EQ_INT(rn, 2);
        TEST_EQ_INT(rep[0], 0x14);
        TEST_EQ_INT(rep[1], 0x01);
        TEST_EQ_INT((unsigned long long)fake_ts_val, 1787184000ULL);
        TEST_EQ_INT(fake_ts_calls, 1);
    }

    /* ---- 8. SET_TIME 范围门: <2000 年 (946684800) 被钩子拒 -> [0x14][00] ---- */
    {
        uint8_t t[4];

        io_put_be32(946684799u, t); /* 1999-12-31 23:59:59 */
        rn = udp_app_cmd(0x14, t, 4, rep, sizeof(rep));
        TEST_EQ_INT(rn, 2);
        TEST_EQ_INT(rep[0], 0x14);
        TEST_EQ_INT(rep[1], 0x00);
        TEST_EQ_INT((unsigned long long)fake_ts_val, 946684799ULL);
        TEST_EQ_INT(fake_ts_calls, 2);

        /* len<4: ok=0 且不调 set_timestamp */
        rn = udp_app_cmd(0x14, t, 3, rep, sizeof(rep));
        TEST_EQ_INT(rn, 2);
        TEST_EQ_INT(rep[1], 0x00);
        TEST_EQ_INT(fake_ts_calls, 2);
    }

    /* ---- 9. FACTORY_RESET 两步确认 (5s 内): 首步只记时不应答破坏 ---- */
    udp_cfg_reset_pending();
    fakes_reset();
    update_holding_reg(0x03, 77); /* 非默认参数供擦除验证 */
    holding_reg_save();
    fake_ms = 10000;
    rn = udp_app_cmd(0x19, NULL, 0, rep, sizeof(rep));
    TEST_EQ_INT(rn, 2);
    TEST_EQ_INT(rep[0], 0x19);
    TEST_EQ_INT(rep[1], 0x00);
    TEST_ASSERT(udp_cfg_reboot_pending() == false);
    TEST_EQ_INT(fake_sync_calls, 0);
    TEST_EQ_INT(fake_reboots, 0);
    config_store_init(fake_flash_get());
    config_store_get(&c);
    TEST_EQ_INT(c.di_si, 77); /* 未擦除 */
    fake_transport_post_reply(); /* 标志未置: 传输层不动 */
    TEST_EQ_INT(fake_sync_calls, 0);
    TEST_EQ_INT(fake_reboots, 0);

    fake_ms = 13000; /* +3000ms <= 5000: 确认步 */
    rn = udp_app_cmd(0x19, NULL, 0, rep, sizeof(rep));
    TEST_EQ_INT(rn, 2);
    TEST_EQ_INT(rep[0], 0x19);
    TEST_EQ_INT(rep[1], 0x01);
    TEST_ASSERT(udp_cfg_reboot_pending() == true); /* 重启移交传输层 */
    TEST_EQ_INT(fake_sync_calls, 0); /* 命令层自身不刷不重启 */
    TEST_EQ_INT(fake_reboots, 0);
    config_store_init(fake_flash_get());
    config_store_get(&c);
    TEST_EQ_INT(c.di_si, 200); /* 已擦除回默认 (erase_all 生效) */
    /* 传输层: sendto 之后 sync -> reboot (顺序对齐 Zephyr) */
    fake_transport_post_reply();
    TEST_EQ_INT(fake_sync_calls, 1);
    TEST_EQ_INT(fake_reboots, 1);
    TEST_ASSERT(seq_sync < seq_reboot); /* 先刷历史后重启 */

    /* ---- 9b. 契约用例: 确认步 0x19 -> 应答返回 AND reboot_pending
     * 置位; 标志仅由 udp_cfg_reset_pending (重新上电) 复位 ---- */
    udp_cfg_reset_pending();
    fakes_reset();
    fake_ms = 100000;
    rn = udp_app_cmd(0x19, NULL, 0, rep, sizeof(rep));
    TEST_EQ_INT(rep[1], 0x00);
    TEST_ASSERT(udp_cfg_reboot_pending() == false);
    fake_ms = 101000; /* +1000ms: 确认步 */
    rn = udp_app_cmd(0x19, NULL, 0, rep, sizeof(rep));
    TEST_EQ_INT(rn, 2); /* 应答照常返回, 不被重启吞掉 */
    TEST_EQ_INT(rep[0], 0x19);
    TEST_EQ_INT(rep[1], 0x01);
    TEST_ASSERT(udp_cfg_reboot_pending() == true);
    udp_cfg_reset_pending(); /* 模拟重新上电清除待办 */
    TEST_ASSERT(udp_cfg_reboot_pending() == false);

    /* ---- 10. FACTORY_RESET 超 5s: 第二条只重记时; 第三条才确认 ---- */
    udp_cfg_reset_pending();
    fakes_reset();
    fake_ms = 20000;
    rn = udp_app_cmd(0x19, NULL, 0, rep, sizeof(rep));
    TEST_EQ_INT(rn, 2);
    TEST_EQ_INT(rep[1], 0x00);
    TEST_ASSERT(udp_cfg_reboot_pending() == false);
    fake_ms = 25001; /* +5001ms > 5000: 重新计时 */
    rn = udp_app_cmd(0x19, NULL, 0, rep, sizeof(rep));
    TEST_EQ_INT(rn, 2);
    TEST_EQ_INT(rep[1], 0x00);
    TEST_ASSERT(udp_cfg_reboot_pending() == false);
    TEST_EQ_INT(fake_reboots, 0);
    TEST_EQ_INT(fake_sync_calls, 0);
    fake_ms = 28000; /* +2999ms: 确认步 */
    rn = udp_app_cmd(0x19, NULL, 0, rep, sizeof(rep));
    TEST_EQ_INT(rn, 2);
    TEST_EQ_INT(rep[1], 0x01);
    TEST_ASSERT(udp_cfg_reboot_pending() == true);
    TEST_EQ_INT(fake_reboots, 0); /* 命令层不重启 */
    fake_transport_post_reply();
    TEST_EQ_INT(fake_reboots, 1);

    /* ---- 10b. 边界: 距首步恰好 5000ms (非 >5000) 即确认 ---- */
    udp_cfg_reset_pending();
    fakes_reset();
    fake_ms = 30000;
    rn = udp_app_cmd(0x19, NULL, 0, rep, sizeof(rep));
    TEST_EQ_INT(rep[1], 0x00);
    fake_ms = 35000; /* 恰好 +5000 */
    rn = udp_app_cmd(0x19, NULL, 0, rep, sizeof(rep));
    TEST_EQ_INT(rn, 2);
    TEST_EQ_INT(rep[1], 0x01);
    TEST_ASSERT(udp_cfg_reboot_pending() == true);
    fake_transport_post_reply();
    TEST_EQ_INT(fake_reboots, 1);

    /* ---- 11. 怪癖: 开机 5s 内首条命令即确认 (单命令立即执行) ---- */
    udp_cfg_reset_pending();
    fakes_reset();
    fake_ms = 4000; /* uptime 4s: (4000-0) 不 > 5000 -> 确认分支 */
    rn = udp_app_cmd(0x19, NULL, 0, rep, sizeof(rep));
    TEST_EQ_INT(rn, 2);
    TEST_EQ_INT(rep[0], 0x19);
    TEST_EQ_INT(rep[1], 0x01);
    TEST_EQ_INT(fake_sync_calls, 0); /* 命令层只置标志 */
    TEST_EQ_INT(fake_reboots, 0);
    TEST_ASSERT(udp_cfg_reboot_pending() == true);
    fake_transport_post_reply();
    TEST_EQ_INT(fake_sync_calls, 1);
    TEST_EQ_INT(fake_reboots, 1);

    /* 怪癖边界: uptime 恰 5000ms 仍单命令执行; 5001ms 起恢复两步 */
    udp_cfg_reset_pending();
    fakes_reset();
    fake_ms = 5000;
    rn = udp_app_cmd(0x19, NULL, 0, rep, sizeof(rep));
    TEST_EQ_INT(rn, 2);
    TEST_EQ_INT(rep[1], 0x01);
    TEST_ASSERT(udp_cfg_reboot_pending() == true);
    fake_transport_post_reply();
    TEST_EQ_INT(fake_reboots, 1);
    udp_cfg_reset_pending();
    fakes_reset();
    fake_ms = 5001;
    rn = udp_app_cmd(0x19, NULL, 0, rep, sizeof(rep));
    TEST_EQ_INT(rn, 2);
    TEST_EQ_INT(rep[1], 0x00);
    TEST_ASSERT(udp_cfg_reboot_pending() == false);
    TEST_EQ_INT(fake_reboots, 0);

    /* ---- 12. 未知命令静默 (含 0x01-0x06 固件升级, 一期无 MCUboot) ---- */
    {
        uint8_t d[5] = {1, 2, 3, 4, 5};

        TEST_EQ_INT(udp_app_cmd(0x01, d, 5, rep, sizeof(rep)), 0);
        TEST_EQ_INT(udp_app_cmd(0x06, d, 5, rep, sizeof(rep)), 0);
        TEST_EQ_INT(udp_app_cmd(0x00, NULL, 0, rep, sizeof(rep)), 0);
        TEST_EQ_INT(udp_app_cmd(0x0F, NULL, 0, rep, sizeof(rep)), 0);
        TEST_EQ_INT(udp_app_cmd(0x15, NULL, 0, rep, sizeof(rep)), 0);
        TEST_EQ_INT(udp_app_cmd(0x20, d, 5, rep, sizeof(rep)), 0);
        TEST_EQ_INT(udp_app_cmd(0xFF, NULL, 0, rep, sizeof(rep)), 0);
        /* 静默路径不产生任何副作用 */
        TEST_EQ_INT(fake_reboots, 0);
        TEST_EQ_INT(fake_sync_calls, 0);
        TEST_ASSERT(udp_cfg_reboot_pending() == false);
    }

    /* ---- 13. 跨网段白名单: 仅 GET_IP 0x11 ---- */
    TEST_ASSERT(udp_cmd_bcast_allowed(0x11) == true);
    TEST_ASSERT(udp_cmd_bcast_allowed(0x10) == false);
    TEST_ASSERT(udp_cmd_bcast_allowed(0x12) == false);
    TEST_ASSERT(udp_cmd_bcast_allowed(0x13) == false);
    TEST_ASSERT(udp_cmd_bcast_allowed(0x14) == false);
    TEST_ASSERT(udp_cmd_bcast_allowed(0x19) == false);
    TEST_ASSERT(udp_cmd_bcast_allowed(0x01) == false);
    TEST_ASSERT(udp_cmd_bcast_allowed(0x00) == false);
    TEST_ASSERT(udp_cmd_bcast_allowed(0xFF) == false);

    TEST_MAIN_END();
}
