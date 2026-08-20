/*
 * test_rtu_frame.c - Modbus RTU 帧状态机 (src/modbus/rtu_frame.c) 主机测试。
 * 链接 rtu_frame.c + mb_server.c + regmap.c + config_store.c + fake_flash.c,
 * io_hooks 假件同 test_mbtcp_adu.c; tx 用捕获假件, rtu_t35_kick 用计数
 * 强符号覆盖 rtu_frame.c 的弱默认 (锁定"每次喂数据重启 t3.5 钩子"契约)。
 *
 * 语义基准: Zephyr subsys/modbus/modbus_serial.c (IRQ API 路径) 帧语义 +
 * modbus_core.c modbus_rx_handler + modbus_server.c modbus_server_handler:
 * 帧 [unit][pdu][crc16 LE16], len<4 / 溢出 / CRC 错 / 他站 unit 静默丢弃,
 * 广播执行副作用不应答; 诊断计数组合经 FC08 子功能交叉验证
 * (bus_msg 含被丢弃的帧 —— Zephyr handler 入口无条件 +1;
 *  crc_err 仅 CRC 错; no_resp 覆盖所有丢弃/广播路径)。
 */
#include "test_util.h"
#include "rtu_frame.h"
#include "mb_server.h"
#include "init.h"
#include "io_hooks.h"
#include "io_crc.h"

/* ==================== io_hooks 假件 (与 test_mbtcp_adu.c 相同) ==================== */

static uint16_t fake_do_val;
static int fake_do_calls;
static bool fake_hist_en;
static int fake_hist_en_calls;
static int fake_sync_calls;
static time_t fake_ts_val;
static int fake_ts_calls;
static int fake_reboots;
static uint32_t fake_time;

int mb_set_do(uint16_t val)       { fake_do_val = val; fake_do_calls++; return 0; }
void history_enable_write(bool en) { fake_hist_en = en; fake_hist_en_calls++; }
void history_sync(void)            { fake_sync_calls++; }
bool set_timestamp(time_t t)        { fake_ts_val = t; fake_ts_calls++; return true; }
void io_reboot_cold(void)          { fake_reboots++; }
uint32_t io_now_epoch(void)        { return fake_time; }
void io_lock(void) {}
void io_unlock(void) {}

/* ==================== tx 捕获 / t3.5 钩子计数 ==================== */

static uint8_t tx_buf[300];
static uint16_t tx_len;
static int tx_calls;

static void fake_tx(const uint8_t *frame, uint16_t len)
{
    TEST_ASSERT(tx_calls == 0); /* 一帧至多一次应答 */
    TEST_ASSERT(len <= sizeof(tx_buf));
    memcpy(tx_buf, frame, len);
    tx_len = len;
    tx_calls++;
}

static int kick_calls;
void rtu_t35_kick(void) { kick_calls++; } /* 强符号覆盖弱默认 */

/* ==================== 帧构造 / 投递 ==================== */

static uint8_t frame[300];

/* [unit][pdu...][crc16 低字节][crc16 高字节], 返回帧长 */
static uint16_t mk_frame(uint8_t unit, const uint8_t *pdu, uint16_t pdu_len)
{
    uint16_t crc;

    frame[0] = unit;
    memcpy(&frame[1], pdu, pdu_len);
    crc = crc16_modbus(frame, (size_t)pdu_len + 1);
    frame[1 + pdu_len] = (uint8_t)(crc & 0xFF);
    frame[2 + pdu_len] = (uint8_t)(crc >> 8);
    return (uint16_t)(pdu_len + 3);
}

static void run(const uint8_t *f, uint16_t len)
{
    tx_calls = 0; /* 每帧的应答期望以本帧为界 */
    rtu_rx_feed(f, len);
    rtu_t35_expired();
}

/* FC03 读保持寄存器 (addr 0x08 = RS485 波特率, 上电默认 9600=0x2580) */
static uint16_t mk_fc03(uint8_t unit)
{
    return mk_frame(unit, (uint8_t[]){0x03, 0x00, 0x08, 0x00, 0x01}, 5);
}

/* 经帧层发 FC08 并解析应答值 (读自身计 bus/srv, 见 diag_check 注释) */
static int diag(uint16_t sub)
{
    uint8_t pdu[5] = {0x08, (uint8_t)(sub >> 8), (uint8_t)(sub & 0xFF), 0x00, 0x00};

    run(frame, mk_frame(1, pdu, 5));
    TEST_ASSERT(tx_calls == 1);
    return (int)(((tx_buf[4] & 0xFF) << 8) | tx_buf[5]);
}

static void diag_clear(void)
{
    uint8_t pdu[5] = {0x08, 0x00, 0x0A, 0x00, 0x00};

    run(frame, mk_frame(1, pdu, 5));
}

/* FC08 0x0A 清零 -> action -> 依序读 0x0B/0x0C/0x0E/0x0F (第 1/2/3/4 次读,
 * 每次读自身进解码器计 bus+1/srv+1, 不影响 crc/no_resp):
 *   bus   = act_bus + 1    (第 1 次读时)
 *   crc   = act_crc
 *   srv   = act_srv + 3    (第 3 次读时)
 *   noresp= act_noresp
 * 其中 act_* 为 action 帧本身的计数贡献 (传输层补的 bus_msg 与解码器
 * 入口的 bus/srv 均计入)。 */
static void diag_check(int act_bus, int act_crc, int act_srv, int act_noresp)
{
    TEST_EQ_INT(diag(0x0B), act_bus + 1);
    TEST_EQ_INT(diag(0x0C), act_crc);
    TEST_EQ_INT(diag(0x0E), act_srv + 3);
    TEST_EQ_INT(diag(0x0F), act_noresp);
}

int main(void)
{
    uint16_t n;
    uint8_t exp[16];
    uint16_t crc;
    uint8_t big[257];

    rtu_reset();
    rtu_frame_bind(1, 9600, fake_tx); /* regmap 上电默认: 从站号 1 / 9600 */

    /* ---- 0. t3.5 周期: ceil(3.5*11bit/baud); >19200 固定 2ms ---- */
    TEST_EQ_INT(rtu_t35_ms(1200), 33);   /* 32083.3us -> 33ms */
    TEST_EQ_INT(rtu_t35_ms(9600), 5);    /* 4010.4us  -> 5ms */
    TEST_EQ_INT(rtu_t35_ms(19200), 3);   /* 2005.2us  -> 3ms */
    TEST_EQ_INT(rtu_t35_ms(38400), 2);   /* >19200 -> 2ms */
    TEST_EQ_INT(rtu_t35_ms(115200), 2);

    /* ---- 1. 合法 FC03 (unit=1, 分两段喂): 正确响应帧 (含 CRC LE16) ---- */
    n = mk_fc03(1);
    kick_calls = 0;
    rtu_rx_feed(frame, 2);                    /* 帧内分段 (IDLE 事件粒度) */
    rtu_rx_feed(&frame[2], (uint16_t)(n - 2));
    TEST_EQ_INT(kick_calls, 2);               /* 每次喂数据都重启 t3.5 */
    TEST_EQ_INT(tx_calls, 0);                 /* t3.5 前不应答 */
    rtu_t35_expired();
    TEST_EQ_INT(tx_calls, 1);
    TEST_EQ_INT(tx_len, 7);                   /* unit + fc03/cnt/data + crc2 */
    exp[0] = 0x01; exp[1] = 0x03; exp[2] = 0x02; exp[3] = 0x25; exp[4] = 0x80;
    crc = crc16_modbus(exp, 5);
    exp[5] = (uint8_t)(crc & 0xFF); exp[6] = (uint8_t)(crc >> 8);
    TEST_EQ_MEM(tx_buf, exp, 7);
    /* 诊断: 单播正常帧 bus+srv 各 1 (解码器), 无 crc/no_resp */
    diag_clear();
    run(frame, mk_fc03(1));
    TEST_EQ_INT(tx_calls, 1);
    diag_check(1, 0, 1, 0);

    /* ---- 2. CRC 破坏 (高字节取反): 静默; bus+1 crc+1 noresp+1 ---- */
    diag_clear();
    n = mk_fc03(1);
    frame[n - 1] ^= 0xFF;
    tx_calls = 0;
    run(frame, n);
    TEST_EQ_INT(tx_calls, 0);
    diag_check(1, 1, 0, 1);

    /* ---- 3a. unit=5 (非本站非广播): 静默; bus+1 noresp+1 ---- */
    diag_clear();
    tx_calls = 0;
    run(frame, mk_fc03(5));
    TEST_EQ_INT(tx_calls, 0);
    diag_check(1, 0, 0, 1);

    /* ---- 3b. unit=0 广播 FC06: 寄存器已写、无应答; bus/srv (解码器) + noresp ---- */
    diag_clear();
    tx_calls = 0;
    run(frame, mk_frame(0, (uint8_t[]){0x06, 0x00, 0x03, 0x00, 0x32}, 5));
    TEST_EQ_INT(tx_calls, 0);
    TEST_EQ_INT(get_holding_reg(0x03), 0x0032); /* 副作用已执行 */
    diag_check(1, 0, 1, 1);

    /* ---- 4. 长度 3 (<4): 静默; bus+1 noresp+1 (Zephyr -EMSGSIZE 路径) ---- */
    diag_clear();
    tx_calls = 0;
    run((uint8_t[]){0x01, 0x03, 0x00}, 3);
    TEST_EQ_INT(tx_calls, 0);
    diag_check(1, 0, 0, 1);

    /* ---- 5. 257 字节溢出: 无应答 + 状态复位 (后续帧恢复正常) ---- */
    diag_clear();
    memset(big, 0xA5, sizeof(big));
    big[0] = 0x01;
    kick_calls = 0;
    rtu_rx_feed(big, 257);                   /* 第 257 字节起丢弃, 但仍踢 t3.5 */
    rtu_rx_feed(big, 1);
    TEST_EQ_INT(kick_calls, 2);
    tx_calls = 0;
    rtu_t35_expired();
    TEST_EQ_INT(tx_calls, 0);
    /* 状态复位: 紧跟一帧合法 FC03 正常应答 */
    run(frame, mk_fc03(1));
    TEST_EQ_INT(tx_calls, 1);
    TEST_EQ_INT(tx_len, 7);
    TEST_EQ_MEM(tx_buf, exp, 7);
    /* 溢出帧: bus+1 noresp+1 (长度违例类, 不计 crc); 合法帧再 bus/srv+1 */
    diag_check(2, 0, 1, 1);

    /* ---- 6. 空 t3.5 到期 (无字节): 无操作, 不计任何计数 ---- */
    diag_clear();
    tx_calls = 0;
    rtu_t35_expired();
    TEST_EQ_INT(tx_calls, 0);
    diag_check(0, 0, 0, 0);

    TEST_MAIN_END();
}
