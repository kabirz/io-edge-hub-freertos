#include "test_util.h"
#include "config_store.h"
#include "fake_flash.h"
#include "io_bytes.h"

/* ---- helpers ---- */

static void expect_defaults(const struct io_cfg *c)
{
    TEST_EQ_INT(c->di_en, 0xFFFF);
    TEST_EQ_INT(c->ai_en, 0x000F);
    TEST_EQ_INT(c->di_si, 200);
    TEST_EQ_INT(c->ai_si, 200);
    TEST_EQ_INT(c->his, 0);
    TEST_EQ_INT(c->can_id, 0x0111);
    TEST_EQ_INT(c->can_bps, 250);
    TEST_EQ_INT(c->rs485_bps, 9600);
    TEST_EQ_INT(c->slave_id, 1);
    TEST_EQ_INT(c->ip[0], 192);
    TEST_EQ_INT(c->ip[1], 168);
    TEST_EQ_INT(c->ip[2], 12);
    TEST_EQ_INT(c->ip[3], 101);
}

static int slot_has_magic(uint32_t slot)
{
    uint8_t m[4];
    TEST_EQ_INT(fake_flash_get()->read(slot, m, 4), 0);
    return memcmp(m, "IOCF", 4) == 0;
}

static int slot_is_erased(uint32_t slot)
{
    uint8_t m[4];
    TEST_EQ_INT(fake_flash_get()->read(slot, m, 4), 0);
    return m[0] == 0xFF && m[1] == 0xFF && m[2] == 0xFF && m[3] == 0xFF;
}

static uint32_t slot_generation(uint32_t slot)
{
    uint8_t hdr[8];
    TEST_EQ_INT(fake_flash_get()->read(slot, hdr, 8), 0);
    return io_get_le32(hdr + 4);
}

int main(void)
{
    struct io_cfg c, loaded;

    /* ---- 1. empty chip (all 0xFF): init -> defaults ---- */
    fake_flash_reset();
    TEST_EQ_INT(config_store_init(fake_flash_get()), 0);
    config_store_get(&c);
    expect_defaults(&c);
    config_store_get_defaults(&loaded);
    expect_defaults(&loaded);

    /* ---- 2. save then re-init -> saved value restored ---- */
    fake_flash_reset();
    TEST_EQ_INT(config_store_init(fake_flash_get()), 0);
    config_store_get(&c);
    c.di_en = 0x00FF; c.ai_en = 0;
    c.di_si = 10; c.ai_si = 20; c.his = 7;
    c.can_id = 0x07FF; c.can_bps = 1000; c.rs485_bps = 57600; c.slave_id = 17;
    c.ip[0] = 10; c.ip[1] = 0; c.ip[2] = 0; c.ip[3] = 5;
    TEST_EQ_INT(config_store_save(&c), 0);
    TEST_EQ_INT(config_store_init(fake_flash_get()), 0);
    config_store_get(&loaded);
    TEST_EQ_INT(loaded.di_en, 0x00FF);
    TEST_EQ_INT(loaded.can_bps, 1000);
    TEST_EQ_INT(loaded.rs485_bps, 57600);
    TEST_EQ_INT(loaded.ip[3], 5);
    TEST_EQ_MEM(&loaded, &c, sizeof c);

    /* ---- 3. two saves with different values -> A/B slot alternation,
     *          generation increments, latest wins after re-init ---- */
    fake_flash_reset();
    TEST_EQ_INT(config_store_init(fake_flash_get()), 0);
    config_store_get(&c);
    c.can_bps = 500;
    TEST_EQ_INT(config_store_save(&c), 0);
    TEST_ASSERT(slot_has_magic(CFG_SLOT_A));     /* first save -> slot A */
    TEST_ASSERT(slot_is_erased(CFG_SLOT_B));
    TEST_EQ_INT(slot_generation(CFG_SLOT_A), 1);
    c.can_bps = 125;
    TEST_EQ_INT(config_store_save(&c), 0);
    TEST_ASSERT(slot_has_magic(CFG_SLOT_B));     /* second save -> slot B */
    TEST_ASSERT(slot_has_magic(CFG_SLOT_A));     /* slot A untouched */
    TEST_EQ_INT(slot_generation(CFG_SLOT_A), 1);
    TEST_EQ_INT(slot_generation(CFG_SLOT_B), 2);
    TEST_EQ_INT(config_store_init(fake_flash_get()), 0);
    config_store_get(&loaded);
    TEST_EQ_INT(loaded.can_bps, 125);            /* newest generation wins */

    /* ---- 4. corrupt one byte of the newest slot (B) -> fall back to A ---- */
    fake_flash_corrupt(CFG_SLOT_B + 12);         /* inside stored io_cfg body */
    TEST_EQ_INT(config_store_init(fake_flash_get()), 0);
    config_store_get(&loaded);
    TEST_EQ_INT(loaded.can_bps, 500);            /* slot A value */

    /* ---- 5. corrupt the other slot too -> both bad -> defaults ---- */
    fake_flash_corrupt(CFG_SLOT_A + 10);         /* first body byte of slot A */
    TEST_EQ_INT(config_store_init(fake_flash_get()), 0);
    config_store_get(&loaded);
    expect_defaults(&loaded);

    /* ---- 6. erase_all -> factory reset: defaults, slots wiped ---- */
    fake_flash_reset();
    TEST_EQ_INT(config_store_init(fake_flash_get()), 0);
    config_store_get(&c);
    c.slave_id = 99;
    TEST_EQ_INT(config_store_save(&c), 0);
    config_store_erase_all();
    config_store_get(&loaded);
    expect_defaults(&loaded);
    TEST_ASSERT(slot_is_erased(CFG_SLOT_A));
    TEST_ASSERT(slot_is_erased(CFG_SLOT_B));
    TEST_EQ_INT(config_store_init(fake_flash_get()), 0);
    config_store_get(&loaded);
    expect_defaults(&loaded);

    TEST_MAIN_END();
}
