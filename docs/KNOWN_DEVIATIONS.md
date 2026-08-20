# 已知偏差汇总 (FreeRTOS 移植一期 vs Zephyr 现版)

逐条列出与 Zephyr 版 (`applications/io-edge-hub`) 的行为/实现差异及
原因。**协议帧格式与寄存器布局无偏差**; 下列均为边沿场景或实现层差异,
均已在对应任务评审中裁定接受。标 (非偏差) 的条目是与 Zephyr 完全
一致、但容易在上机时被误报为 bug 的行为, 写明以防误判。

## Modbus TCP

1. **并发客户端上限 2**。Zephyr 版为动态链表、无上限 (listen backlog
   16)。原因: W5500 硬件 socket 硬约束 —— 8 个 socket 中 0 归 UDP
   配置、4-7 留给二期, Modbus TCP 预算 3 个, 其中 1 个须保持
   LISTEN, 故最多 2 个并发主站; 满 2 时不再 LISTEN, 第 3 个连接由
   W5500 硬件拒绝。
2. **keepalive 单参数 30s**。Zephyr 版 `SO_KEEPALIVE` 三参数 30/5/3
   (idle/interval/count)。原因: W5500 仅提供 `Sn_KPALVTR` 一个寄存器
   (值 x 5s = 探测周期), 取 6 → 30s 周期探测; 拔线后断开时间约
   45-90s, 与现版同量级。
3. **PDU 长度违例静默丢弃**。MBAP 声明长度超上限时本版静默丢帧;
   Zephyr 版 800ms 后回 `SERVER_DEVICE_FAILURE`。原因: 移植版半帧
   500ms 无进展直接断开回收连接, 长度违例与半帧超时合并为同一
   防御路径, 不再单独构造异常应答。
4. **SO_REUSEADDR 不可用** (非行为偏差)。W5500 无此 socket 选项;
   槽位回收用 close 后重开, TIME_WAIT 由 close 的 CR_CLOSE 语义
   兜住, 对主站不可见。

## CAN

5. **800kbps 不可达**。CAN 时钟 = PCLK1 = 42MHz, 42M/800k = 52.5 tq
   非整数, 无整数分频组合; reg 0x07=800 视为非法回落 250k。Zephyr
   版 (APB1 50MHz 时钟树) 可达 800k。原因: F407 168MHz 时钟树下
   APB1 只能取 42MHz。
6. **滤波器 IDE/RTR don't-care**。16 位标识符掩码模式下掩码的
   RTR/IDE 位为 0, 远端帧按 STID 命中同判通过。Zephyr 版
   `can_add_rx_filter` 按 IDE/RTR 精确匹配。原因: 本版收帧即静默
   丢弃 (无业务处理), 滤波粒度不影响行为, 只影响哪些帧进 RX 中断。
7. **mod_can_send 忙等邮箱 + id 超 0x7FF 截断**。空闲邮箱等待为
   HAL_GetTick 忙等 <=100ms (不假设调用上下文可睡眠); id > 0x7FF
   按位域截断, Zephyr 版 `can_send` 传入非法 id 返回 -EINVAL。
   原因: 当前无调用者, 简化实现; 二期启用时再对齐错误语义。

## ADC / RTU 时序

8. **ADC 时钟 21MHz (PCLK2/4)**。Zephyr overlay 配 /2 = 42MHz, 超
   F407 数据手册 ADC 时钟上限 (21/42MHz 档边界, 12bit 分辨率下
   规格值为 21MHz), 移植版取 /4 = 21MHz 规格内。影响: 采样转换
   时间略长, 换算结果位数不变。
9. **RTU t3.5 定时**: >19200bps 固定 2ms (Modbus 规范推荐值;
   Zephyr 版 >38400bps 约 1ms); 9600bps 时 t3.5 理论 4.01ms, 本版
   ms 定时器分辨率向上双进位为 5ms。原因: 软件定时器以整 ms 为
   粒度, 不引入 us 级定时器; t3.5 偏大只会让分帧更保守, 不错帧。

## 诊断与溢出

10. **RTU 溢出帧归类为尺寸违例**。接收缓冲满时整帧按
    `BUS_MSG_SIZE_ERR` 丢弃; Zephyr 版会对截断帧继续做 CRC 校验,
    CRC 恰好正确时按正常帧处理。原因: 溢出帧内容已不可信, 直接
    丢弃更安全; 计数器仍记录。
11. **MB_DIAG 计数器为共享 RMW**。多传输 (TCP/RTU) 的诊断计数为
    非原子读-改-写, 并发自增理论可丢计数。原因: 单字对齐读写本身
    原子, 仅 ++ 组合非原子; 诊断用途可容忍, 加锁不划算。

## UDP 配置协议

12. **0x19 (FACTORY_RESET) 应答 ok 恒 1**。Zephyr 版应答回传配置区
    擦除结果。原因: `config_store` 的擦除 API 无失败信号 (void
    语义), 擦失败时设备也会随后重启, ok 字段失去意义。
13. **子网判定固定 /24**。跨网段白名单判定用 `remote & /24 ==
    local & /24`; Zephyr 版查 `net_if` 的实际掩码。原因: 本版掩码
    本身固定 /24 (W5500 静态配置), 与配置寄存器一致。

## 参数与标识

14. **波特率 u16 上限 65535**。(非新偏差) holding 寄存器本身 16 位,
    Zephyr 现版同限; 写入 >65535 截断为低 16 位, 与现版一致, 注明
    以防误报。
15. **MAC 无 01:02:03 回退**。Zephyr 版 hwinfo 读取失败回退
    01:02:03:04:05:06; 本版 MAC 由 `UID_BASE` (96-bit 唯一 ID 的
    存储器映射) 折叠派生, 恒可读, 无失败路径, 回退分支自然消失。
16. **开机 banner 的 flash/RAM 容量硬编码** (512/192KB)。Zephyr 版
    从 Kconfig 读。原因: 单板固定器件, 未引入构建期容量注入。

## 存储与体积

17. **littlefs 文件缓存走 heap_4, 1KB/文件**。Zephyr 版由 Zephyr
    系统堆/静态区供给。原因: FreeRTOS 侧统一 heap_4 (16KB 静态池)
    供给 —— 固件 newlib `_sbrk` 桩恒返回 ENOMEM, littlefs 默认
    malloc 路径上机必 `LFS_ERR_NOMEM`, 故经 `LFS_MALLOC` 钩子
    (`src/include/lfs_heap.h`) 显式路由到 `pvPortMalloc`。当前
    历史记录同一时刻仅打开 1 个文件 (峰值 1KB), 池余量充足;
    二期 web/FTP 多文件并发时重估池容量。
18. **newlib stdio 全量链接 (+约 80KB text)**。littlefs 拉入
    snprintf/stdio 全家桶。可选优化: 换 `--specs=nano.specs`
    (newlib-nano) 可省 ~数十 KB; 当前 148KB flash 占用远低于 512KB
    器件容量 (预算 300KB), 一期不裁剪。
19. **FC16 尾字节容忍规则 (非偏差)**: 写寄存器数与字节数不一致时
    按字节数处理、忽略尾随字节, 与 Zephyr 版逐字节一致, 写明以防
    上机对比测试误报。

## 环境说明

- 主机单元测试在 WSL/Linux 下构建运行 (`tests/`, 见 README);
  Windows 原生无编译器环境, 非固件行为偏差。
