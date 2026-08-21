/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 固件升级核心 (Zephyr 版 libs/udp_fw_upgrade + ws_io 验证逻辑的
 * io_flash 移植): 镜像写入 slot1 (外部 NOR)、CRC16-CCITT 校验、
 * MCUboot TLV keyhash 解析。OS 无关, host 可测 (锁为空实现);
 * boot_request_upgrade (写 trailer) 由调用方在 fw_upg_finish 成功后
 * 执行, 保持核心不依赖 bootutil。
 *
 * 通道互斥: 三通道 (UDP/CAN/WS) 共用单例状态, fw_upg_active() 期间
 * 其他通道的 start 被拒绝; 内部锁保护跨任务交错 (host 单线程无锁)。
 */

#ifndef FW_UPG_H
#define FW_UPG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* keyhash 预校验长度 */
#define FW_KEYHASH_LEN 32u

/* 开始升级: 擦 slot1 (ROUND_UP(total,4K)) 并复位状态。
 * keyhash 非空时先与编译期 fw_keyhash.h 比对, 不一致拒绝且不擦。
 * 返回 0 成功, -1 参数/flash 错, -2 keyhash 不一致, -3 已在进行中 */
int fw_upg_start(uint32_t total, const uint8_t *keyhash);

/* 追加镜像数据 (内部按 256B 页缓冲写)。返回 0 成功, -1 失败
 * (失败后通道应终止本会话) */
int fw_upg_write(const uint8_t *data, uint32_t len);

/* 终止并复位会话 (通道失败/超时路径) */
void fw_upg_abort(void);

/* 收尾校验: flush -> 尺寸 -> 读回 CRC16-CCITT 比对 -> TLV keyhash
 * 比对。全部通过返回 0 且复位会话 (调用方随后 boot_request_upgrade);
 * 否则复位会话并返回 -1 */
int fw_upg_finish(uint16_t crc_expect);

/* 已收字节数 (FW_DATA 应答回显) */
uint32_t fw_upg_received(void);

bool fw_upg_active(void);

/* 会话处于失败态 (写失败后静默丢弃后续数据) */
bool fw_upg_failed(void);

/* target 专用: 创建内部互斥锁 (调度器启动后、首条 fw 命令前调用) */
void fw_upg_os_init(void);

#ifdef __cplusplus
}
#endif

#endif /* FW_UPG_H */
