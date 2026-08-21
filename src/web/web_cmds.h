/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Web 控制命令执行器接口 (web_cmds.c 实现, httpd.c 调用; Zephyr 版
 * src/web/web_cmds.h 的移植, WS 后续迭代复用同一入口)
 * 写路径与 Modbus 回调共用 io_write_*, 副作用与 FC05/FC06 一致
 */

#ifndef __WEB_CMDS_H__
#define __WEB_CMDS_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DO 单点控制: index 0-7 对应 DO1-DO8, value 0/1 */
int web_cmd_exec_do(int32_t index, int32_t value);

/* 保持寄存器写: addr 0-17, value 0-65535 (0x11 重启转延迟重启保住响应) */
int web_cmd_exec_reg(int32_t addr, int32_t value);

/* 系统配置批量写 (POST /api/cfg): 字段 ip/rs485/sid/can_bps/can_id 均可选,
 * 校验通过才写入; 失败时 *err 指向静态错误描述, 返回 -EINVAL */
int web_cmd_exec_cfg(const char *json, size_t len, const char **err);

/* ==================== 共享 JSON 构造器 (web_cmds.c 实现) ==================== */
/* 设备详情 JSON (含 "t":"info"), 返回长度 */
int web_build_info_json(char *buf, size_t bufsz);
/* 实时 IO 快照 JSON (含 "t":"io") */
int web_build_io_json(char *buf, size_t bufsz);
/* 寄存器全量 JSON (含 "t":"regs") */
int web_build_regs_json(char *buf, size_t bufsz);

#ifdef __cplusplus
}
#endif

#endif /* __WEB_CMDS_H__ */
