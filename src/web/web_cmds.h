/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Web 控制命令执行器 + JSON 构造器 (web_cmds.c 实现, Zephyr 版移植;
 * 写路径与 Modbus 回调共用 io_write_*, 副作用同 FC05/FC06)
 */

#ifndef __WEB_CMDS_H__
#define __WEB_CMDS_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DO 单点控制: index 0-7 (DO1-8), value 0/1 */
int web_cmd_exec_do(int32_t index, int32_t value);

/* 保持寄存器写: addr 0-17, value 0-65535 (0x11 转延迟重启) */
int web_cmd_exec_reg(int32_t addr, int32_t value);

/* 配置批量写: 字段 ip/rs485/sid/can_bps/can_id 均可选, 校验通过才写;
 * 失败 *err 指向静态错误描述 */
int web_cmd_exec_cfg(const char *json, size_t len, const char **err);

/* 共享 JSON 构造器 (含类型标记 "t":info/io/regs), 返回长度 */
int web_build_info_json(char *buf, size_t bufsz);
int web_build_io_json(char *buf, size_t bufsz);
int web_build_regs_json(char *buf, size_t bufsz);

#ifdef __cplusplus
}
#endif

#endif /* __WEB_CMDS_H__ */
