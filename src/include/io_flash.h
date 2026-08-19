#ifndef IO_FLASH_H
#define IO_FLASH_H

#include <stdint.h>

/*
 * NOR flash backend abstraction. Target uses W25Qxx over QSPI/SPI,
 * host tests use the RAM fake in tests/fake_flash.c. Addresses are
 * absolute offsets into the NOR device (16 MiB W25Q128 layout).
 * All ops return 0 on success, -1 on error.
 */
struct io_flash {
    int (*read)(uint32_t addr, uint8_t *buf, uint32_t len);
    int (*erase)(uint32_t addr, uint32_t len);   /* 4 KiB sector erase; addr/len multiples of 4096 */
    int (*write)(uint32_t addr, const uint8_t *buf, uint32_t len); /* page program; len <= 256, no page crossing */
};

#endif /* IO_FLASH_H */
