/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * mbedTLS 3.0 最小配置: 仅 RSA-2048 公钥验签路径
 * (asn1parse + bignum + rsa + sha256/md), 静态内存缓冲分配器
 * (boot 域无 newlib malloc)。本目录排在 ext/mbedtls/include 之前,
 * 覆盖默认 mbedtls/mbedtls_config.h。
 */

#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

#define MBEDTLS_CONFIG_VERSION 0x03000000

#define MBEDTLS_HAVE_ASM

/* 内存: 静态缓冲分配器 (mbedtls_memory_buffer_alloc_init 注入) */
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_MEMORY_BUFFER_ALLOC_C
#define MBEDTLS_PLATFORM_C

/* RSA-2048 验签 */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_OID_C /* check_config: RSA_C 前置 (未用到 x509 解析) */
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA224_C /* check_config: SHA256_C 要求成对定义 */

/* mpi 体积优先: 窗口 1 (boot 每次启动仅一次验签, 速度可换体积) */
#define MBEDTLS_MPI_WINDOW_SIZE 1

#endif /* MBEDTLS_CONFIG_H */
