/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 固件升级核心实现。CRC16-CCITT 为 Zephyr sys/crc.h crc16_ccitt
 * 逐式移植 (反射, poly 0x1021, init 0), 与上位机 firmware_upgrade.py
 * 一致; 终检以"读回 slot1 实际内容"计算 (兼验证编程结果)。
 * TLV 解析对齐 Zephyr ws_io.c fw_upg_verify_keyhash。
 */

#include <string.h>

#include "fw_upg.h"
#include "flash_layout.h"
#include "fw_keyhash.h"
#include "io_flash.h"
#include "w25qxx.h"

#ifdef FW_UPG_FW
#include "FreeRTOS.h"
#include "semphr.h"
#endif

#define PAGE_SZ 256u
#define READ_CHUNK 64u

static const struct io_flash *iof(void)
{
	return w25qxx_flash();
}

/* ==================== 会话状态 ==================== */

static struct {
	bool active;
	bool failed;
	uint32_t total;
	uint32_t written; /* 已整页落盘字节数 */
	uint8_t page[PAGE_SZ];
	uint16_t page_len;
} s;

#ifdef FW_UPG_FW
static StaticSemaphore_t lock_cb;
static SemaphoreHandle_t lock;

static void lock_take(void)
{
	if (lock != NULL) {
		(void)xSemaphoreTake(lock, portMAX_DELAY);
	}
}

static void lock_give(void)
{
	if (lock != NULL) {
		(void)xSemaphoreGive(lock);
	}
}

void fw_upg_os_init(void)
{
	lock = xSemaphoreCreateMutexStatic(&lock_cb);
}
#else
static void lock_take(void) {}
static void lock_give(void) {}
#endif

/* ==================== CRC16-CCITT (Zephyr crc16_ccitt 逐式) ==================== */

static uint16_t crc16_ccitt(uint16_t seed, const uint8_t *src, uint32_t len)
{
	while (len-- > 0) {
		seed = (uint16_t)((seed >> 8) | (seed << 8));
		seed ^= *src++;
		seed ^= (uint16_t)((seed & 0xFFu) >> 4);
		seed ^= (uint16_t)(seed << 12);
		seed ^= (uint16_t)((seed & 0xFFu) << 5);
	}
	return seed;
}

/* ==================== MCUboot TLV keyhash 解析 ==================== */

static int tlv_keyhash_get(uint8_t *out)
{
	uint8_t hdr[32];
	uint32_t magic, img_size, tlv_off, tlv_end;
	uint16_t hdr_size, tlv_magic, tlv_size;

	if (iof()->read(SLOT1_OFFSET, hdr, sizeof(hdr)) != 0) {
		return -1;
	}
	memcpy(&magic, hdr, 4);
	if (magic != 0x96F3B83Du) {
		return -1;
	}
	memcpy(&hdr_size, &hdr[8], 2);
	memcpy(&img_size, &hdr[12], 4);
	if (hdr_size != IMG_HDR_SIZE) {
		return -1;
	}
	tlv_off = (uint32_t)hdr_size + img_size;
	if (tlv_off + 4 > SLOT1_SIZE || (tlv_off & 3u) != 0) {
		return -1;
	}

	{
		uint8_t info[4];

		if (iof()->read(SLOT1_OFFSET + tlv_off, info, 4) != 0) {
			return -1;
		}
		memcpy(&tlv_magic, &info[0], 2);
		memcpy(&tlv_size, &info[2], 2);
		if (tlv_magic != 0x6907u || tlv_size == 0 ||
		    tlv_off + tlv_size > SLOT1_SIZE) {
			return -1;
		}
		tlv_end = tlv_off + tlv_size;
	}

	for (uint32_t off = tlv_off + 4; off + 4 <= tlv_end;) {
		uint8_t ent[4];
		uint16_t tag, len;

		if (iof()->read(SLOT1_OFFSET + off, ent, 4) != 0) {
			return -1;
		}
		memcpy(&tag, &ent[0], 2);
		memcpy(&len, &ent[2], 2);
		if (tag == 0x0001u && len == FW_KEYHASH_LEN) {
			return iof()->read(SLOT1_OFFSET + off + 4, out,
					   FW_KEYHASH_LEN);
		}
		off += 4u + (((uint32_t)len + 3u) & ~3u);
	}
	return -1;
}

/* ==================== 页缓冲写 ==================== */

static int page_flush(void)
{
	int rc = 0;

	if (s.page_len > 0) {
		rc = iof()->write(SLOT1_OFFSET + s.written, s.page, s.page_len);
		s.page_len = 0;
	}
	return rc;
}

/* ==================== 对外 API ==================== */

int fw_upg_start(uint32_t total, const uint8_t *keyhash)
{
	lock_take();
	if (s.active) {
		lock_give();
		return -3;
	}
	if (total < 64u || total > SLOT1_SIZE) {
		lock_give();
		return -1;
	}
	if (keyhash != NULL &&
	    memcmp(keyhash, fw_keyhash, FW_KEYHASH_LEN) != 0) {
		lock_give();
		return -2;
	}

	/* 整槽擦除: trailer/magic 位于 slot 末尾 (boot_set_next 不自擦),
	 * 仅按 total 擦会留下脏 trailer 区导致升级请求写入失败。
	 * w25qxx_erase 对 64K 对齐段用块擦命令, 448KB 整槽 ~1s */
	if (iof()->erase(SLOT1_OFFSET, SLOT1_SIZE) != 0) {
		lock_give();
		return -1;
	}

	s.active = true;
	s.failed = false;
	s.total = total;
	s.written = 0;
	s.page_len = 0;
	lock_give();
	return 0;
}

int fw_upg_write(const uint8_t *data, uint32_t len)
{
	int rc = 0;

	lock_take();
	if (!s.active) {
		lock_give();
		return -1;
	}
	if (s.failed) {
		lock_give();
		return -1;
	}
	if (s.written + s.page_len + len > s.total) {
		s.failed = true;
		lock_give();
		return -1;
	}

	while (len > 0 && rc == 0) {
		uint32_t chunk = PAGE_SZ - s.page_len;

		if (chunk > len) {
			chunk = len;
		}
		memcpy(&s.page[s.page_len], data, chunk);
		s.page_len += (uint16_t)chunk;
		data += chunk;
		len -= chunk;

		if (s.page_len == PAGE_SZ) {
			rc = page_flush();
			s.written += PAGE_SZ;
		}
	}
	if (rc != 0) {
		s.failed = true;
	}
	lock_give();
	return rc;
}

void fw_upg_abort(void)
{
	lock_take();
	s.active = false;
	s.failed = false;
	s.page_len = 0;
	s.written = 0;
	lock_give();
}

int fw_upg_finish(uint16_t crc_expect)
{
    return fw_upg_finish_ex(crc_expect, true);
}

int fw_upg_finish_ex(uint16_t crc_expect, bool check_crc)
{
    uint16_t tail;
    int rc = -1;

    lock_take();
    if (!s.active) {
        lock_give();
        return -1;
    }
    s.active = false;

    tail = s.page_len;
    if (s.failed || page_flush() != 0) {
        goto out;
    }
    s.written += tail;

    if (s.written != s.total) {
        goto out;
    }

    /* 读回 CRC 校验 (64B 块累加); CAN 紧急通道协议无 CRC 字段
     * (对齐 Zephyr can_fw_upgrade), 完整性由 MCUboot 验签兜底 */
    if (check_crc) {
        uint16_t crc = 0;
        uint8_t buf[READ_CHUNK];

        for (uint32_t off = 0; off < s.total; off += READ_CHUNK) {
            uint32_t n = s.total - off;

            if (n > READ_CHUNK) {
                n = READ_CHUNK;
            }
            if (iof()->read(SLOT1_OFFSET + off, buf, n) != 0) {
                goto out;
            }
            crc = crc16_ccitt(crc, buf, n);
        }
        if (crc != crc_expect) {
            goto out;
        }
    }

    /* TLV keyhash 校验 (镜像自带公钥指纹) */
    {
        uint8_t kh[FW_KEYHASH_LEN];

        if (tlv_keyhash_get(kh) != 0 ||
            memcmp(kh, fw_keyhash, FW_KEYHASH_LEN) != 0) {
            goto out;
        }
    }

    rc = 0;
out:
    s.failed = false;
    s.written = 0;
    lock_give();
    return rc;
}

uint32_t fw_upg_received(void)
{
    uint32_t r;

    lock_take();
    r = s.written + s.page_len;
    lock_give();
    return r;
}

uint32_t fw_upg_total(void)
{
    uint32_t r;

    lock_take();
    r = s.active ? s.total : 0u;
    lock_give();
    return r;
}

bool fw_upg_active(void)
{
	return s.active;
}

bool fw_upg_failed(void)
{
	return s.failed;
}
