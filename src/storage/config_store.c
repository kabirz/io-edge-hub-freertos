#include "config_store.h"
#include "io_bytes.h"
#include "io_crc.h"
#include <string.h>

/*
 * On-disk slot format (little-endian scalar fields, io_cfg native bytes):
 *   [0]  magic "IOCF"          4 B
 *   [4]  generation            u32 LE
 *   [8]  len                   u16 LE (== sizeof(struct io_cfg))
 *   [10] struct io_cfg         native bytes
 *   [36] crc32_ieee([0..35])   u32 LE
 *
 * Writes go header -> body -> CRC (CRC last), so a torn write always
 * leaves a CRC mismatch and the peer slot stays loadable.
 */

_Static_assert(sizeof(struct io_cfg) == 26, "io_cfg must be 13 packed u16");

#define CFG_MAGIC      "IOCF"
#define CFG_MAGIC_LEN  4u
#define CFG_GEN_OFF    CFG_MAGIC_LEN                 /* 4 */
#define CFG_LEN_OFF    (CFG_GEN_OFF + 4u)            /* 8 */
#define CFG_HDR_LEN    (CFG_LEN_OFF + 2u)            /* 10 */
#define CFG_CRC_OFF    (CFG_HDR_LEN + sizeof(struct io_cfg))
#define CFG_REC_LEN    (CFG_CRC_OFF + 4u)            /* 40 */

static const struct io_flash *iof;
static struct io_cfg cur;
static uint32_t cur_gen;
static uint32_t cur_slot;    /* address of active slot, 0 = none (defaults) */
static uint8_t inited;

static void put_le16(uint16_t v, uint8_t *p)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

void config_store_get_defaults(struct io_cfg *out)
{
    static const struct io_cfg d = {
        .di_en = 0xFFFF, .ai_en = 0x000F,
        .di_si = 200, .ai_si = 200, .his = 0,
        .can_id = 0x0111, .can_bps = 250,
        .rs485_bps = 9600, .slave_id = 1,
        .ip = {192, 168, 12, 101},
    };
    *out = d;
}

/* 0 = slot valid, 1 = invalid content (bad magic/len/CRC), -1 = flash I/O error */
static int slot_read(uint32_t addr, struct io_cfg *out, uint32_t *gen)
{
    uint8_t rec[CFG_REC_LEN];

    if (iof->read(addr, rec, sizeof rec) != 0) return -1;
    if (memcmp(rec, CFG_MAGIC, CFG_MAGIC_LEN) != 0) return 1;
    if (get_le16(rec + CFG_LEN_OFF) != sizeof(struct io_cfg)) return 1;
    if (io_get_le32(rec + CFG_CRC_OFF) != crc32_ieee(rec, CFG_CRC_OFF)) return 1;
    memcpy(out, rec + CFG_HDR_LEN, sizeof(struct io_cfg));
    *gen = io_get_le32(rec + CFG_GEN_OFF);
    return 0;
}

/* Page-program respecting writes: len <= 256 per call, never crossing a 256 B page. */
static int write_pages(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    uint32_t off = 0;

    while (off < len) {
        uint32_t room = 256u - ((addr + off) & 0xFFu);
        uint32_t n = len - off;
        if (n > room) n = room;
        if (iof->write(addr + off, buf + off, n) != 0) return -1;
        off += n;
    }
    return 0;
}

int config_store_init(const struct io_flash *f)
{
    struct io_cfg ca, cb;
    uint32_t ga = 0, gb = 0;
    int ra, rb;

    iof = f;
    inited = 1;
    config_store_get_defaults(&cur);
    cur_gen = 0;
    cur_slot = 0;

    ra = slot_read(CFG_SLOT_A, &ca, &ga);
    rb = slot_read(CFG_SLOT_B, &cb, &gb);
    if (ra < 0 || rb < 0) return -1;                 /* flash error propagates */

    if (ra == 0 && (rb != 0 || ga >= gb)) {
        cur = ca; cur_gen = ga; cur_slot = CFG_SLOT_A;
    } else if (rb == 0) {
        cur = cb; cur_gen = gb; cur_slot = CFG_SLOT_B;
    }
    /* both invalid -> defaults stay; re-save targets slot A with generation 1 */
    return 0;
}

int config_store_save(const struct io_cfg *cfg)
{
    uint8_t rec[CFG_REC_LEN];
    uint32_t tgt, gen;

    if (!inited) return -1;

    tgt = (cur_slot == CFG_SLOT_A) ? CFG_SLOT_B : CFG_SLOT_A;
    gen = cur_gen + 1;

    memcpy(rec, CFG_MAGIC, CFG_MAGIC_LEN);
    io_put_le32(gen, rec + CFG_GEN_OFF);
    put_le16((uint16_t)sizeof(struct io_cfg), rec + CFG_LEN_OFF);
    memcpy(rec + CFG_HDR_LEN, cfg, sizeof(struct io_cfg));
    io_put_le32(crc32_ieee(rec, CFG_CRC_OFF), rec + CFG_CRC_OFF);

    if (iof->erase(tgt, CFG_SLOT_SIZE) != 0) return -1;
    if (write_pages(tgt, rec, CFG_HDR_LEN) != 0) return -1;
    if (write_pages(tgt + CFG_HDR_LEN, rec + CFG_HDR_LEN, sizeof(struct io_cfg)) != 0) return -1;
    if (write_pages(tgt + CFG_CRC_OFF, rec + CFG_CRC_OFF, 4u) != 0) return -1;

    cur = *cfg;
    cur_gen = gen;
    cur_slot = tgt;
    return 0;
}

void config_store_erase_all(void)
{
    if (inited && iof) {
        (void)iof->erase(CFG_SLOT_A, CFG_SLOT_SIZE);
        (void)iof->erase(CFG_SLOT_B, CFG_SLOT_SIZE);
    }
    config_store_get_defaults(&cur);
    cur_gen = 0;
    cur_slot = 0;
}

void config_store_get(struct io_cfg *out)
{
    if (!inited) {
        config_store_get_defaults(out);
        return;
    }
    *out = cur;
}
