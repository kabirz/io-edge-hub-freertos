/*
 * test_mb_server.c - Modbus PDU 解码器 (src/modbus/mb_server.c) 主机测试。
 * 链接 mb_server.c + regmap.c + config_store.c + fake_flash.c,
 * 测试文件内自带 io_hooks.h 全部假件 (test_regmap.c 同款)。
 *
 * 语义基准: zephyr/subsys/modbus/modbus_server.c (逐分支) +
 * io-edge-hub 的 io_modbus_cbs 回调 (地址范围检查 -> 0x02)。
 * in/out 全部以字节数组断言; 期望值里的寄存器数据来自 regmap.c
 * 编译期默认值 (版本经 -D 注入 0.3.0 => input_reg[0] == 0x0300)。
 */
#include "test_util.h"
#include "mb_server.h"
#include "init.h"
#include "io_hooks.h"

/* ==================== io_hooks 假件 (与 test_regmap.c 相同) ==================== */

static uint16_t fake_do_val;
static int fake_do_calls;
static bool fake_hist_en;
static int fake_hist_en_calls;
static int fake_sync_calls;
static time_t fake_ts_val;
static int fake_ts_calls;
static int fake_reboots;
static uint32_t fake_time;

void mb_set_do(uint16_t val)       { fake_do_val = val; fake_do_calls++; }
void history_enable_write(bool en) { fake_hist_en = en; fake_hist_en_calls++; }
void history_sync(void)            { fake_sync_calls++; }
bool set_timestamp(time_t t)        { fake_ts_val = t; fake_ts_calls++; return true; }
void io_reboot_cold(void)          { fake_reboots++; }
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
}

/* ==================== PDU 收发辅助 ==================== */

static uint8_t in_buf[300];
static uint8_t out_buf[300];
static uint16_t out_len;

static bool process(const uint8_t *pdu, uint16_t len)
{
    memcpy(in_buf, pdu, len);
    memset(out_buf, 0xAA, sizeof(out_buf)); /* 残留哨兵: 捕捉超长写 */
    out_len = 0;
    return mb_server_process(in_buf, len, out_buf, &out_len);
}

/* 正常响应: 全字节比对 + 长度含 fc + 哨兵未被踩 */
static void check_rsp(const uint8_t *pdu, uint16_t len,
                      const uint8_t *exp, uint16_t exp_len)
{
    TEST_ASSERT(process(pdu, len) == true);
    TEST_EQ_INT(out_len, exp_len);
    TEST_EQ_MEM(out_buf, exp, exp_len);
    TEST_EQ_INT(out_buf[exp_len], 0xAA);
}

/* 异常响应: fc|0x80 + 异常码, 共 2 字节。
 * 注意不检查 out_buf[2] 之后: FC01/02/03/04 在响应组装中途出错时
 * (Zephyr 同款) 缓冲里会残留部分数据, 以 out_len 为准 */
static void check_exc(const uint8_t *pdu, uint16_t len, uint8_t fc, uint8_t code)
{
    TEST_ASSERT(process(pdu, len) == true);
    TEST_EQ_INT(out_len, 2);
    TEST_EQ_INT(out_buf[0], fc | 0x80);
    TEST_EQ_INT(out_buf[1], code);
}

/* 静默丢弃: 返回 false, 无任何响应 */
static void check_drop(const uint8_t *pdu, uint16_t len)
{
    TEST_ASSERT(process(pdu, len) == false);
}

int main(void)
{
    /* ---- 1. FC03 正常: 读 0x0000..2 (DO=0, di_en=0xFFFF) + 全量 18 寄存器 ---- */
    /* 必须最先跑: 依赖 regmap.c 编译期默认值未被改写 */
    check_rsp((uint8_t[]){0x03, 0x00, 0x00, 0x00, 0x02}, 5,
              (uint8_t[]){0x03, 0x04, 0x00, 0x00, 0xFF, 0xFF}, 6);
    /* addr=0 qty=18: 全部合法 (fake_time=0 => 0x0E/0x0F 实时读为 0) */
    check_rsp((uint8_t[]){0x03, 0x00, 0x00, 0x00, 0x12}, 5,
              (uint8_t[]){0x03, 0x24,
                          0x00, 0x00, /* [0]  DO        */
                          0xFF, 0xFF, /* [1]  di_en     */
                          0x00, 0x0F, /* [2]  ai_en     */
                          0x00, 0xC8, /* [3]  di_si 200 */
                          0x00, 0xC8, /* [4]  ai_si 200 */
                          0x00, 0x00, /* [5]  his_en    */
                          0x01, 0x11, /* [6]  can_id    */
                          0x00, 0xFA, /* [7]  can_bps 250 */
                          0x25, 0x80, /* [8]  rs485_bps 9600 */
                          0x00, 0x01, /* [9]  slave_id  */
                          0x00, 0xC0, /* [10] ip1 192   */
                          0x00, 0xA8, /* [11] ip2 168   */
                          0x00, 0x0C, /* [12] ip3 12    */
                          0x00, 0x65, /* [13] ip4 101   */
                          0x00, 0x00, /* [14] ts_hi (实时) */
                          0x00, 0x00, /* [15] ts_lo (实时) */
                          0x00, 0x00, /* [16] cfg_save  */
                          0x00, 0x00, /* [17] reboot    */}, 38);

    /* ---- 2. FC03 异常: 地址 18 -> 0x02; qty 0/126 -> 0x03; 地址 5000 -> 0x01 ---- */
    check_exc((uint8_t[]){0x03, 0x00, 0x12, 0x00, 0x01}, 5, 0x03, 0x02);
    check_exc((uint8_t[]){0x03, 0x00, 0x00, 0x00, 0x00}, 5, 0x03, 0x03);
    check_exc((uint8_t[]){0x03, 0x00, 0x00, 0x00, 0x7E}, 5, 0x03, 0x03);
    check_exc((uint8_t[]){0x03, 0x13, 0x88, 0x00, 0x01}, 5, 0x03, 0x01);
    /* qty=125 通过数量门, 但读到地址 18 回调失败 -> 0x02 (非 0x03) */
    check_exc((uint8_t[]){0x03, 0x00, 0x00, 0x00, 0x7D}, 5, 0x03, 0x02);

    /* ---- 3. FC03 读 0x0E/0x0F: 实时时间拼装 (fake_time=0x12345678) ---- */
    fake_time = 0x12345678u;
    check_rsp((uint8_t[]){0x03, 0x00, 0x0E, 0x00, 0x02}, 5,
              (uint8_t[]){0x03, 0x04, 0x12, 0x34, 0x56, 0x78}, 6);
    /* 数组里的陈旧值不参与实时读 */
    TEST_EQ_INT(update_holding_reg(0x0E, 0xDEAD), 0);
    check_rsp((uint8_t[]){0x03, 0x00, 0x0E, 0x00, 0x01}, 5,
              (uint8_t[]){0x03, 0x02, 0x12, 0x34}, 4);

    /* ---- 4. FC04 读 input; FC01/02 读 coil/discrete (含越界 0x02) ---- */
    check_rsp((uint8_t[]){0x04, 0x00, 0x00, 0x00, 0x06}, 5,
              (uint8_t[]){0x04, 0x0C,
                          0x03, 0x00, /* ver 0.3.0 */
                          0x00, 0x00, 0x00, 0x00,
                          0x00, 0x00, 0x00, 0x00, /* AI1-4 */
                          0x00, 0x00}, 14); /* DI bitmap 默认 0 */
    check_exc((uint8_t[]){0x04, 0x00, 0x06, 0x00, 0x01}, 5, 0x04, 0x02);
    check_exc((uint8_t[]){0x04, 0x00, 0x00, 0x00, 0x00}, 5, 0x04, 0x03);
    check_exc((uint8_t[]){0x04, 0x00, 0x00, 0x00, 0x7E}, 5, 0x04, 0x03);
    check_exc((uint8_t[]){0x04, 0x13, 0x88, 0x00, 0x01}, 5, 0x04, 0x01);
    /* FC01 coil: DO=0x0A => coil1/3=1 */
    TEST_EQ_INT(update_holding_reg(0x00, 0x0A), 0);
    check_rsp((uint8_t[]){0x01, 0x00, 0x00, 0x00, 0x08}, 5,
              (uint8_t[]){0x01, 0x01, 0x0A}, 3);
    check_exc((uint8_t[]){0x01, 0x00, 0x08, 0x00, 0x01}, 5, 0x01, 0x02);
    check_exc((uint8_t[]){0x01, 0x00, 0x00, 0x00, 0x00}, 5, 0x01, 0x03);
    check_exc((uint8_t[]){0x01, 0x00, 0x00, 0x07, 0xD1}, 5, 0x01, 0x03);
    /* qty=2000 (上限内) 但 coil 只有 8 个 -> 0x02 */
    check_exc((uint8_t[]){0x01, 0x00, 0x00, 0x07, 0xD0}, 5, 0x01, 0x02);
    /* FC02 discrete: DI bitmap=0x8001, qty=16 -> 双字节位图 0b1000_0000_0000_0001 */
    TEST_EQ_INT(update_input_reg(INPUT_DI_IDX, 0x8001), 0);
    check_rsp((uint8_t[]){0x02, 0x00, 0x00, 0x00, 0x10}, 5,
              (uint8_t[]){0x02, 0x02, 0x01, 0x80}, 4);
    check_exc((uint8_t[]){0x02, 0x00, 0x10, 0x00, 0x01}, 5, 0x02, 0x02);
    check_exc((uint8_t[]){0x02, 0x00, 0x00, 0x00, 0x00}, 5, 0x02, 0x03);

    /* ---- 5. FC05: FF00=ON / 0000=OFF, 响应回显原值; bit>=8 -> 0x02 ---- */
    fakes_reset();
    TEST_EQ_INT(update_holding_reg(0x00, 0x00), 0);
    check_rsp((uint8_t[]){0x05, 0x00, 0x03, 0xFF, 0x00}, 5,
              (uint8_t[]){0x05, 0x00, 0x03, 0xFF, 0x00}, 5);
    TEST_EQ_INT(get_holding_reg(0x00), 0x08);
    TEST_EQ_INT(fake_do_val, 0x08);
    TEST_EQ_INT(fake_do_calls, 1);
    check_rsp((uint8_t[]){0x05, 0x00, 0x03, 0x00, 0x00}, 5,
              (uint8_t[]){0x05, 0x00, 0x03, 0x00, 0x00}, 5);
    TEST_EQ_INT(get_holding_reg(0x00), 0x00);
    /* 非 0x0000 的一律 ON (0x1234), 响应回显原始值 (不归一化为 FF00) */
    check_rsp((uint8_t[]){0x05, 0x00, 0x05, 0x12, 0x34}, 5,
              (uint8_t[]){0x05, 0x00, 0x05, 0x12, 0x34}, 5);
    TEST_EQ_INT(get_holding_reg(0x00), 0x20);
    check_exc((uint8_t[]){0x05, 0x00, 0x08, 0xFF, 0x00}, 5, 0x05, 0x02);

    /* ---- 6. FC06: 回显; 写 0x11=1 触发 reboot; 同值写跳过 ---- */
    fakes_reset();
    check_rsp((uint8_t[]){0x06, 0x00, 0x03, 0x00, 0x32}, 5,
              (uint8_t[]){0x06, 0x00, 0x03, 0x00, 0x32}, 5);
    TEST_EQ_INT(get_holding_reg(0x03), 0x32);
    TEST_EQ_INT(fake_do_calls, 0); /* 0x03 无 DO 副作用 */
    check_rsp((uint8_t[]){0x06, 0x00, 0x11, 0x00, 0x01}, 5,
              (uint8_t[]){0x06, 0x00, 0x11, 0x00, 0x01}, 5);
    TEST_EQ_INT(fake_reboots, 1);
    TEST_EQ_INT(fake_sync_calls, 1);
    TEST_EQ_INT(get_holding_reg(0x11), 0); /* reboot 后寄存器回 0 */
    /* 同值写 (0x11 已是 0) 跳过全部副作用 -> 无 reboot */
    check_rsp((uint8_t[]){0x06, 0x00, 0x11, 0x00, 0x00}, 5,
              (uint8_t[]){0x06, 0x00, 0x11, 0x00, 0x00}, 5);
    TEST_EQ_INT(fake_reboots, 1);
    TEST_EQ_INT(fake_sync_calls, 1);
    check_exc((uint8_t[]){0x06, 0x00, 0x12, 0x00, 0x01}, 5, 0x06, 0x02);
    check_exc((uint8_t[]){0x06, 0x13, 0x88, 0x00, 0x01}, 5, 0x06, 0x01);

    /* ---- 7. FC15: 多线圈写; 字节数规则违例 -> 0x03 ---- */
    fakes_reset();
    TEST_EQ_INT(update_holding_reg(0x00, 0x00), 0);
    /* qty=3, nbytes=1, bits=101b => DO=0x05; 响应回显 addr+qty */
    check_rsp((uint8_t[]){0x0F, 0x00, 0x00, 0x00, 0x03, 0x01, 0x05}, 7,
              (uint8_t[]){0x0F, 0x00, 0x00, 0x00, 0x03}, 5);
    TEST_EQ_INT(get_holding_reg(0x00), 0x05);
    /* qty=10 nbytes=2: 第 9 个线圈 (addr 8 >= DO_NUM) 写失败 -> 0x02,
     * 但已写的 coil0/2 保留 (DO=0x05) */
    TEST_EQ_INT(update_holding_reg(0x00, 0x00), 0);
    check_exc((uint8_t[]){0x0F, 0x00, 0x00, 0x00, 0x0A, 0x02, 0x05, 0x01}, 8,
              0x0F, 0x02);
    TEST_EQ_INT(get_holding_reg(0x00), 0x05);
    /* qty=0 -> 0x03 */
    check_exc((uint8_t[]){0x0F, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00}, 7,
              0x0F, 0x03);
    /* nbytes 与 qty 不匹配: ((3-1)/8)+1=1 != 2 -> 0x03 */
    check_exc((uint8_t[]){0x0F, 0x00, 0x00, 0x00, 0x03, 0x02, 0x05, 0x01}, 8,
              0x0F, 0x03);
    /* dlen != nbytes+5: qty=9 nbytes=2 => 应为 7 字节 data, 只有 6 -> 0x03 */
    check_exc((uint8_t[]){0x0F, 0x00, 0x00, 0x00, 0x09, 0x02, 0x05}, 7,
              0x0F, 0x03);
    /* qty=2001, 字节数自洽 (251) -> 0x03 */
    {
        uint8_t pdu[260];
        pdu[0] = 0x0F;
        pdu[1] = 0x00; pdu[2] = 0x00;         /* addr 0 */
        pdu[3] = 0x07; pdu[4] = 0xD1;         /* qty 2001 */
        pdu[5] = 251;                          /* ((2001-1)/8)+1 */
        memset(&pdu[6], 0, 251);
        check_exc(pdu, 6 + 251, 0x0F, 0x03);
    }
    /* data 长 < 6 -> 静默丢弃 */
    check_drop((uint8_t[]){0x0F, 0x00, 0x00, 0x00, 0x03, 0x01}, 6);

    /* ---- 8. FC16: 多寄存器写 (逐个写, 失败时已写保留) ---- */
    check_rsp((uint8_t[]){0x10, 0x00, 0x01, 0x00, 0x02, 0x04, 0x00, 0x05, 0x00, 0x06}, 10,
              (uint8_t[]){0x10, 0x00, 0x01, 0x00, 0x02}, 5);
    TEST_EQ_INT(get_holding_reg(0x01), 5);
    TEST_EQ_INT(get_holding_reg(0x02), 6);
    /* 写 addr 17..18: 第 1 个成功, 18 越界 -> 0x02, 但 17 已生效且保留
     * (Zephyr 逐寄存器调 holding_reg_wr_cb, 失败前的写入不回滚)。
     * 预置 0x11=5, 写 0 -> 值变化但不满足 reboot 的 reg!=0 条件 */
    fakes_reset();
    TEST_EQ_INT(update_holding_reg(0x11, 5), 0);
    check_exc((uint8_t[]){0x10, 0x00, 0x11, 0x00, 0x02, 0x04, 0x00, 0x00, 0x00, 0xAB}, 10,
              0x10, 0x02);
    TEST_EQ_INT(get_holding_reg(0x11), 0);
    TEST_EQ_INT(fake_reboots, 0);
    TEST_EQ_INT(fake_sync_calls, 0);
    /* qty=0 -> 0x03 */
    check_exc((uint8_t[]){0x10, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00}, 7,
              0x10, 0x03);
    /* qty=126, 字节数自洽 (252) -> 0x03 */
    {
        uint8_t pdu[260];
        pdu[0] = 0x10;
        pdu[1] = 0x00; pdu[2] = 0x01;         /* addr 1 */
        pdu[3] = 0x00; pdu[4] = 0x7E;         /* qty 126 */
        pdu[5] = 252;                          /* 2*126 */
        memset(&pdu[6], 0, 252);
        check_exc(pdu, 6 + 252, 0x10, 0x03);
    }
    /* (dlen-5) != nbytes -> 0x03 */
    check_exc((uint8_t[]){0x10, 0x00, 0x01, 0x00, 0x02, 0x04, 0x00, 0x05}, 8,
              0x10, 0x03);
    /* nbytes != 2*qty (5 != 4), 但 dlen-5 == nbytes -> 0x03 */
    check_exc((uint8_t[]){0x10, 0x00, 0x01, 0x00, 0x02, 0x05, 0x00, 0x05, 0x00, 0x06, 0x00}, 11,
              0x10, 0x03);
    /* FP 扩展区 -> 0x01 */
    check_exc((uint8_t[]){0x10, 0x13, 0x88, 0x00, 0x01, 0x02, 0x00, 0x05}, 8,
              0x10, 0x01);
    /* data 长 < 6 -> 静默丢弃 */
    check_drop((uint8_t[]){0x10, 0x00, 0x01, 0x00, 0x02}, 5);

    /* ---- 9. FC08 诊断: 0x00 回显 / 0x0A 清零 / 0x0B-0x0F 读计数 / 未知 -> 0x01 ---- */
    check_rsp((uint8_t[]){0x08, 0x00, 0x00, 0xA5, 0x37}, 5,
              (uint8_t[]){0x08, 0x00, 0x00, 0xA5, 0x37}, 5);
    /* 清零 (本条请求先计数后清零 => 清零后全 0) */
    check_rsp((uint8_t[]){0x08, 0x00, 0x0A, 0x00, 0x00}, 5,
              (uint8_t[]){0x08, 0x00, 0x0A, 0x00, 0x00}, 5);
    /* 制造已知计数: 1 条正常 FC03 + 1 条异常 (未知 FC) + 1 条静默丢弃 */
    TEST_ASSERT(process((uint8_t[]){0x03, 0x00, 0x00, 0x00, 0x01}, 5) == true);
    check_exc((uint8_t[]){0x07, 0x00, 0x00, 0x00, 0x00}, 5, 0x07, 0x01);
    check_drop((uint8_t[]){0x03, 0x00, 0x00}, 3);
    /* 传输层上报: 2 次 CRC 错 + 1 次无响应 */
    mb_server_diag_count(MB_DIAG_CRC_ERR);
    mb_server_diag_count(MB_DIAG_CRC_ERR);
    mb_server_diag_count(MB_DIAG_NO_RESP);
    /* 状态: bus=3 (含静默丢弃那条), srv=3, exc=1, crc=2, no_resp=1。
     * 每条 FC08 读请求自身先计入 bus/srv 再返回计数值。 */
    check_rsp((uint8_t[]){0x08, 0x00, 0x0B, 0x00, 0x00}, 5,
              (uint8_t[]){0x08, 0x00, 0x0B, 0x00, 0x04}, 5); /* bus: 3+1 */
    check_rsp((uint8_t[]){0x08, 0x00, 0x0C, 0x00, 0x00}, 5,
              (uint8_t[]){0x08, 0x00, 0x0C, 0x00, 0x02}, 5); /* crc: 2 */
    check_rsp((uint8_t[]){0x08, 0x00, 0x0D, 0x00, 0x00}, 5,
              (uint8_t[]){0x08, 0x00, 0x0D, 0x00, 0x01}, 5); /* exc: 1 (静默丢弃不增) */
    check_rsp((uint8_t[]){0x08, 0x00, 0x0E, 0x00, 0x00}, 5,
              (uint8_t[]){0x08, 0x00, 0x0E, 0x00, 0x07}, 5); /* srv: 3+4 */
    check_rsp((uint8_t[]){0x08, 0x00, 0x0F, 0x00, 0x00}, 5,
              (uint8_t[]){0x08, 0x00, 0x0F, 0x00, 0x01}, 5); /* noresp: 1 */
    /* 未知子功能 -> 0x01 */
    check_exc((uint8_t[]){0x08, 0x00, 0x01, 0x00, 0x00}, 5, 0x08, 0x01);
    check_exc((uint8_t[]){0x08, 0x00, 0x10, 0x00, 0x00}, 5, 0x08, 0x01);
    /* data 长 != 4 -> 静默丢弃 */
    check_drop((uint8_t[]){0x08, 0x00, 0x00, 0xA5}, 4);

    /* ---- 10. 未知 FC -> 0x01; 各 FC 长度违例 -> 静默丢弃 ---- */
    check_exc((uint8_t[]){0x07, 0x00, 0x00, 0x00, 0x00}, 5, 0x07, 0x01);
    check_exc((uint8_t[]){0x11, 0x00, 0x00, 0x00, 0x00}, 5, 0x11, 0x01);
    check_exc((uint8_t[]){0x41, 0x00, 0x00, 0x00, 0x00}, 5, 0x41, 0x01);
    check_exc((uint8_t[]){0x07}, 1, 0x07, 0x01); /* 未知 FC 无长度前提 */
    check_drop((uint8_t[]){0x03, 0x00, 0x00, 0x00, 0x01, 0x00}, 6); /* FC03 dlen=5 */
    check_drop((uint8_t[]){0x01, 0x00, 0x00, 0x00}, 4);             /* FC01 dlen=3 */
    check_drop((uint8_t[]){0x02, 0x00, 0x00, 0x00, 0x00, 0x00}, 6); /* FC02 dlen=5 */
    check_drop((uint8_t[]){0x04, 0x00, 0x00, 0x00, 0x00, 0x00}, 6); /* FC04 dlen=5 */
    check_drop((uint8_t[]){0x05, 0x00, 0x03, 0xFF}, 4);             /* FC05 dlen=3 */
    check_drop((uint8_t[]){0x06, 0x00, 0x03, 0x00}, 4);             /* FC06 dlen=3 */
    check_drop((uint8_t[]){0x08, 0x00, 0x00, 0xA5, 0x37, 0x00}, 6); /* FC08 dlen=5 */

    /* ---- 11. 广播 (unit_id==0) 不应答: 由传输层在 PDU 之外判定,
     *  对 mb_server_process 透明 -> Task 10/11 传输层测试覆盖 ---- */

    TEST_MAIN_END();
}
