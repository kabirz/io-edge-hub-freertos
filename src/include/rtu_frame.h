/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modbus RTU 从站帧状态机 (纯逻辑, host 可测)。传输层喂数据/超时,
 * 帧完整时内部走 mb_server 解码, 需要应答时经 tx 回调输出完整帧。
 *
 * 语义基准: Zephyr subsys/modbus/modbus_serial.c (IRQ API 路径) +
 * modbus_core.c modbus_rx_handler + modbus_server.c modbus_server_handler。
 *
 * 帧 = [unit][pdu...][crc16 低字节][crc16 高字节]; 缓冲 256 字节,
 * 溢出后丢弃后续字节至复位; t3.5 到期后依序校验 (任一不过即静默):
 *   溢出 / len<4 -> 丢弃 (Zephyr -EMSGSIZE)
 *   CRC 不符     -> 丢弃 (Zephyr -EIO)
 *   unit!=0 且 != srv_unit -> 丢弃 (他站帧)
 *   unit==0 (广播) -> 副作用照常执行但不应答
 * 诊断计数组合见 rtu_frame.c 头部注释 (bus_msg 含被丢弃的帧)。
 */

#ifndef RTU_FRAME_H
#define RTU_FRAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 清空拼帧状态 (缓冲/长度/溢出标志); 不触碰绑定参数 */
void rtu_reset(void);

/* 喂一段收到的字节 (一次 UART IDLE 事件的载荷): 追加进拼帧缓冲,
 * 溢出部分丢弃; 每次调用重启 t3.5 定时钩子 rtu_t35_kick()。
 * target 上运行于 USART2 ISR 上下文。 */
void rtu_rx_feed(const uint8_t *bytes, uint16_t len);

/* t3.5 到期钩子 (传输层定时器触发): 当前缓冲内容即为一帧, 按上述
 * 规则校验/交付/应答, 完成后复位拼帧状态。空缓冲时无操作。 */
void rtu_t35_expired(void);

/* 注入配置 (host 测试与 target 传输层各自注册):
 *   srv_unit - 本站从站号 (target: 启动时读 reg 0x09, 运行期不生效)
 *   baud     - 波特率 (记录对齐 Zephyr cfg; 定时周期由传输层经
 *              rtu_t35_ms() 自行计算)
 *   tx       - 应答帧输出回调 (frame = unit+pdu+crc16 LE16, 完整帧)
 * 绑定时清空拼帧状态。 */
void rtu_frame_bind(uint8_t srv_unit, uint32_t baud,
		    void (*tx)(const uint8_t *frame, uint16_t len));

/* t3.5 定时重启钩子, rtu_rx_feed 内部每次调用。弱默认为空 (host);
 * target 传输层提供强符号 (ISR 上下文, 用 xTimerResetFromISR)。 */
void rtu_t35_kick(void);

/* t3.5 周期 (ms): ceil(3.5 字符 x 11 bit / baud), baud>19200 固定 2ms
 * (Modbus 规范上限波特率的推荐定值; Zephyr 字面公式为 >38400 钳制,
 * 任务锁定规范值)。baud==0 按 2ms 防御处理。 */
uint32_t rtu_t35_ms(uint32_t baud);

#ifdef __cplusplus
}
#endif

#endif /* RTU_FRAME_H */
