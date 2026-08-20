/*
 * LwIP arch/cc.h for ARM Cortex-M4 + GCC toolchain.
 */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>

/* Byte order: little-endian (STM32F407) */
#define BYTE_ORDER LITTLE_ENDIAN

#define LWIP_PROVIDE_ERRNO 1
#define LWIP_PLATFORM_DIAG(x)
#define LWIP_PLATFORM_ASSERT(x)

/* Pack struct declarations */
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END
#define PACK_STRUCT_FLD_8(x)  x
#define PACK_STRUCT_FIELD(x)  x

/* Types */
typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;
typedef uintptr_t mem_ptr_t;

#endif /* LWIP_ARCH_CC_H */
