/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * WebSocket 实时通道实现 (Zephyr 版 ws_io.c 的 FreeRTOS 移植):
 *
 * 推送帧 (JSON 文本, web_cmds.c 构造器自带 "t" 标记):
 *   io / regs 1s 周期 (httpd poll 回调驱动), info 10s 周期,
 *   会话建立 ~500ms 后推首帧 (避开 101 尚未收妥的竞态窗口)
 *
 * 命令 ({"cmd":...} 文本帧, 回 {"ok":...} ack 帧):
 *   do / reg / time / cfg / save 与 HTTP POST 共用执行器;
 *   factory_reset = 擦 config + 延迟重启; fw_start / fw_end 与
 *   <binary 帧> 固件数据 — fw_start 的 keyhash (Base64) 先于擦除校验,
 *   数据帧直写 fw_upg (页缓冲内部拆 256B, 10KB 帧 ~20ms), start 的
 *   整槽擦除与 end 的读回校验经 op 队列转 fw 工作任务, 不阻塞 tcpip。
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "lwip/tcp.h"
#include "lwip/tcpip.h"

#include "bootutil/bootutil_public.h"
#include "config_store.h"
#include "fw_keyhash.h"
#include "fw_upg.h"
#include "init.h"
#include "io_hooks.h"
#include "io_time.h"
#include "web_cmds.h"
#include "web_json.h"
#include "ws.h"

#include "log.h"

/* SPA 固件上传分片 10KB + 帧头最宽 14B */
#define WS_RX_MAX  (10u * 1024u + 16u)
#define WS_TX_BUF  768u /* info 帧最大 */
#define WS_PUSH_MS 1000u
#define WS_INFO_MS 10000u

/* ==================== SHA-1 (仅握手; 非安全用途) ==================== */

struct sha1_ctx {
    uint32_t h[5];
    uint64_t total;
    uint8_t buf[64];
    uint32_t buf_len;
};

static void sha1_init(struct sha1_ctx *c)
{
    c->h[0] = 0x67452301u;
    c->h[1] = 0xEFCDAB89u;
    c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u;
    c->h[4] = 0xC3D2E1F0u;
    c->total = 0;
    c->buf_len = 0;
}

#define SHA_ROL(v, n) (((v) << (n)) | ((v) >> (32u - (n))))

static void sha1_block(struct sha1_ctx *c, const uint8_t *p)
{
    uint32_t w[80];
    uint32_t a, b, cc, d, e, k, t;

    for (uint32_t i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
               (uint32_t)p[i * 4 + 2] << 8 | (uint32_t)p[i * 4 + 3];
    }
    for (uint32_t i = 16; i < 80; i++) {
        w[i] = SHA_ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    a = c->h[0];
    b = c->h[1];
    cc = c->h[2];
    d = c->h[3];
    e = c->h[4];
    for (uint32_t i = 0; i < 80; i++) {
        uint32_t g;

        if (i < 20) {
            g = (b & cc) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            g = b ^ cc ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            g = (b & cc) | (b & d) | (cc & d);
            k = 0x8F1BBCDCu;
        } else {
            g = b ^ cc ^ d;
            k = 0xCA62C1D6u;
        }
        t = SHA_ROL(a, 5) + g + e + k + w[i];
        e = d;
        d = cc;
        cc = SHA_ROL(b, 30);
        b = a;
        a = t;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

static void sha1_update(struct sha1_ctx *c, const uint8_t *p, uint32_t len)
{
    c->total += len;
    while (len > 0) {
        uint32_t n = 64u - c->buf_len;

        if (n > len) {
            n = len;
        }
        memcpy(&c->buf[c->buf_len], p, n);
        c->buf_len += n;
        p += n;
        len -= n;
        if (c->buf_len == 64u) {
            sha1_block(c, c->buf);
            c->buf_len = 0;
        }
    }
}

static void sha1_final(struct sha1_ctx *c, uint8_t out[20])
{
    uint8_t tail[8];
    uint64_t bits = c->total << 3;

    sha1_update(c, (const uint8_t *)"\x80", 1);
    while (c->buf_len != 56u) {
        sha1_update(c, (const uint8_t *)"\0", 1);
    }
    for (uint32_t i = 0; i < 8; i++) {
        tail[i] = (uint8_t)(bits >> (56u - 8u * i));
    }
    sha1_update(c, tail, 8);
    for (uint32_t i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)c->h[i];
    }
}

/* ==================== Base64 ==================== */

static const char b64_tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const uint8_t *src, uint32_t len, char *dst)
{
    uint32_t i = 0, o = 0;

    while (i + 3 <= len) {
        uint32_t v = (uint32_t)src[i] << 16 | (uint32_t)src[i + 1] << 8 |
                     src[i + 2];

        dst[o++] = b64_tab[(v >> 18) & 0x3Fu];
        dst[o++] = b64_tab[(v >> 12) & 0x3Fu];
        dst[o++] = b64_tab[(v >> 6) & 0x3Fu];
        dst[o++] = b64_tab[v & 0x3Fu];
        i += 3;
    }
    if (len - i == 1u) {
        uint32_t v = (uint32_t)src[i] << 16;

        dst[o++] = b64_tab[(v >> 18) & 0x3Fu];
        dst[o++] = b64_tab[(v >> 12) & 0x3Fu];
        dst[o++] = '=';
        dst[o++] = '=';
    } else if (len - i == 2u) {
        uint32_t v = (uint32_t)src[i] << 16 | (uint32_t)src[i + 1] << 8;

        dst[o++] = b64_tab[(v >> 18) & 0x3Fu];
        dst[o++] = b64_tab[(v >> 12) & 0x3Fu];
        dst[o++] = b64_tab[(v >> 6) & 0x3Fu];
        dst[o++] = '=';
    }
    return (int)o;
}

static int b64_val(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    if (ch == '+') {
        return 62;
    }
    if (ch == '/') {
        return 63;
    }
    return -1;
}

/* 解码 (忽略空白与 '='), 返回字节数, 非法字符/溢出返回 -1 */
static int b64_decode(const char *src, uint32_t len, uint8_t *dst,
		      uint32_t dst_max)
{
    uint32_t acc = 0, nbits = 0, o = 0;

    for (uint32_t i = 0; i < len; i++) {
        int v = b64_val(src[i]);

        if (v < 0) {
            if (src[i] == '=' || src[i] == ' ' || src[i] == '\r' ||
                src[i] == '\n') {
                continue;
            }
            return -1;
        }
        acc = acc << 6 | (uint32_t)v;
        nbits += 6;
        if (nbits >= 8u) {
            nbits -= 8u;
            if (o >= dst_max) {
                return -1;
            }
            dst[o++] = (uint8_t)(acc >> nbits);
        }
    }
    return (int)o;
}

/* ==================== CRC16 (fw 数据帧累加) ==================== */

/* Zephyr crc16_ccitt 逐式 (反射 nibble, poly 0x8408 --
 * zephyr/subsys/crc/crc16_sw.c)。须与 fw_upg.c 内部累加一致:
 * fw_end 传入的接收侧 CRC 与固件侧读回 CRC 对比, 算法不一致则
 * 恒 mismatch (41372ae 对齐 fw_upg.c 时本函数漏改, 致 WS 升级失败) */
static uint16_t ws_crc16(uint16_t seed, const uint8_t *src, uint32_t len)
{
    while (len-- > 0) {
        uint8_t e = (uint8_t)(seed ^ *src++);
        uint8_t f = (uint8_t)(e ^ (e << 4));
        seed = (uint16_t)((seed >> 8) ^ ((uint16_t)f << 8) ^
                          ((uint16_t)f << 3) ^ (f >> 4));
    }
    return seed;
}

/* ==================== 会话状态 ==================== */

/* 帧解析状态机 */
enum ws_rx_state {
    WS_HDR,     /* 2B 基础头 */
    WS_LEN16,   /* 126 扩展长度 */
    WS_LEN64,   /* 127 扩展长度 */
    WS_MASK,    /* 4B 掩码 */
    WS_PAYLOAD, /* 载荷累积 (边收边解掩码) */
};

static struct {
    bool active;
    struct tcp_pcb *pcb;
    ws_close_cb_t on_close;

    /* rx 状态机 */
    uint8_t state;
    uint8_t hdr[10]; /* 长度/掩码暂存 */
    uint8_t hdr_got;
    uint8_t op;
    bool fin;
    bool masked;
    uint8_t mask[4];
    uint32_t plen;
    uint32_t got;
    uint8_t payload[WS_RX_MAX];

    /* fw 数据帧 CRC 累积 (tcpip 线程; fw_end 入队时快照) */
    uint16_t fw_crc;

    /* 推送节律 */
    uint32_t t_push;
    uint32_t t_info;

    /* 发送缓冲 (ack/推送共用, 均 tcpip 线程) */
    char tx[WS_TX_BUF];
} ws;

static uint32_t ws_now_ms(void)
{
    return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}

static void ws_close_session(void);

/* ==================== 发送 (tcpip 线程) ==================== */

/* 服务器帧不掩码; opcode: 0x1 文本 / 0x8 close / 0xA pong */
static void ws_send(uint8_t opcode, const uint8_t *payload, uint32_t len)
{
    uint8_t hdr[10];
    uint32_t hl = 0;

    if (!ws.active || ws.pcb == NULL) {
        return;
    }
    hdr[hl++] = (uint8_t)(0x80u | opcode); /* FIN */
    if (len < 126u) {
        hdr[hl++] = (uint8_t)len;
    } else if (len <= 0xFFFFu) {
        hdr[hl++] = 126u;
        hdr[hl++] = (uint8_t)(len >> 8);
        hdr[hl++] = (uint8_t)len;
    } else {
        hdr[hl++] = 127u;
        for (uint32_t i = 0; i < 8; i++) {
            hdr[hl++] = (uint8_t)(len >> (56u - 8u * i));
        }
    }

    u16_t snd = tcp_sndbuf(ws.pcb);

    if (snd < hl + len) {
        return; /* 窗口不足: 丢弃 (推送下周期重试) */
    }
    if (tcp_write(ws.pcb, hdr, (u16_t)hl, TCP_WRITE_FLAG_COPY) == ERR_OK &&
        (len == 0u ||
         tcp_write(ws.pcb, payload, (u16_t)len, TCP_WRITE_FLAG_COPY) ==
             ERR_OK)) {
        tcp_output(ws.pcb);
    }
}

static void ws_send_json(const char *json)
{
    ws_send(0x1u, (const uint8_t *)json, (uint32_t)strlen(json));
}

/* 工作任务应答回执 (tcpip_callback 桥) */
struct ws_fw_reply {
    char json[80];
};

static struct ws_fw_reply fw_replies[2];
static volatile uint8_t fw_reply_idx;

static void ws_fw_reply_cb(void *arg)
{
    ws_send_json(((struct ws_fw_reply *)arg)->json);
}

/* 仅工作任务上下文调用 */
static void fw_reply_from_worker(const char *json)
{
    uint8_t i = fw_reply_idx++ & 1u;

    strncpy(fw_replies[i].json, json, sizeof(fw_replies[i].json) - 1u);
    fw_replies[i].json[sizeof(fw_replies[i].json) - 1u] = '\0';
    (void)tcpip_callback(ws_fw_reply_cb, &fw_replies[i]);
}

/* ==================== fw start/end 工作任务 ==================== */

enum { WS_FW_OP_START = 0, WS_FW_OP_END = 1 };

struct ws_fw_op {
    uint8_t kind;
    uint32_t total;
    bool has_kh;
    uint8_t kh[FW_KEYHASH_LEN];
    uint16_t crc; /* END: tcpip 线程快照的接收侧 CRC */
};

static QueueHandle_t fw_op_q;
static StaticQueue_t fw_op_q_cb;
static uint8_t fw_op_q_buf[4 * sizeof(struct ws_fw_op)];

static StackType_t fw_stack[512];
static StaticTask_t fw_tcb;

static void ws_fw_task(void *arg)
{
    struct ws_fw_op op;

    (void)arg;
    for (;;) {
        if (xQueueReceive(fw_op_q, &op, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (op.kind == WS_FW_OP_START) {
            int rc = fw_upg_start(op.total, op.has_kh ? op.kh : NULL);

            if (rc == 0) {
                LOG_INF("ws fw: start size=%u", (unsigned)op.total);
                fw_reply_from_worker("{\"ok\":true}");
            } else if (rc == -2) {
                fw_reply_from_worker(
                    "{\"ok\":false,\"err\":\"keyhash mismatch\"}");
            } else if (rc == -3) {
                fw_reply_from_worker(
                    "{\"ok\":false,\"err\":\"already in progress\"}");
            } else {
                fw_reply_from_worker(
                    "{\"ok\":false,\"err\":\"erase/init\"}");
            }
        } else {
            /* END: 尺寸预检 -> CRC/TLV 读回校验 -> 请求换机 */
            char rep[80];
            uint32_t got = fw_upg_received();

            if (got == 0u) {
                fw_upg_abort(); /* 预检失败也要复位会话 (Zephyr 语义) */
                snprintf(rep, sizeof(rep),
                         "{\"ok\":false,\"err\":\"no data\"}");
            } else if (got != fw_upg_total()) {
                fw_upg_abort();
                LOG_WRN("ws fw: size mismatch recv=%u", (unsigned)got);
                snprintf(rep, sizeof(rep),
                         "{\"ok\":false,\"err\":\"size mismatch\"}");
            } else if (fw_upg_finish(op.crc) != 0) {
                snprintf(rep, sizeof(rep),
                         "{\"ok\":false,\"err\":\"crc mismatch\"}");
            } else if (boot_set_pending(1) != 0) {
                snprintf(rep, sizeof(rep),
                         "{\"ok\":false,\"err\":\"boot_request\"}");
            } else {
                LOG_INF("ws fw: verified, rebooting for swap");
                fw_reply_from_worker("{\"ok\":true}");
                vTaskDelay(pdMS_TO_TICKS(300));
                set_reboot_status(true);
                continue;
            }
            fw_reply_from_worker(rep);
        }
    }
}

/* ==================== 命令处理 (tcpip 线程) ==================== */

static void ws_handle_cmd(const char *cmd, size_t len)
{
    int32_t index = 0, addr = 0, value = 0, ts = 0, fw_size = 0;

    /* 快命令: 与 HTTP POST 共用执行器, 直接回 ack */
    if (strncmp(cmd, "\"do\"", 4) == 0 &&
        json_get_i32(cmd, len, "index", &index) &&
        json_get_i32(cmd, len, "value", &value)) {
        if (web_cmd_exec_do(index, value) == 0) {
            /* do 命令回改变后的完整 IO 快照 (Zephyr 语义) */
            int n = web_build_io_json(ws.tx, sizeof(ws.tx));

            ws_send(0x1u, (const uint8_t *)ws.tx, (uint32_t)n);
        } else {
            ws_send_json("{\"ok\":false,\"err\":\"bad index\"}");
        }
        return;
    }
    if (strncmp(cmd, "\"reg\"", 5) == 0 &&
        json_get_i32(cmd, len, "addr", &addr) &&
        json_get_i32(cmd, len, "value", &value)) {
        snprintf(ws.tx, sizeof(ws.tx), "{\"ok\":%s}",
                 web_cmd_exec_reg(addr, value) == 0 ? "true" : "false");
        ws_send_json(ws.tx);
        return;
    }
    if (strncmp(cmd, "\"time\"", 6) == 0 &&
        json_get_i32(cmd, len, "ts", &ts)) {
        snprintf(ws.tx, sizeof(ws.tx), "{\"ok\":%s}",
                 set_timestamp((time_t)ts) ? "true" : "false");
        ws_send_json(ws.tx);
        return;
    }
    if (strncmp(cmd, "\"cfg\"", 5) == 0) {
        const char *err = "invalid";

        if (web_cmd_exec_cfg(cmd, len, &err) == 0) {
            ws_send_json("{\"ok\":true}");
        } else {
            snprintf(ws.tx, sizeof(ws.tx),
                     "{\"ok\":false,\"err\":\"%s\"}", err);
            ws_send_json(ws.tx);
        }
        return;
    }
    if (strncmp(cmd, "\"save\"", 6) == 0) {
        holding_reg_save();
        ws_send_json("{\"ok\":true}");
        return;
    }
    if (strncmp(cmd, "\"factory_reset\"", 15) == 0) {
        config_store_erase_all();
        LOG_INF("factory reset via ws, rebooting");
        ws_send_json("{\"ok\":true}");
        set_reboot_status(true);
        return;
    }
    if (strncmp(cmd, "\"fw_start\"", 10) == 0) {
        struct ws_fw_op op;

        (void)json_get_i32(cmd, len, "size", &fw_size);
        if (fw_size <= 0) {
            ws_send_json("{\"ok\":false,\"err\":\"bad size\"}");
            return;
        }
        memset(&op, 0, sizeof(op));
        op.kind = WS_FW_OP_START;
        op.total = (uint32_t)fw_size;

        /* keyhash (Base64, 可选): 擦除前校验, 长度/内容不符即拒绝 */
        {
            char b64[64];

            if (json_get_str(cmd, len, "keyhash", b64, sizeof(b64))) {
                int dlen = b64_decode(b64, (uint32_t)strlen(b64), op.kh,
                                      FW_KEYHASH_LEN);

                if (dlen == FW_KEYHASH_LEN) {
                    op.has_kh = true;
                } else {
                    ws_send_json(
                        "{\"ok\":false,\"err\":\"keyhash mismatch\"}");
                    return;
                }
            }
        }
        if (fw_op_q == NULL || xQueueSend(fw_op_q, &op, 0) != pdTRUE) {
            ws_send_json("{\"ok\":false,\"err\":\"busy\"}");
        }
        return; /* 应答由工作任务回 */
    }
    if (strncmp(cmd, "\"fw_end\"", 8) == 0) {
        struct ws_fw_op op;

        if (!fw_upg_active()) {
            ws_send_json("{\"ok\":false,\"err\":\"not in progress\"}");
            return;
        }
        memset(&op, 0, sizeof(op));
        op.kind = WS_FW_OP_END;
        op.crc = ws.fw_crc;
        if (fw_op_q == NULL || xQueueSend(fw_op_q, &op, 0) != pdTRUE) {
            ws_send_json("{\"ok\":false,\"err\":\"busy\"}");
        }
        return;
    }

    ws_send_json("{\"ok\":false,\"err\":\"unknown cmd\"}");
}

/* ==================== 帧分发 (tcpip 线程) ==================== */

static void ws_frame_done(void)
{
    if (!ws.fin) {
        ws_send(0x8u, NULL, 0); /* 分片帧不支持 */
        ws_close_session();
        return;
    }
    switch (ws.op) {
    case 0x1u: { /* text: {"cmd":...} */
        if (ws.got < sizeof(ws.payload)) {
            const char *cmd;

            ws.payload[ws.got] = '\0';
            cmd = json_find_value((const char *)ws.payload, ws.got, "cmd");
            if (cmd != NULL) {
                ws_handle_cmd(cmd,
                              ws.got -
                                  (size_t)(cmd - (const char *)ws.payload));
            }
        }
        break;
    }
    case 0x2u: /* binary: 固件数据帧直写 (fw_upg 内部页缓冲) */
        if (fw_upg_active()) {
            if (fw_upg_write(ws.payload, ws.got) == 0) {
                ws.fw_crc = ws_crc16(ws.fw_crc, ws.payload, ws.got);
            } else {
                LOG_ERR("ws fw: write failed @%u",
                        (unsigned)fw_upg_received());
            }
        }
        break;
    case 0x8u: /* close: 回 close 后断开 */
        ws_send(0x8u, NULL, 0);
        ws_close_session();
        return;
    case 0x9u: /* ping -> pong */
        ws_send(0xAu, ws.payload, ws.got);
        break;
    case 0xAu: /* pong: 忽略 */
        break;
    default:
        ws_close_session();
        return;
    }
}

/* ==================== rx 状态机 (tcpip 线程) ==================== */

void ws_feed(const uint8_t *data, uint16_t len)
{
    if (!ws.active) {
        return;
    }
    while (len > 0) {
        uint8_t b = *data++;

        len--;
        switch (ws.state) {
        case WS_HDR:
            ws.hdr[ws.hdr_got++] = b;
            if (ws.hdr_got < 2u) {
                break;
            }
            ws.fin = (ws.hdr[0] & 0x80u) != 0u;
            ws.op = ws.hdr[0] & 0x0Fu;
            ws.masked = (ws.hdr[1] & 0x80u) != 0u;
            ws.plen = ws.hdr[1] & 0x7Fu;
            ws.got = 0;
            ws.hdr_got = 0;
            if (ws.plen == 126u) {
                ws.state = WS_LEN16;
            } else if (ws.plen == 127u) {
                ws.state = WS_LEN64;
            } else if (ws.plen > sizeof(ws.payload)) {
                ws_close_session();
                return;
            } else {
                ws.state = ws.masked ? WS_MASK : WS_PAYLOAD;
            }
            break;
        case WS_LEN16:
            ws.hdr[ws.hdr_got++] = b;
            if (ws.hdr_got == 2u) {
                ws.plen = (uint32_t)ws.hdr[0] << 8 | ws.hdr[1];
                if (ws.plen > sizeof(ws.payload)) {
                    ws_close_session();
                    return;
                }
                ws.hdr_got = 0;
                ws.state = ws.masked ? WS_MASK : WS_PAYLOAD;
            }
            break;
        case WS_LEN64: {
            ws.hdr[ws.hdr_got++] = b;
            if (ws.hdr_got == 8u) {
                uint64_t v = 0;

                for (uint32_t i = 0; i < 8; i++) {
                    v = v << 8 | ws.hdr[i];
                }
                if (v > sizeof(ws.payload)) {
                    ws_close_session();
                    return;
                }
                ws.plen = (uint32_t)v;
                ws.hdr_got = 0;
                ws.state = ws.masked ? WS_MASK : WS_PAYLOAD;
            }
            break;
        }
        case WS_MASK:
            ws.mask[ws.hdr_got++] = b;
            if (ws.hdr_got == 4u) {
                ws.hdr_got = 0;
                ws.state = WS_PAYLOAD;
            }
            break;
        case WS_PAYLOAD: {
            uint32_t idx = ws.got;

            if (idx >= sizeof(ws.payload)) {
                ws_close_session();
                return;
            }
            ws.payload[idx] =
                ws.masked ? (uint8_t)(b ^ ws.mask[idx & 3u]) : b;
            ws.got = idx + 1u;
            if (ws.got == ws.plen) {
                ws_frame_done();
                if (!ws.active) {
                    return;
                }
                ws.state = WS_HDR;
                ws.hdr_got = 0;
            }
            break;
        }
        default:
            ws.state = WS_HDR;
            ws.hdr_got = 0;
            break;
        }
    }
}

/* ==================== 推送 (httpd poll 回调, tcpip 线程) ==================== */

void ws_poll(void)
{
    uint32_t now = ws_now_ms();

    if (!ws.active) {
        return;
    }
    if (now - ws.t_push >= WS_PUSH_MS) {
        int n = web_build_io_json(ws.tx, sizeof(ws.tx));

        ws_send(0x1u, (const uint8_t *)ws.tx, (uint32_t)n);
        n = web_build_regs_json(ws.tx, sizeof(ws.tx));
        ws_send(0x1u, (const uint8_t *)ws.tx, (uint32_t)n);
        ws.t_push = now;
    }
    if (now - ws.t_info >= WS_INFO_MS) {
        int n = web_build_info_json(ws.tx, sizeof(ws.tx));

        ws_send(0x1u, (const uint8_t *)ws.tx, (uint32_t)n);
        ws.t_info = now;
    }
}

/* ==================== 会话生命周期 ==================== */

bool ws_active(void)
{
    return ws.active;
}

static void ws_close_session(void)
{
    ws_close_cb_t cb;

    if (!ws.active) {
        return;
    }
    ws.active = false;
    ws.pcb = NULL;
    cb = ws.on_close;
    ws.on_close = NULL;
    LOG_INF("ws: closed");
    if (cb != NULL) {
        cb(); /* httpd 执行 conn_close (tcpip 线程上下文) */
    }
}

void ws_detach(void)
{
    ws.active = false;
    ws.pcb = NULL;
    ws.on_close = NULL;
}

void ws_attach(struct tcp_pcb *pcb, const uint8_t *pending, uint16_t len,
	       ws_close_cb_t on_close)
{
    if (ws.active) {
        return; /* 单连接限制: httpd 在握手时已拒绝 */
    }
    ws.active = true;
    ws.pcb = pcb;
    ws.on_close = on_close;
    ws.state = WS_HDR;
    ws.hdr_got = 0;
    ws.got = 0;
    ws.fw_crc = 0;
    ws.t_push = ws_now_ms() - WS_PUSH_MS + 500u; /* ~500ms 后首帧 */
    ws.t_info = ws_now_ms() - WS_INFO_MS + 500u;
    LOG_INF("ws: connected (%s:%u)", ipaddr_ntoa(&pcb->remote_ip),
            (unsigned)pcb->remote_port);
    if (pending != NULL && len > 0) {
        ws_feed(pending, len);
    }
}

/* ==================== 握手 ==================== */

bool ws_handshake(const char *req_hdr, uint16_t hdr_len,
		  char *resp, uint16_t resp_max, uint16_t *resp_len)
{
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const char *key = NULL;
    const size_t klen = strlen("Sec-WebSocket-Key:");

    for (uint16_t i = 0; i + klen < hdr_len; i++) {
        if (strncasecmp(&req_hdr[i], "Sec-WebSocket-Key:", klen) == 0) {
            key = &req_hdr[i + klen];
            while (*key == ' ') {
                key++;
            }
            break;
        }
    }
    if (key == NULL) {
        return false;
    }

    {
        struct sha1_ctx sc;
        uint8_t digest[20];
        char accept[32];
        int alen;
        int n;

        sha1_init(&sc);
        sha1_update(&sc, (const uint8_t *)key, 24);
        sha1_update(&sc, (const uint8_t *)guid, sizeof(guid) - 1u);
        sha1_final(&sc, digest);
        alen = b64_encode(digest, 20, accept);
        if (alen != 28) {
            return false;
        }
        accept[28] = '\0';
        n = snprintf(resp, resp_max,
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n",
                     accept);
        if (n <= 0 || n >= (int)resp_max) {
            return false;
        }
        *resp_len = (uint16_t)n;
        return true;
    }
}

void ws_init(void)
{
    fw_op_q = xQueueCreateStatic(4, sizeof(struct ws_fw_op),
                                 fw_op_q_buf, &fw_op_q_cb);
    xTaskCreateStatic(ws_fw_task, "wsfw", 512, NULL, 3, fw_stack, &fw_tcb);
}
