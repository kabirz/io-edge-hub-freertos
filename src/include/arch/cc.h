/*
 * LwIP arch/cc.h for ARM Cortex-M4 + GCC toolchain.
 */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stdio.h> /* printf (LWIP_PLATFORM_DIAG/ASSERT); GCC 14 起隐式
                    * 函数声明是硬错误, -w 无法降级 */
#include <sys/time.h> /* struct timeval (LWIP_TIMEVAL_PRIVATE=0 时
                       * lwip/sockets.h 直接用平台定义) */

/* Byte order: little-endian (STM32F407) — skip if already defined by newlib */
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

/* errno 走 newlib (<errno.h> 可重入), 见 lwipopts.h
 * (LWIP_ERRNO_STDINCLUDE=1); LWIP_PROVIDE_ERRNO 须完全不定义
 * (errno.h 用 #ifdef 判定, 定义为 0 也算已定义) */
#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while(0)
#define LWIP_PLATFORM_ASSERT(x) do { printf("LWIP_ASSERT: %s\n", x); for(;;){} } while(0)

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

/*
 * Relocate all LwIP memp pool memory to CCM (0x10000000, 64KB).
 * SPI is CPU-driven (no DMA), so CCM's "no DMA" restriction is irrelevant.
 * This saves ~23KB of SRAM (pbuf pool 15KB + memp pools ~8KB).
 */
#define LWIP_DECLARE_MEMORY_ALIGNED(variable_name, size) \
    u8_t variable_name[LWIP_MEM_ALIGN_BUFFER(size)] \
    __attribute__((section(".ccmram")))

#endif /* LWIP_ARCH_CC_H */
