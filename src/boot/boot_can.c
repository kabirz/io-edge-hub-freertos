/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * MCUboot CAN 紧急升级实现 (Zephyr can_fw_boot.c + can_fw_upgrade.c
 * BOOT_WAIT 分支的裸机移植):
 *
 * bxCAN 寄存器级驱动 (无 HAL/无中断, 轮询 FIFO0): ONE_SHOT (NART=1)
 * 对齐 Zephyr boot 域 — 波特率失配总线上首帧即触发无限重传, 一次发送
 * 后邮箱必然释放。250kbps 定值 (boot 无 config store)。
 * 阶段经 0x108 trace 帧广播 (串口不可用时的黑匣子)。
 */

#include <string.h>

#include "main.h"

#include "boot_can.h"
#include "boot_uart.h"
#include "flash_layout.h"
#include "fw_keyhash.h"
#include "fw_upg.h" /* FW_KEYHASH_LEN */
#include "fw_version.h"
#include "intflash.h"
#include "io_watchdog.h"

/* ==================== 协议常量 (与 app 域 fw_can.c / Zephyr 一致) ==================== */

#define CAN_ID_CMD 0x101u     /* [cmd LE32][arg LE32] */
#define CAN_ID_REPLY 0x102u   /* [code LE32][arg LE32] */
#define CAN_ID_DATA 0x103u    /* <=8B 数据帧 */
#define CAN_ID_KEYHASH 0x104u /* [seq][7B] x5 */
#define CAN_ID_VERSION 0x105u /* [seq][7B 文本] (设备发) */
#define CAN_ID_PROBE 0x106u   /* 探测帧 (设备发) */
#define CAN_ID_ACK 0x107u     /* 探测响应 (上位机发, 任意 1B) */
#define CAN_ID_TRACE 0x108u   /* 阶段记录 (设备发) */

enum {
    FW_CMD_START = 0,
    FW_CMD_CONFIRM = 1,
    FW_CMD_VERSION = 2,
    FW_CMD_REBOOT = 3,
};

enum {
    FW_CODE_OFFSET = 0,
    FW_CODE_UPDATE_SUCCESS = 1,
    FW_CODE_VERSION = 2,
    FW_CODE_CONFIRM = 3,
    FW_CODE_FLASH_ERROR = 4,
    FW_CODE_TRANSFER_ERROR = 5,
    FW_CODE_KEYHASH_ERROR = 6,
};

#define FW_CONFIRM_MAGIC 0x55AA55AAu
#define PROBE_MAGIC 0x42544F31u /* "BTO1" */

enum {
    TRACE_HOOK_ENTER = 2,
    TRACE_HOST_ACK = 3,
    TRACE_CONFIRMED = 4,
    TRACE_PROCEED = 5,
    TRACE_FW_START = 6,
};

#define KEYHASH_CHUNK 7u
#define KEYHASH_CHUNKS ((FW_KEYHASH_LEN + KEYHASH_CHUNK - 1u) / KEYHASH_CHUNK)
#define KEYHASH_FULL_MASK ((1u << KEYHASH_CHUNKS) - 1u)

#define PROBE_TIMEOUT_MS 500u
#define PROBE_INTERVAL_MS 200u
#define IDLE_TIMEOUT_MS 15000u
#define ACK_INTERVAL 64u

/* ==================== 会话状态 ==================== */

static struct {
    bool active;
    uint32_t total;
    uint32_t written;
    bool confirmed;   /* 会话结束 (CONFIRM 或 REBOOT) */
    bool installed;   /* CONFIRM 装好镜像 (调用方据此复位重启) */
    uint32_t last_activity;
    uint8_t keybuf[FW_KEYHASH_LEN];
    uint8_t key_mask;
} s;

/* ==================== bxCAN 寄存器级驱动 ==================== */

/* 250kbps @ PCLK1 42MHz: presc 21, BS1 6TQ, BS2 1TQ (87.5% 采样点) */
static int can_init(void)
{
    uint32_t t;

    __HAL_RCC_CAN1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* PA11 RX / PA12 TX, AF9 */
    {
        GPIO_InitTypeDef io = {0};

        io.Pin = GPIO_PIN_11 | GPIO_PIN_12;
        io.Mode = GPIO_MODE_AF_PP;
        io.Pull = GPIO_NOPULL;
        io.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        io.Alternate = GPIO_AF9_CAN1;
        HAL_GPIO_Init(GPIOA, &io);
    }

    /* 进初始化模式 */
    CAN1->MCR |= CAN_MCR_INRQ;
    t = 1000;
    while ((CAN1->MSR & CAN_MCR_INRQ) == 0u) {
        if (--t == 0u) {
            return -1;
        }
    }
    /* NART=1 (one-shot) + ABOM=1 (bus-off 自动恢复); 不使能任何中断 */
    CAN1->MCR = CAN_MCR_INRQ | CAN_MCR_NART | CAN_MCR_ABOM;
    CAN1->BTR = (0u << CAN_BTR_SJW_Pos) |   /* SJW 1TQ */
                (5u << CAN_BTR_TS1_Pos) |   /* BS1 6TQ */
                (0u << CAN_BTR_TS2_Pos) |   /* BS2 1TQ */
                (21u - 1u);                 /* 分频 21 -> 250kbps */

    /* 过滤器组 0: 32 位掩码 (id=0, mask=0) 全收 -> FIFO0 */
    CAN1->FMR |= CAN_FMR_FINIT;
    CAN1->sFilterRegister[0].FR1 = 0u;
    CAN1->sFilterRegister[0].FR2 = 0u;
    CAN1->FM1R &= ~CAN_FM1R_FBM0;  /* 掩码模式 */
    CAN1->FS1R |= CAN_FS1R_FSC0;   /* 32 位刻度 */
    CAN1->FFA1R &= ~CAN_FFA1R_FFA0; /* FIFO0 */
    CAN1->FA1R |= CAN_FA1R_FACT0;  /* 激活 */
    CAN1->FMR &= ~CAN_FMR_FINIT;

    /* 退出初始化模式 (总线参与: 需要至少一个应答节点才完成同步;
     * 独占总线时 INAK 不清零 — 用超时继续, 探测帧 one-shot 发送) */
    CAN1->MCR &= ~CAN_MCR_INRQ;
    t = 1000;
    while ((CAN1->MSR & CAN_MSR_INAK) != 0u) {
        if (--t == 0u) {
            break;
        }
    }
    return 0;
}

/* 引导收尾: 回初始化模式释放总线 (app 侧 HAL_CAN_Init 全量重配) */
static void can_leave(void)
{
    CAN1->MCR |= CAN_MCR_INRQ;
    CAN1->MCR |= CAN_MCR_SLEEP;
}

static void can_send(uint32_t id, const uint8_t *data, uint8_t len)
{
    uint32_t box;
    uint32_t t;

    if (len > 8u) {
        return;
    }
    /* 找空邮箱: TSR.CODE 指示下一个可用, RQCP/TME 位域判定 */
    if ((CAN1->TSR & CAN_TSR_TME0) != 0u) {
        box = 0u;
    } else if ((CAN1->TSR & CAN_TSR_TME1) != 0u) {
        box = 1u;
    } else if ((CAN1->TSR & CAN_TSR_TME2) != 0u) {
        box = 2u;
    } else {
        return; /* 无空邮箱 (one-shot 必然释放) */
    }

    {
        uint32_t dl = 0, dh = 0;

        for (uint8_t i = 0; i < len; i++) {
            if (i < 4u) {
                dl |= (uint32_t)data[i] << (8u * i);
            } else {
                dh |= (uint32_t)data[i] << (8u * (i - 4u));
            }
        }
        CAN1->sTxMailBox[box].TDLR = dl;
        CAN1->sTxMailBox[box].TDHR = dh;
    }
    CAN1->sTxMailBox[box].TDTR = len;
    CAN1->sTxMailBox[box].TIR = (id & 0x7FFu) << CAN_TI0R_STID_Pos |
                                CAN_TI0R_TXRQ;

    /* one-shot: 无 ACK 单次失败即完成, 等待上限 ~100ms */
    t = 100000u;
    while (t-- > 0u) {
        if ((CAN1->TSR & (CAN_TSR_RQCP0 << (box * 8u))) != 0u) {
            CAN1->TSR = CAN_TSR_RQCP0 << (box * 8u); /* 清完成标志 */
            return;
        }
    }
}

/* 轮询收一帧; 返回 dlc (0=无帧, 0xFF=错误释放) */
static uint8_t can_recv(uint32_t *id, uint8_t *data)
{
    uint32_t rir, rdtr, dl, dh;
    uint8_t len, i;

    if ((CAN1->RF0R & CAN_RF0R_FMP0) == 0u) {
        return 0u;
    }
    rir = CAN1->sFIFOMailBox[0].RIR;
    rdtr = CAN1->sFIFOMailBox[0].RDTR;
    dl = CAN1->sFIFOMailBox[0].RDLR;
    dh = CAN1->sFIFOMailBox[0].RDHR;
    CAN1->RF0R |= CAN_RF0R_RFOM0; /* 释放 FIFO0 */

    if ((rir & CAN_RI0R_IDE) != 0u) {
        return 0xFFu; /* 仅标准帧 */
    }
    *id = (rir & CAN_RI0R_STID) >> CAN_RI0R_STID_Pos;
    len = (uint8_t)(rdtr & CAN_RDT0R_DLC);
    if (len > 8u) {
        len = 8u;
    }
    for (i = 0; i < len; i++) {
        uint32_t w = i < 4u ? dl : dh;
        uint8_t sh = (uint8_t)((i & 3u) * 8u);

        data[i] = (uint8_t)(w >> sh);
    }
    return len;
}

/* ==================== 应答 / 探测 / trace ==================== */

static void fw_reply(uint32_t code, uint32_t arg)
{
    uint8_t d[8];

    d[0] = (uint8_t)code;
    d[1] = (uint8_t)(code >> 8);
    d[2] = (uint8_t)(code >> 16);
    d[3] = (uint8_t)(code >> 24);
    d[4] = (uint8_t)arg;
    d[5] = (uint8_t)(arg >> 8);
    d[6] = (uint8_t)(arg >> 16);
    d[7] = (uint8_t)(arg >> 24);
    can_send(CAN_ID_REPLY, d, 8);
}

static void send_probe(void)
{
    uint8_t d[8];

    /* 魔数按 LE32 写入 (Zephyr data_32[0]=0x42544F31 语义,
     * 线上字节序 31 4F 54 42, 上位机按 '<I' 解) */
    d[0] = (uint8_t)PROBE_MAGIC;
    d[1] = (uint8_t)(PROBE_MAGIC >> 8);
    d[2] = (uint8_t)(PROBE_MAGIC >> 16);
    d[3] = (uint8_t)(PROBE_MAGIC >> 24);
    d[4] = (uint8_t)FW_VERSION_MAJOR;
    d[5] = (uint8_t)FW_VERSION_MINOR;
    d[6] = (uint8_t)FW_VERSION_PATCH;
    d[7] = 0;
    can_send(CAN_ID_PROBE, d, 8);
}

static void send_trace(uint8_t phase)
{
    can_send(CAN_ID_TRACE, &phase, 1);
}

static void send_version_string(const char *ver, uint8_t len)
{
    for (uint8_t off = 0, seq = 0; off < len; off += 7u, seq++) {
        uint8_t chunk = (uint8_t)(len - off);
        uint8_t d[8];

        if (chunk > 7u) {
            chunk = 7u;
        }
        d[0] = seq;
        memcpy(&d[1], &ver[off], chunk);
        memset(&d[1 + chunk], 0, 7u - chunk);
        can_send(CAN_ID_VERSION, d, 8);
    }
}

/* "vM.m.p_<git>" 手工拼装 (boot 域无 newlib snprintf) */
static int put_u8(char *p, uint32_t v)
{
    int n = 0;

    if (v >= 100u) {
        p[n++] = (char)('0' + v / 100u);
    }
    if (v >= 10u) {
        p[n++] = (char)('0' + (v / 10u) % 10u);
    }
    p[n++] = (char)('0' + v % 10u);
    return n;
}

static int build_version(char *ver, int max)
{
    const char *g = FW_GIT_VERSION;
    int n = 0;

    ver[n++] = 'v';
    n += put_u8(&ver[n], (uint32_t)FW_VERSION_MAJOR);
    ver[n++] = '.';
    n += put_u8(&ver[n], (uint32_t)FW_VERSION_MINOR);
    ver[n++] = '.';
    n += put_u8(&ver[n], (uint32_t)FW_VERSION_PATCH);
    ver[n++] = '_';
    while (*g != '\0' && n < max - 1) {
        ver[n++] = *g++;
    }
    ver[n] = '\0';
    return n;
}

/* ==================== 协议帧处理 ==================== */

static void handle_keyhash(const uint8_t *data, uint8_t dlc)
{
    uint8_t seq = data[0];
    uint8_t rem = FW_KEYHASH_LEN - seq * KEYHASH_CHUNK;
    uint8_t chunk = rem < KEYHASH_CHUNK ? rem : KEYHASH_CHUNK;

    if (seq >= KEYHASH_CHUNKS || dlc < 1u + chunk) {
        return;
    }
    memcpy(&s.keybuf[seq * KEYHASH_CHUNK], &data[1], chunk);
    s.key_mask |= (uint8_t)(1u << seq);
}

static void handle_cmd(const uint8_t *data, uint8_t dlc)
{
    uint32_t cmd = (uint32_t)data[0] | (uint32_t)data[1] << 8 |
                   (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
    uint32_t arg = (uint32_t)data[4] | (uint32_t)data[5] << 8 |
                   (uint32_t)data[6] << 16 | (uint32_t)data[7] << 24;

    if (dlc != 8u) {
        fw_reply(FW_CODE_FLASH_ERROR, 0);
        return;
    }

    switch (cmd) {
    case FW_CMD_START: {
        uint8_t *kh = NULL;
        uint32_t erase_len;

        send_trace(TRACE_FW_START);
        if ((s.key_mask & KEYHASH_FULL_MASK) == KEYHASH_FULL_MASK) {
            s.key_mask = 0;
            kh = s.keybuf;
        }
        if (kh != NULL && memcmp(kh, fw_keyhash, FW_KEYHASH_LEN) != 0) {
            boot_log("boot can: keyhash mismatch");
            fw_reply(FW_CODE_KEYHASH_ERROR, 0);
            return;
        }
        if (arg < 64u || arg > SLOT0_SIZE) {
            fw_reply(FW_CODE_FLASH_ERROR, 0);
            return;
        }
        /* 擦 slot0 覆盖镜像 (内部扇区 64K/128K, 逐扇区喂狗) */
        erase_len = (arg + 0xFFFu) & ~0xFFFu;
        if (intflash_erase(SLOT0_ADDR, erase_len) != 0) {
            boot_log("boot can: slot0 erase failed");
            fw_reply(FW_CODE_FLASH_ERROR, 0);
            return;
        }
        s.active = true;
        s.total = arg;
        s.written = 0;
        boot_log("boot can: start size=%u", (unsigned)arg);
        fw_reply(FW_CODE_OFFSET, 0);
        break;
    }

    case FW_CMD_CONFIRM:
        if (!s.active) {
            fw_reply(FW_CODE_TRANSFER_ERROR, 0);
            return;
        }
        s.active = false;
        if (s.written != s.total) {
            boot_log("boot can: size mismatch %u != %u",
                     (unsigned)s.written, (unsigned)s.total);
            fw_reply(FW_CODE_TRANSFER_ERROR, 0);
            return;
        }
        /* 直写 slot0 完成: 置确认位, 会话结束 (调用方继续 boot_go
         * 验签启动; forever 模式由调用方复位重启) */
        boot_log("boot can: confirmed, written=%u", (unsigned)s.written);
        fw_reply(FW_CODE_CONFIRM, FW_CONFIRM_MAGIC);
        s.installed = true;
        s.confirmed = true;
        break;

    case FW_CMD_VERSION: {
        char ver[24];
        int len = build_version(ver, (int)sizeof(ver));

        fw_reply(FW_CODE_VERSION, (uint32_t)len);
        send_version_string(ver, (uint8_t)len);
        break;
    }

    case FW_CMD_REBOOT:
        /* 结束等待继续引导 (应用侧 REBOOT 进 boot 的入口场景) */
        s.confirmed = true;
        break;

    default:
        break;
    }
}

static void handle_data(const uint8_t *data, uint8_t dlc)
{
    if (!s.active) {
        fw_reply(FW_CODE_TRANSFER_ERROR, 0);
        return;
    }
    if (dlc == 0u || s.written + dlc > s.total) {
        s.active = false;
        fw_reply(FW_CODE_TRANSFER_ERROR, 0);
        return;
    }
    if (intflash_write(SLOT0_ADDR + s.written, data, dlc) != 0) {
        boot_log("boot can: write failed @%u", (unsigned)s.written);
        s.active = false;
        fw_reply(FW_CODE_FLASH_ERROR, 0);
        return;
    }
    s.written += dlc;
    if (s.written == s.total) {
        fw_reply(FW_CODE_UPDATE_SUCCESS, s.total);
    } else if (s.written % ACK_INTERVAL == 0u) {
        fw_reply(FW_CODE_OFFSET, s.written);
    }
}

/* 处理一帧 (仅升级会话内调用); 返回 true = 帧属于升级协议 */
static bool handle_frame(uint32_t id, const uint8_t *data, uint8_t dlc)
{
    if (dlc == 0xFFu) {
        return true; /* 伪帧 (扩展 ID): 已在 can_recv 释放 */
    }
    switch (id) {
    case CAN_ID_CMD:
        if (dlc >= 4u) {
            handle_cmd(data, dlc);
        }
        return true;
    case CAN_ID_DATA:
        handle_data(data, dlc);
        return true;
    case CAN_ID_KEYHASH:
        if (dlc >= 1u) {
            handle_keyhash(data, dlc);
        }
        return true;
    default:
        return false;
    }
}

/* ==================== 等待主循环 ==================== */

static uint32_t now_ms(void)
{
    return HAL_GetTick();
}

void boot_can_wait(bool forever)
{
    uint32_t deadline, next_probe, id;
    uint8_t data[8];
    bool acked = false;

    memset(&s, 0, sizeof(s));
    if (can_init() != 0) {
        boot_log("boot can: init failed");
        return; /* CAN 不可用: 不阻断引导 */
    }
    send_trace(TRACE_HOOK_ENTER);

    /* 探测窗口: forever=false 限时 500ms; forever 持续探测 */
    deadline = now_ms() + PROBE_TIMEOUT_MS;
    next_probe = 0;
    while (!acked) {
        uint32_t now = now_ms();

        watchdog_feed();
        if (!forever && (int32_t)(now - deadline) >= 0) {
            break;
        }
        if ((int32_t)(now - next_probe) >= 0) {
            send_probe();
            next_probe = now + PROBE_INTERVAL_MS;
        }
        {
            uint8_t dlc = can_recv(&id, data);

            if (dlc != 0u && id == CAN_ID_ACK) {
                acked = true;
            }
        }
    }

    if (!acked) {
        send_trace(TRACE_PROCEED);
        can_leave();
        return; /* 无人应答: 正常引导 */
    }

    send_trace(TRACE_HOST_ACK);
    boot_log("boot can: host detected, waiting firmware");
    /* 丢弃 ACK 前滞留的旧帧 (上位机引导重启用的 REBOOT 等) */
    while (can_recv(&id, data) != 0u) {
        watchdog_feed();
    }

    s.last_activity = now_ms();
    while (!s.confirmed) {
        uint32_t now = now_ms();

        watchdog_feed();
        {
            uint8_t dlc = can_recv(&id, data);

            if (dlc != 0u) {
                if (handle_frame(id, data, dlc)) {
                    s.last_activity = now_ms();
                } else if (id == CAN_ID_ACK) {
                    s.last_activity = now_ms();
                }
            }
        }
        if ((int32_t)(now - s.last_activity) >= (int32_t)IDLE_TIMEOUT_MS) {
            boot_log("boot can: idle timeout, booting");
            break;
        }
    }

    if (s.confirmed) {
        send_trace(TRACE_CONFIRMED);
    }
    send_trace(TRACE_PROCEED);
    can_leave();
}

bool boot_rescue_done(void)
{
    return s.installed;
}
