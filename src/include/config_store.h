#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <stdint.h>
#include "io_flash.h"

/* NOR layout (16 MiB W25Q128): two config slots then littlefs. */
#define CFG_SLOT_A    0x100000u
#define CFG_SLOT_B    0x108000u
#define CFG_SLOT_SIZE 0x8000u
#define LFS_OFFSET    0x110000u
#define LFS_SIZE      (0x1000000u - 0x110000u)

/*
 * The 10 persisted configuration keys (one-to-one with the Zephyr
 * settings/FCB keys of the original firmware).
 */
struct io_cfg {
    uint16_t di_en, ai_en, di_si, ai_si, his;
    uint16_t can_id, can_bps, rs485_bps, slave_id;
    uint16_t ip[4];   /* 4 octets */
};

/*
 * Load config from flash. Reads both A/B slots; if both are invalid,
 * RAM holds the factory defaults. Returns 0, or -1 on flash I/O error.
 */
int config_store_init(const struct io_flash *f);

/* Persist cfg to the inactive slot (erase, then header/body/CRC writes),
 * generation + 1, and make it the active slot. Returns 0 or -1. */
int config_store_save(const struct io_cfg *cfg);

/* Factory reset: erase both slots, RAM back to defaults. */
void config_store_erase_all(void);

/* Currently effective config (or defaults if never initialized). */
void config_store_get(struct io_cfg *out);

/* Factory defaults (match the Zephyr holding_reg[] initializers). */
void config_store_get_defaults(struct io_cfg *out);

#endif /* CONFIG_STORE_H */
