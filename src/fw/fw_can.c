/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN 固件升级通道实现 (Zephyr libs/can_fw_upgrade/can_fw_upgrade.c
 * app 域部分的逐式移植):
 *   - RX: can.c ISR 读帧入队 (深度 32, 对齐 Zephyr msgq: keyhash 5 帧 +
 *     START 在 ~3ms 内连发), 本任务消费; 0x101/0x103/0x104 内部处理,
 *     其余帧交业务回调 (现无注册者, 静默丢弃 + 计数)
 *   - 协议语义对齐: START 无条件重开会话 (重擦, 兼容上次失败残留)、
 *     keyhash 仅在 5 帧到齐时校验 (老上位机不发 0x104 放行)、
 *     CONFIRM 无 CRC (镜像完整性由 MCUboot 验签兜底)、REBOOT 不应答。
 *     流控窗口 512B 对齐 Zephyr CAN_FW_OFFSET_REPLY_BYTES。
 */

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "main.h" /* NVIC_SystemReset */

#include "bootutil/bootutil_public.h"
#include "fw_can.h"
#include "fw_upg.h"
#include "fw_version.h"
#include "history.h" /* history_sync (升级重启前刷采样缓存) */
#include "io_can.h"
#include "log.h"

#define CAN_FW_PLATFORM_RX 0x101u
#define CAN_FW_PLATFORM_TX 0x102u
#define CAN_FW_FW_DATA_RX 0x103u
#define CAN_FW_KEYHASH_RX 0x104u
#define CAN_FW_VERSION_TX 0x105u

/* 命令码 (0x101 data LE32) */
enum {
    FW_CMD_START_UPDATE = 0,
    FW_CMD_CONFIRM = 1,
    FW_CMD_VERSION = 2,
    FW_CMD_REBOOT = 3,
};

/* 响应码 (0x102 data LE32) */
enum {
    FW_CODE_OFFSET = 0,
    FW_CODE_UPDATE_SUCCESS = 1,
    FW_CODE_VERSION = 2,
    FW_CODE_CONFIRM = 3,
    FW_CODE_FLASH_ERROR = 4,
    FW_CODE_TRANSFER_ERROR = 5,
    FW_CODE_KEYHASH_ERROR = 6,
};

#define FW_CODE_CONFIRM_MAGIC 0x55AA55AAu

/* keyhash 分帧: 1B seq + 7B chunk, 32B -> 5 帧 (末帧 4B) */
#define KEYHASH_CHUNK 7u
#define KEYHASH_CHUNKS ((FW_KEYHASH_LEN + KEYHASH_CHUNK - 1u) / KEYHASH_CHUNK)
#define KEYHASH_FULL_MASK ((1u << KEYHASH_CHUNKS) - 1u)

#define FW_Q_DEPTH 32u
#define ACK_INTERVAL 512u /* 对齐 Zephyr CAN_FW_OFFSET_REPLY_BYTES */

struct can_fw_msg {
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];
};

static QueueHandle_t fw_q;
static StaticQueue_t fw_q_cb;
static uint8_t fw_q_buf[FW_Q_DEPTH * sizeof(struct can_fw_msg)];

static StackType_t fw_stack[512];
static StaticTask_t fw_tcb;

/* 上位机 keyhash 累积 (仅 fw 任务上下文访问) */
static uint8_t rx_keybuf[FW_KEYHASH_LEN];
static uint8_t key_chunk_mask;

/* ==================== 应答 ==================== */

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
    (void)mod_can_send(CAN_FW_PLATFORM_TX, d, 8);
}

/* 版本字符串分帧 (0x105): [seq][<=7B 文本], 末帧 '\0' 填充 */
static void fw_send_version_string(const char *ver, uint8_t len)
{
    for (uint8_t off = 0, seq = 0; off < len; off += 7u, seq++) {
        uint8_t chunk = (uint8_t)(len - off);

        if (chunk > 7u) {
            chunk = 7u;
        }
        {
            uint8_t d[8];

            d[0] = seq;
            memcpy(&d[1], &ver[off], chunk);
            memset(&d[1 + chunk], 0, 7u - chunk);
            (void)mod_can_send(CAN_FW_VERSION_TX, d, 8);
        }
    }
}

/* ==================== 0x101 命令帧 ==================== */

static void handle_platform_rx(const uint8_t *data, uint8_t dlc)
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
    case FW_CMD_START_UPDATE: {
        /* 无条件重开: 上次中途失败的传输若不复位, 旧偏移/缓冲
         * 会导致 flash 写错位 (对齐 Zephyr 语义) */
        const uint8_t *kh = NULL;
        int rc;

        fw_upg_abort();
        if ((key_chunk_mask & KEYHASH_FULL_MASK) == KEYHASH_FULL_MASK) {
            key_chunk_mask = 0;
            kh = rx_keybuf;
        }
        rc = fw_upg_start(arg, kh);
        if (rc == -2) {
            LOG_WRN("fwcan: keyhash mismatch");
            fw_reply(FW_CODE_KEYHASH_ERROR, 0);
            return;
        }
        if (rc != 0) {
            LOG_WRN("fwcan: start rc=%d size=%u", rc, (unsigned)arg);
            fw_reply(FW_CODE_FLASH_ERROR, 0);
            return;
        }

        /* 丢弃擦除期间堆积的旧数据帧 */
        {
            struct can_fw_msg stale;

            while (xQueueReceive(fw_q, &stale, 0) == pdTRUE) {
            }
        }
        LOG_INF("fwcan: start size=%u", (unsigned)arg);
        fw_reply(FW_CODE_OFFSET, 0);
        break;
    }

    case FW_CMD_CONFIRM: {
        if (!fw_upg_active()) {
            LOG_WRN("fwcan: confirm before start");
            fw_reply(FW_CODE_TRANSFER_ERROR, 0);
            return;
        }
        {
            struct can_fw_msg stale;
            while (xQueueReceive(fw_q, &stale, 0) == pdTRUE) {
            }
        }
        if (fw_upg_finish_ex(0, false) != 0) {
            LOG_WRN("fwcan: confirm verify failed");
            fw_reply(FW_CODE_TRANSFER_ERROR, 0);
            return;
        }
        if (boot_set_pending(arg != 0u) != 0) {
            LOG_ERR("fwcan: boot_set_pending failed");
            fw_reply(FW_CODE_TRANSFER_ERROR, 0);
            return;
        }
        LOG_INF("fwcan: confirmed (permanent=%u), waiting for reboot",
                (unsigned)arg);
        fw_reply(FW_CODE_CONFIRM, FW_CODE_CONFIRM_MAGIC);
        break;
    }

    case FW_CMD_VERSION: {
        char ver[24];
        int len = snprintf(ver, sizeof(ver), "v%d.%d.%d_%s",
                           FW_VERSION_MAJOR, FW_VERSION_MINOR,
                           FW_VERSION_PATCH, FW_GIT_VERSION);

        if (len < 0 || len > 63) {
            len = 0;
        }
        fw_reply(FW_CODE_VERSION, (uint32_t)len);
        fw_send_version_string(ver, (uint8_t)len);
        break;
    }

    case FW_CMD_REBOOT:
        /* 对齐 Zephyr: 不应答, 短延迟后排空期重启 */
        LOG_INF("fwcan: reboot requested");
        vTaskDelay(pdMS_TO_TICKS(100));
        log_flush(500); /* 复位前把异步日志刷出 */
        history_sync(); /* 升级重启前刷采样缓存 (与 ws/udp/shell 路径一致) */
        NVIC_SystemReset();
        break;

    default:
        break;
    }
}

/* ==================== 0x103 / 0x104 ==================== */

static void handle_fw_data(const uint8_t *data, uint8_t dlc)
{
    uint32_t got;

    if (!fw_upg_active()) {
        LOG_WRN("fwcan: data before start");
        fw_reply(FW_CODE_TRANSFER_ERROR, 0);
        return;
    }
    if (fw_upg_write(data, dlc) != 0) {
        LOG_ERR("fwcan: write failed @%u", (unsigned)fw_upg_received());
        fw_reply(FW_CODE_FLASH_ERROR, 0);
        return;
    }

    got = fw_upg_received();
    if (got == fw_upg_total()) {
        fw_reply(FW_CODE_UPDATE_SUCCESS, got);
    } else if (got % ACK_INTERVAL == 0u) {
        fw_reply(FW_CODE_OFFSET, got);
    }
}

static void handle_keyhash(const uint8_t *data, uint8_t dlc)
{
    uint8_t seq = data[0];
    uint8_t rem = FW_KEYHASH_LEN - seq * KEYHASH_CHUNK;
    uint8_t chunk = rem < KEYHASH_CHUNK ? rem : KEYHASH_CHUNK;

    if (seq >= KEYHASH_CHUNKS || dlc < 1u + chunk) {
        LOG_WRN("fwcan: keyhash frame invalid seq=%u dlc=%u",
                (unsigned)seq, (unsigned)dlc);
        return;
    }
    memcpy(&rx_keybuf[seq * KEYHASH_CHUNK], &data[1], chunk);
    key_chunk_mask |= (uint8_t)(1u << seq);
}

/* ==================== RX 任务 ==================== */

static void fw_task(void *arg)
{
    struct can_fw_msg m;

    (void)arg;
    for (;;) {
        if (xQueueReceive(fw_q, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (m.id) {
        case CAN_FW_PLATFORM_RX:
            handle_platform_rx(m.data, m.dlc);
            break;
        case CAN_FW_FW_DATA_RX:
            handle_fw_data(m.data, m.dlc);
            break;
        case CAN_FW_KEYHASH_RX:
            handle_keyhash(m.data, m.dlc);
            break;
        default:
            /* 业务帧: 现无业务处理 (原 can.c 静默消费语义), 丢弃 */
            break;
        }
    }
}

/* ==================== 入口 ==================== */

void fw_can_frame_isr(uint32_t id, const uint8_t *data, uint8_t dlc)
{
    BaseType_t woken = pdFALSE;

    if (fw_q != NULL && dlc <= 8u) {
        struct can_fw_msg m;

        m.id = id;
        m.dlc = dlc;
        memcpy(m.data, data, dlc);
        (void)xQueueSendFromISR(fw_q, &m, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

void fw_can_start(void)
{
    fw_q = xQueueCreateStatic(FW_Q_DEPTH, sizeof(struct can_fw_msg),
                              fw_q_buf, &fw_q_cb);
    xTaskCreateStatic(fw_task, "fwc", 512, NULL, 3, fw_stack, &fw_tcb);
    LOG_INF("fwcan: rx task up");
}
