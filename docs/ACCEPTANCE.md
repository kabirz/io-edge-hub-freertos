# 上机验收清单 (FreeRTOS 移植一期)

逐条勾选。目标固件: `phase1` 分支构建的 `app.hex` (Debug 或 Release 均可,
日志项需 Debug 构建)。默认网络 `192.168.12.101/24`, Modbus TCP 端口
502, UDP 配置端口 8600, RS485 9600 8N1 从站号 1, 日志串口 USART1
115200。

前提: [README](../README.md) 的依赖初始化与构建步骤可复现, 主机测试
11/11 通过。

## 1. pytest 套件复用 (Zephyr 版用例, 协议不变)

Zephyr 版测试位于 apps 仓库 `applications/io-edge-hub/tests/`
(Windows 侧 `C:\Users\jxwaz\code\app\apps\applications\io-edge-hub\tests\`)。
Modbus TCP 与 UDP 用例直接复用 —— 帧格式、寄存器布局、命令语义与
Zephyr 版一致, 无需修改:

- [ ] `functional/test_coil_do.py` (FC01/05/15, DO 回读一致)
- [ ] `functional/test_holding_rd_wr.py` (FC03/06/16)
- [ ] `functional/test_holding_side_effect.py` (CFG_SAVE / REBOOT 等副作用寄存器)
- [ ] `functional/test_input_regs.py` (DI/AI/版本输入寄存器)
- [ ] `functional/test_timestamp_live.py` (0x0E/0x0F 实时时间戳)
- [ ] `functional/test_udp_commands.py` (UDP 0x10-0x19 命令)
- [ ] `functional/test_udp_discover.py` (GET_IP 跨网段发现)
- [ ] `functional/test_modbus_multiclient.py` (并发客户端; 上限 2 为已知偏差, 见 KNOWN_DEVIATIONS)

(RTU 用例 `test_modbus_rtu.py` 需 PC 串口接 RS485, 见 §2; CAN 用例
一期无业务应答, 不复用。)

## 2. Modbus RTU (Modbus Poll / 串口工具)

- [ ] 9600 8N1, 从站号 1 (默认): FC01 / FC03 / FC05 / FC06 / FC15 / FC16 全部通过
- [ ] 广播写 (unit 0): DO 实际生效, 主站无应答
- [ ] 改波特率 (reg 0x08) / 从站号 (reg 0x09) + 保存 (reg 0x10) 后重启: 新波特率/新从站号生效, 旧参数不再应答
- [ ] 帧间隔静默 (t3.5) 下连续轮询无误帧 (9600 时 t3.5 实际 5ms, 偏大无害, 见 KNOWN_DEVIATIONS)

## 3. 心跳 / 看门狗

- [ ] PE7 心跳 LED 3s 周期 (亮 300ms / 灭 2700ms)
- [ ] 程序挂死 (调试器断点暂停调度或注入死循环) > 30s: IWDG 自动复位, 重启后 banner 重现
- [ ] 调试器挂起 (halt) 任意时长不死机: IWDG 调试冻结生效 (__HAL_DBGMCU_FREEZE_IWDG), 恢复运行后继续

## 4. 历史记录 (littlefs @ W25Q128)

- [ ] 写使能 reg 0x05 = 1 + 保存, 重启后仍为 1
- [ ] 使能后采样写入; 断电重启后**续写同一文件** (不新建, 日志
      `history file: ... (N bytes, appending)`)
- [ ] 单文件到 1MB 轮转: 新建 `data_MMDD_HHMMSS.raw`, 旧的保留
- [ ] 保留 10 份: 超限删最旧。**盘上出现 11 份属正常** (目录扫描
      names[12] 容量截断 + 懒清理, 与 Zephyr 版一致), 第 12 份出现前
      会清到 10
- [ ] 验证方式: FTP 读回为二期; 一期用 pytest/Modbus 读回 (log 或
      复挂 NOR) 确认文件存在与大小

## 5. DO 安全 (断线保护)

- [ ] DO 置非零后拔网线: 链路下降沿 DO 全灭 (含影子寄存器 reg 0x00 清零)
- [ ] 插回网线: DO **不自动恢复** (保持全灭, 需主站重写)

## 6. UDP 配置协议 (端口 8600)

- [ ] `SET_IP` / `SET_TIME` / `SET_MODBUS`: 两步生效 (5s 内重复同一
      命令才执行, 第二步前不应答生效结果)
- [ ] `GET_IP`: 应答 `[0x11][a][b][c][d]` 5 字节, 仅当前 IP 四段
      (无 MAC/版本字段; 对齐 Zephyr 协议)
- [ ] `FACTORY_RESET`: 两步确认后恢复出厂并自动重启
- [ ] 跨网段 (源 IP 不在本机 /24 内): 仅 `GET_IP` 应答 (广播应答到
      `255.255.255.255:8601`), 其余命令静默
- [ ] 命令字 0x01-0x06 (保留/未实现): 静默无应答

## 7. Modbus TCP (端口 502)

- [ ] keepalive 30s: 拔线后约 45-90s 连接断开 (Sn_KPALVTR=6 →
      6x5s=30s 探测周期; 实际断开时间含链路层重试, 落在该区间即通过)
- [ ] 最多 2 个并发客户端: 满 2 时无 LISTEN socket, 第 3 个客户端
      连接被拒 (socket 预算 3; 已知偏差)
- [ ] unit 0 广播写: 生效且不应答
- [ ] 任意非 0 unit: 应答, MBAP unit 回显**原始请求值** (非本机从站号)

## 8. RTC

- [ ] UDP `SET_TIME` (或 Modbus 0x0E/0x0F) 设置时间后断电 (含 VBAT
      在位) 重启: 时间连续, 不回 2020-01-01 默认值
- [ ] 无 VBAT/冷备份域: 回默认 2020-01-01 (预期行为)

## 9. 版本

- [ ] Modbus `input_reg[0]` = `(0<<12 | 3<<8 | 0)` = `0x0300`
- [ ] 开机日志 banner: `version: v0.3.0_xxxxxx` (xxxxxx = git 短哈希)

## 10. 恢复出厂 (FACTORY_RESET 第二步后)

- [ ] 配置回默认: IP `192.168.12.101`, 从站号 1, 波特率 9600,
      CAN 250k / ID 0x0111, 采样 200ms, 历史使能 0
- [ ] **历史文件保留** (只擦配置区, 不格 NOR 历史分区)
- [ ] 重启后可通过默认 IP 重新连接

## 11. W5500 故障降级

- [ ] 拔掉/损坏 W5500 (或 VERSIONR 校验失败) 后设备继续本地运行:
      RTU 应答正常、无 30 秒重启循环、日志出现降级告警
      (`W5500 VERSIONR mismatch` / `W5500 init failed, network
      unavailable`)
