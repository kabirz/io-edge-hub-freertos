#ifndef IO_BYTES_H
#define IO_BYTES_H

#include <stdint.h>

/* Big/little-endian byte-order helpers over unaligned byte buffers. */

uint16_t io_get_be16(const uint8_t *p);
uint32_t io_get_be32(const uint8_t *p);
uint32_t io_get_le32(const uint8_t *p);

void io_put_be16(uint16_t v, uint8_t *p);
void io_put_be32(uint32_t v, uint8_t *p);
void io_put_le32(uint32_t v, uint8_t *p);

#endif /* IO_BYTES_H */
