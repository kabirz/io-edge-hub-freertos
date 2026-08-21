/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * os_malloc 桩: loader.c 仅为兼容 Mynewt 头文件而 include, 本移植
 * 无任何 os_malloc/os_free 调用点 (全树 grep 验证)。
 */

#ifndef OS_MALLOC_STUB_H
#define OS_MALLOC_STUB_H

#include <stddef.h>

static inline void *os_malloc(size_t size)
{
    (void)size;
    return NULL;
}

static inline void os_free(void *p)
{
    (void)p;
}

#endif /* OS_MALLOC_STUB_H */
