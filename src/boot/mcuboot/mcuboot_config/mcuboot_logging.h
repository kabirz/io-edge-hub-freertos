/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * BOOT_LOG_* 映射: boot 域 -> boot_log (UART), app 域 -> LOG_*。
 * BOOT_DOMAIN 仅 boot 目标定义。bootutil 日志仅用 %d/%u/%x/%s
 * (boot_log 已支持, 含 l/z 长度修饰)。
 */

#ifndef MCUBOOT_LOGGING_H
#define MCUBOOT_LOGGING_H

#ifdef BOOT_DOMAIN
#include "boot_uart.h"

#define MCUBOOT_LOG_MODULE_DECLARE(module)
#define MCUBOOT_LOG_MODULE_REGISTER(module)

#define MCUBOOT_LOG_ERR(...) boot_log("E: " __VA_ARGS__)
#define MCUBOOT_LOG_WRN(...) boot_log("W: " __VA_ARGS__)
#define MCUBOOT_LOG_INF(...) boot_log("I: " __VA_ARGS__)
#define MCUBOOT_LOG_DBG(...) ((void)0) /* 调试级默认关 (体积/噪声) */

#else /* app 域 (bootutil_public) */
#include "log.h"

#define MCUBOOT_LOG_MODULE_DECLARE(module)
#define MCUBOOT_LOG_MODULE_REGISTER(module)

#define MCUBOOT_LOG_ERR(...) LOG_ERR(__VA_ARGS__)
#define MCUBOOT_LOG_WRN(...) LOG_WRN(__VA_ARGS__)
#define MCUBOOT_LOG_INF(...) LOG_INF(__VA_ARGS__)
#define MCUBOOT_LOG_DBG(...) ((void)0)

#endif /* BOOT_DOMAIN */

#endif /* MCUBOOT_LOGGING_H */
