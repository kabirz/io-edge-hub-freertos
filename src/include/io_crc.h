#ifndef IO_CRC_H
#define IO_CRC_H

#include <stddef.h>
#include <stdint.h>

/* CRC-16/MODBUS: reflected poly 0xA001, init 0xFFFF (Modbus RTU frame check). */
uint16_t crc16_modbus(const uint8_t *data, size_t len);

/* CRC-32 (IEEE 802.3): reflected poly 0xEDB88320, init/xorout 0xFFFFFFFF. */
uint32_t crc32_ieee(const uint8_t *data, size_t len);

#endif /* IO_CRC_H */
