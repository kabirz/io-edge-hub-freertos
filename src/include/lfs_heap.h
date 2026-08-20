/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * littlefs 动态分配适配 (仅固件构建): CMake 对 littlefs 源文件定义
 * LFS_MALLOC=lfs_heap_alloc / LFS_FREE=lfs_heap_free, lfs_util.h 的
 * 内联 lfs_malloc/lfs_free 据此把每文件 1KB 缓存路由到 FreeRTOS
 * heap_4 (pvPortMalloc/vPortFree, 16KB 静态池)。
 *
 * 为什么必须路由: 固件 newlib 桩 _sbrk 恒返回 ENOMEM
 * (src/sys/syscalls.c), 不路由的话 littlefs 默认 malloc 路径上机
 * 必然返回 NULL -> lfs_file_open LFS_ERR_NOMEM, 历史记录静默失效。
 *
 * host 测试 (tests/CMakeLists.txt 独立项目) 不带该定义, 直接用系统
 * malloc。本头由 CMake 以 -include 前置进 littlefs 两个源文件的编译
 * 单元 (lfs.c 的 lfs_malloc 调用点需要原型); 适配实现见 lfs_port.c
 * (LFS_HEAP_FW 门控编译)。
 */

#ifndef LFS_HEAP_H
#define LFS_HEAP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *lfs_heap_alloc(size_t size);
void lfs_heap_free(void *p);

#ifdef __cplusplus
}
#endif

#endif /* LFS_HEAP_H */
