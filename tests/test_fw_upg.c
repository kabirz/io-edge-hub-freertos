/*
 * fw_upg 核心 host 测试: 页缓冲写 / CRC16-CCITT 读回校验 / MCUboot TLV
 * keyhash 解析 / 会话状态机 (active 互斥、keyhash 预校验、溢出失败)。
 *
 * w25qxx_flash() 由本测试提供 (RAM fake, 全片 16MiB), fw_upg.c 直编。
 * 镜像构造: [0x200 头 (magic/hdr_size/img_size)][payload][TLV: keyhash],
 * 与 imgtool 签名产物同构 (fw_upg_finish 只看 keyhash, 不验签名)。
 */
#include <stdio.h>
#include <string.h>

#include "fake_flash.h"
#include "flash_layout.h"
#include "fw_keyhash.h"
#include "fw_upg.h"
#include "test_util.h"

/* fw_upg.c 的 NOR 后端在 host 上指向 fake */
const struct io_flash *w25qxx_flash(void)
{
    return fake_flash_get();
}

/* ============ 参考实现 (Zephyr crc16_ccitt 逐式: 反射 nibble, poly
 * 0x8408 -- zephyr/subsys/crc/crc16_sw.c; 旧版误用的字节交换形式是
 * Zephyr 的 crc16_itu_t/XMODEM, 41372ae 已对齐) ============ */

static uint16_t ref_crc16(uint16_t seed, const uint8_t *src, uint32_t len)
{
    while (len-- > 0) {
        uint8_t e = (uint8_t)(seed ^ *src++);
        uint8_t f = (uint8_t)(e ^ (e << 4));
        seed = (uint16_t)((seed >> 8) ^ ((uint16_t)f << 8) ^
                          ((uint16_t)f << 3) ^ (f >> 4));
    }
    return seed;
}

/* ============ 测试镜像构造 ============ */

#define TEST_HDR 0x200u
#define TEST_PAYLOAD 1000u
/* TLV = 头 4B + [tag 2B][len 2B][keyhash 32B] */
#define TLV_SZ (4u + 4u + 32u)
#define TEST_TOTAL (TEST_HDR + TEST_PAYLOAD + TLV_SZ)

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* keyhash=NULL -> TLV 放 SHA256 tag (0x0010) 模拟无指纹镜像 */
static void make_image(uint8_t *img, uint32_t payload,
                       const uint8_t *keyhash)
{
    memset(img, 0xFF, TEST_TOTAL);
    put_le32(&img[0], 0x96F3B83Du);      /* header magic */
    put_le16(&img[8], (uint16_t)TEST_HDR);
    put_le32(&img[12], payload);         /* img_size */
    for (uint32_t i = 0; i < payload; i++) {
        img[TEST_HDR + i] = (uint8_t)(i * 7u);
    }
    /* TLV */
    {
        uint8_t *tlv = &img[TEST_HDR + payload];

        put_le16(&tlv[0], 0x6907u);      /* TLV magic */
        put_le16(&tlv[2], (uint16_t)TLV_SZ);
        put_le16(&tlv[4], keyhash ? 0x0001u : 0x0010u);
        put_le16(&tlv[6], 32u);
        memcpy(&tlv[8], keyhash ? keyhash : fw_keyhash, 32u);
    }
}

static void write_chunks(const uint8_t *img, uint32_t total, uint32_t chunk)
{
    uint32_t off = 0;

    while (off < total) {
        uint32_t n = total - off < chunk ? total - off : chunk;

        TEST_ASSERT(fw_upg_write(&img[off], n) == 0);
        off += n;
        TEST_ASSERT(fw_upg_received() == off);
    }
}

int main(void)
{
    uint8_t img[TEST_TOTAL];
    const uint8_t bad_hash[32] = {0};

    fake_flash_reset();

    /* ---- CRC16 已知答案 ("123456789" -> 0x2189, KERMIT) ---- */
    {
        const char *kat = "123456789";

        TEST_ASSERT(ref_crc16(0, (const uint8_t *)kat, 9) == 0x2189);
    }

    /* ---- 参数边界 ---- */
    TEST_ASSERT(fw_upg_start(63, NULL) == -1);          /* total < 64 */
    TEST_ASSERT(fw_upg_start(SLOT1_SIZE + 1, NULL) == -1);
    TEST_ASSERT(!fw_upg_active());

    /* ---- 未开始就写/收尾 ---- */
    TEST_ASSERT(fw_upg_write((const uint8_t *)"x", 1) == -1);
    TEST_ASSERT(fw_upg_finish(0) == -1);

    /* ---- keyhash 预校验: 不一致拒绝且不擦 slot1 ---- */
    make_image(img, TEST_PAYLOAD, fw_keyhash);
    {
        uint8_t marker = 0x5A;
        uint8_t v = 0;
        const struct io_flash *f = fake_flash_get();

        TEST_ASSERT(f->erase(SLOT1_OFFSET, 4096) == 0);
        TEST_ASSERT(f->write(SLOT1_OFFSET, &marker, 1) == 0);
        TEST_ASSERT(fw_upg_start(TEST_TOTAL, bad_hash) == -2);
        TEST_ASSERT(f->read(SLOT1_OFFSET, &v, 1) == 0 && v == 0x5A);
    }

    /* ---- 完整成功路径: 奇数分片跨页 -> CRC/TLV 全过 ---- */
    TEST_ASSERT(fw_upg_start(TEST_TOTAL, fw_keyhash) == 0);
    TEST_ASSERT(fw_upg_active());
    TEST_ASSERT(fw_upg_start(TEST_TOTAL, fw_keyhash) == -3); /* 互斥 */
    write_chunks(img, TEST_TOTAL, 77);
    TEST_ASSERT(fw_upg_finish(ref_crc16(0, img, TEST_TOTAL)) == 0);
    TEST_ASSERT(!fw_upg_active());

    /* slot1 内容逐字节核对 (含页缓冲尾块) */
    {
        uint8_t back[TEST_TOTAL];
        const struct io_flash *f = fake_flash_get();

        TEST_ASSERT(f->read(SLOT1_OFFSET, back, sizeof back) == 0);
        TEST_ASSERT(memcmp(back, img, sizeof back) == 0);
    }

    /* ---- CRC 不一致: finish 拒绝, 会话复位 ---- */
    TEST_ASSERT(fw_upg_start(TEST_TOTAL, fw_keyhash) == 0);
    write_chunks(img, TEST_TOTAL, 256);
    TEST_ASSERT(fw_upg_finish(
        (uint16_t)(ref_crc16(0, img, TEST_TOTAL) ^ 1u)) == -1);
    TEST_ASSERT(!fw_upg_active());

    /* ---- 数据量不足: finish 拒绝 ---- */
    TEST_ASSERT(fw_upg_start(TEST_TOTAL, fw_keyhash) == 0);
    write_chunks(img, TEST_TOTAL - 3, 77);
    TEST_ASSERT(fw_upg_finish(ref_crc16(0, img, TEST_TOTAL)) == -1);

    /* ---- 溢出写: 失败态静默丢弃后续 ---- */
    TEST_ASSERT(fw_upg_start(TEST_TOTAL, fw_keyhash) == 0);
    TEST_ASSERT(fw_upg_write(img, TEST_TOTAL + 1) == -1);
    TEST_ASSERT(fw_upg_failed());
    TEST_ASSERT(fw_upg_write(img, 16) == -1);
    TEST_ASSERT(fw_upg_finish(ref_crc16(0, img, TEST_TOTAL)) == -1);
    TEST_ASSERT(!fw_upg_failed());

    /* ---- abort 复位会话 ---- */
    TEST_ASSERT(fw_upg_start(TEST_TOTAL, fw_keyhash) == 0);
    fw_upg_abort();
    TEST_ASSERT(!fw_upg_active());
    TEST_ASSERT(fw_upg_write(img, 16) == -1);

    /* ---- TLV keyhash 与编译期不一致: finish 拒绝 ---- */
    make_image(img, TEST_PAYLOAD, bad_hash);
    TEST_ASSERT(fw_upg_start(TEST_TOTAL, NULL) == 0);
    write_chunks(img, TEST_TOTAL, 77);
    TEST_ASSERT(fw_upg_finish(ref_crc16(0, img, TEST_TOTAL)) == -1);

    /* ---- 镜像无 keyhash TLV (仅 SHA256): finish 拒绝 ---- */
    make_image(img, TEST_PAYLOAD, NULL);
    TEST_ASSERT(fw_upg_start(TEST_TOTAL, NULL) == 0);
    write_chunks(img, TEST_TOTAL, 77);
    TEST_ASSERT(fw_upg_finish(ref_crc16(0, img, TEST_TOTAL)) == -1);

    /* ---- 镜像头损坏: TLV 解析拒绝 ---- */
    make_image(img, TEST_PAYLOAD, fw_keyhash);
    put_le32(&img[0], 0xDEADBEEFu);
    TEST_ASSERT(fw_upg_start(TEST_TOTAL, NULL) == 0);
    write_chunks(img, TEST_TOTAL, 77);
    TEST_ASSERT(fw_upg_finish(ref_crc16(0, img, TEST_TOTAL)) == -1);

    TEST_MAIN_END();
}
