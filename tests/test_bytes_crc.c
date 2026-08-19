#include "test_util.h"
#include "io_bytes.h"
#include "io_crc.h"

int main(void)
{
    uint8_t b[4] = {0x12, 0x34, 0x56, 0x78};
    TEST_EQ_INT(io_get_be16(b), 0x1234);
    TEST_EQ_INT(io_get_be32(b), 0x12345678U);
    TEST_EQ_INT(io_get_le32(b), 0x78563412U);

    uint8_t o[4];
    io_put_be16(0xAABB, o); TEST_EQ_INT(o[0], 0xAA); TEST_EQ_INT(o[1], 0xBB);
    io_put_be32(0x11223344U, o); TEST_EQ_INT(o[0], 0x11); TEST_EQ_INT(o[3], 0x44);
    io_put_le32(0x11223344U, o); TEST_EQ_INT(o[0], 0x44); TEST_EQ_INT(o[3], 0x11);

    const uint8_t v[] = "123456789";
    TEST_EQ_INT(crc16_modbus(v, 9), 0x4B37);      /* CRC-16/MODBUS 标准校验值 */
    TEST_EQ_INT(crc32_ieee(v, 9), 0xCBF43926U);   /* CRC-32 标准校验值 */
    TEST_MAIN_END();
}
