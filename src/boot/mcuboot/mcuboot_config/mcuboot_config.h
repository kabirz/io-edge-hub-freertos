/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * MCUboot bootutil 配置 (boot 域): RSA-2048 验签 + SWAP_SCRATCH。
 * 对齐 Zephyr 版 io-edge-hub (SB_CONFIG_MCUBOOT_MODE_SWAP_SCRATCH,
 * BOOT_SIGNATURE_TYPE_RSA, BOOT_VALIDATE_SLOT0=y)。
 */

#ifndef MCUBOOT_CONFIG_H
#define MCUBOOT_CONFIG_H

/* RSA-2048 签名验证 (mbedTLS 后端) */
#define MCUBOOT_SIGN_RSA
#define MCUBOOT_SIGN_RSA_LEN 2048
#define MCUBOOT_USE_MBED_TLS

/* 单镜像双槽 */
#define MCUBOOT_IMAGE_NUMBER 1

/* SWAP_SCRATCH 交换 (支持回滚)。注意必须带值定义: bootutil 内
 * 存在 `#if MCUBOOT_SWAP_USING_SCRATCH` 的数值用法 */
#define MCUBOOT_SWAP_USING_SCRATCH 1

/* 每次启动验证 slot0 (检测损坏镜像) */
#define MCUBOOT_VALIDATE_PRIMARY_SLOT

/* 槽扇区数: slot1/scratch = 448KB/4KB = 112, 取整 120 */
#define MCUBOOT_MAX_IMG_SECTORS 120

/* 新版扇区 API (mbed 移植同款) */
#define MCUBOOT_USE_FLASH_AREA_GET_SECTORS

/* 日志/断言映射到 boot_log: SWAP 全程可见, assert 失败报行号
 * (newlib 默认 assert -> abort -> 无声死循环, 排障不可接受) */
#define MCUBOOT_HAVE_LOGGING
#define MCUBOOT_HAVE_ASSERT_H

/* FIH 默认 OFF (fault_injection_hardening.h 缺省) */

/* SWAP 期间逐块喂狗: app 会话启动的 IWDG 跨软复位仍在计数 */
#define MCUBOOT_WATCHDOG_FEED() watchdog_feed()

#define MCUBOOT_CPU_IDLE() \
    do {                   \
    } while (0)

#endif /* MCUBOOT_CONFIG_H */
