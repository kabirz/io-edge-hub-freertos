/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * STM32F407 片内 flash 擦写 (boot 域使用: MCUboot slot0 交换与
 * CAN 紧急升级直写; app 不写片内 flash)。
 *
 * 地址为绝对映射地址 (0x0800xxxx)。F407 非均匀扇区:
 *   sector 0-3  16KB  0x08000000-0x0800FFFF
 *   sector 4    64KB  0x08010000-0x0801FFFF
 *   sector 5-11 128KB 0x08020000-0x0807FFFF
 * 擦除按覆盖到的整个扇区进行; 写入字节粒度 (内部按字对齐段优化)。
 */

#ifndef INTFLASH_H
#define INTFLASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 擦除 [addr, addr+len) 覆盖到的所有扇区。len 按 4KB 向上取整对齐
 * (调用方以镜像大小调用即可)。成功 0, 失败 -1。 */
int intflash_erase(uint32_t addr, uint32_t len);

/* 编程任意长度 (字节粒度, 自动拆为对齐字编程 + 首尾字节编程)。
 * 目标区必须已擦除。成功 0, 失败 -1。 */
int intflash_write(uint32_t addr, const uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* INTFLASH_H */
