# 深度测试报告

日期: 2026-08-21 ~ 2026-08-22
测试对象: io-edge-hub FreeRTOS 移植版 + Zephyr 原版 (对照)
测试套件: `tests/e2e/` (pytest, 93 项, 双固件自适应) + `tests/` 主机单测 (12 项)

## 1. 结论

| 项目 | FreeRTOS v0.3.0 | Zephyr v0.2.2_4c57a2 |
|---|---|---|
| 主机单元测试 | 12/12 通过 | — |
| e2e 全量套件 | **93/93 通过** (含 RTU/串口) | **83 通过 / 10 有据跳过 / 0 失败** |
| 30s 五协议混合负载 | 零错误 | 零错误 |
| 深度测试发现固件 bug | 2 个 httpd bug, 已修复 (84f5f5f) | **1 个数据损坏级 bug (FTP RETR), 已修复** |

两版已双向同步 (web/FTP/Modbus/升级四域行为一致, 见 §6.1), 剩余差异均为
库层/策略层 (chunked 传输、并发上限、WS 忙形态等, 见 §6.2)。性能画像:
FreeRTOS 版 HTTP/FTP 读快, Zephyr 版 Modbus TCP 尾延迟更稳。

## 2. 测试环境

- 设备: io_edge_f407vet6 (STM32F407VET6 + W5500), 静态 IP 192.168.12.101
- 测试机: Windows, Python 3.12.10 / pytest 9.1.1 / pyserial 3.5 /
  websocket-client, 物理网卡 192.168.12.150 (绑定源地址绕开本机 TUN 代理)
- 串口: COM9 (shell/日志 USART1 115200), COM10 (USB-RS485, Modbus RTU 9600)
- 烧写: ST-LINK_CLI; Zephyr 镜像 `build/output/images/full_output.bin`
  (0x08000000), 升级测试用同目录 `update.bin`
- 固件: FreeRTOS v0.3.0 (含 84f5f5f 两项修复); Zephyr v0.2.2_4c57a2
  (apps 仓库 HEAD, 2026-08-22 18:26 构建)
- 测试套件对双固件自适应: `/api/info` 的 board 字段区分固件类型,
  行为差异按 `fw_kind` 条件断言; Zephyr 的 chunked HTTP 响应已兼容 (af25d9a)

## 3. e2e 结果明细 (FreeRTOS, 92/92)

| 模块 | 项数 | 结果 | 要点 |
|---|---|---|---|
| 基础连通 | 9 | 通过 | ping/TCP×3/UDP 版本与 IP/信息字段/三面版本一致 |
| FTP | 14 | 通过 | 全命令集, 421 上限, 匿名只读, PASV/EPSV/PORT, REST/APPE/ASCII, `..` 钳制 |
| Web/HTTP | 17 | 通过 | 全路由, 解析边界, 流水线, 2 连接上限, gzip 首页 |
| WebSocket | 3 | 通过 | 推送/命令 ack/单会话 503/关闭释放 |
| Modbus TCP | 14 | 通过 | FC01-08/16, 异常码, 广播静默, 流水线, 2 主站上限 |
| Modbus RTU | 3 | 通过 | COM10, CRC, 版本跨通道一致 |
| UDP 配置 | 8 | 通过 | 全命令 + 非法输入拒绝 + 未知命令静默 |
| 历史记录 | 5 | 通过 | web/FTP/记录流三方一致, 停用后续写同文件 |
| 重启 | 3 | 通过 | web/UDP/shell 三路, history 先落盘再续写同文件 |
| 固件升级 | 3 | 通过 | UDP 全流程 + 坏 keyhash 拒绝 + WS 通道换机 |
| 压力 | 10 | 通过 | 见 §5 |
| 串口 shell | 6 | 通过 | 提示符/命令/垃圾行/超长行 |

## 4. FreeRTOS 修复的固件 bug (84f5f5f)

1. **WS 忙时 503 被 404 覆盖**: `/ws` 忙路径 respond 503 后缺 `return`,
   落入 `dispatch()` 被通用 404 覆盖。由 `test_ws_single_session` 发现。
2. **同段流水线 HTTP 请求被丢弃**: `conn_pump` 响应完成时先 `rx_len=0`
   再判断流水线残留。由 `test_pipelined_requests` 发现。

## 5. 压力测试双固件对比

| 指标 | FreeRTOS v0.3.0 | Zephyr v0.2.2 |
|---|---|---|
| HTTP 300 连接风暴 p50/p95 | **35 / 54 ms** | 81~105 / 94~118 ms |
| HTTP keep-alive 300 次 p50/p95 | **3 / 13 ms** | 56 / 66 ms |
| FTP 100 连接风暴 | 通过 | 通过 |
| Modbus 500 轮询 p50/p95 | 3.0 / 12.0 ms | **2.4~2.7 / 3.5~12 ms** |
| Modbus 100 组流水线 | 通过 | 通过 |
| UDP 300 连发 (丢包 ≤2%) | 通过 | 通过 |
| FTP 3 并发 128KB (md5) | 通过 | **失败 (数据损坏, 见 §6)** |
| FTP 1MB STOR / RETR | 42 / 318~321 KB/s, 内容一致 | 45 / 230 KB/s, **内容损坏** |
| 30s 混合负载 (http/mb/udp/ftp/ws 帧) | 165~167/264~266/96/15/60~62 全零错误 | 130~132/263~266/96/15/60~64 全零错误 |
| 压力后健康 | 通过 | 通过 |

解读: **Modbus TCP** Zephyr 尾延迟更稳 (独立线程事件驱动);
**HTTP** FreeRTOS 快一个量级 (原生 socket + 短 Content-Length 响应 vs
Zephyr chunked + 完整 TCP 栈开销); **FTP** 上传同为 NOR 写入瓶颈
(~42-45KB/s), 下载 FreeRTOS 快 ~40% 且内容可靠。

## 6. Zephyr 侧发现的问题 (已全部修复)

### 6.1 FTP RETR 数据损坏 (严重, 根因与修复)

- 现象: 大文件 RETR 内容错位; 接收流比原文件**短** (如 1MB 少 2223B),
  损坏区内容在原文件后部重现 —— 即字节被**跳过**
- 根因: `send()` 在 TX 缓冲紧张时只接受部分字节, 原代码忽略返回值,
  尾部字节静默丢失 (存储与 littlefs 完好, web 下载逐字节一致可证)
- 修复: 数据连接全部改走重试到发完的 `send_all()`; RETR 读缓冲改静态。
  FTP 线程栈经 `kernel thread stacks` 实测峰值 2904B, 维持原 4096
  (一度试改 8192 把主 SRAM 推到 95.2%, 实测后回收, 回到 92.2%)
- 复验: 1MB×3 轮逐字节一致, stress FTP 3 并发 + 1MB md5 全过

### 6.2 双向同步 (保证 web/FTP/Modbus/升级四域一致)

| 项 | 同步方向 | 处理 |
|---|---|---|
| `/api/cfg` 路由 | FreeRTOS → Zephyr | Zephyr 补路由 (复用 WS cfg 执行器) |
| 未知方法 (DELETE /api/io) | Zephyr → FreeRTOS | 两边均 405 (RFC 语义) |
| Modbus 未知 FC 异常码 | FreeRTOS → Zephyr | 两边均 0x01 ILLEGAL FUNCTION |
| 截断 PDU | FreeRTOS → Zephyr | 两边均静默丢弃 (原 Zephyr 超时后回 0x04) |
| UDP REBOOT 应答 | FreeRTOS → Zephyr | 两边均 `05 01` |
| 超长 body (>128) | FreeRTOS → Zephyr | Zephyr 修复为单次 400 且不执行 handler |
| sram_kb 字段 | FreeRTOS → Zephyr | 两边均 192 (含 64KB CCM) |
| FTP 传输完整性 | FreeRTOS → Zephyr | 见 §6.1 send_all |

### 6.3 保留差异 (库层/策略层, 不做同步)

| 项 | FreeRTOS | Zephyr | 说明 |
|---|---|---|---|
| HTTP API 响应 | Content-Length | chunked | Zephyr http 库行为, 传输层等价 |
| 请求行 Tab 分隔 | 200 | 500 | 解析器宽容度, 非功能项 |
| 同段流水线 | 支持 | 404 | Zephyr 库限制 |
| HTTP 并发上限 / Modbus 主站数 | 2 / 2 | 无上限 | 资源策略 |
| WS 忙形态 | 503 ws busy | 先 101 再断连 | Zephyr 库先应答后回调 |
| 404 响应体 | JSON | 纯文本 | 移植版增强 |
| 串口 shell | io> 自研 shell | Zephyr 原生 shell | 各自生态 |

### 6.4 过程记录

- WS 单连接槽位事件: 浏览器开 SPA 占住 Zephyr 单 WS 槽位
  (`CONFIG_IO_WEB_WS_HANDLERS=1`), 半开连接需重启清槽
- WS 升级测试竞态 (已修): fw_end 走 ~3s 延迟重启, wait_online 会在
  重启前抢答, 20s swap 落到下一模块 —— 现先等离线再等回线
- 两版 `web /api/time` 超 int32 时间戳均被 strtol 钳到 2038 (行为一致,
  与 Zephyr 原版相同); 版本串均为构建期注入
- RAM 余量核查: Zephyr littlefs 挂载/首次格式化需 malloc 2×1024+32≈2.1KB
  (每打开一个文件再 +1KB), 走 k_malloc 的**固定 16KB 系统堆**
  (CONFIG_HEAP_MEM_POOL_SIZE, 已计入 .bss 统计), 不受"主 SRAM 95%"影响;
  挂载发生在 SYS_INIT 早期 (网络服务启动前), 堆占用极低, 无失败风险

## 7. 收尾状态

- 设备烧写 FreeRTOS 固件 (含 405 同步), 全量 93/93 通过
- Zephyr 侧同步修改经全量 83+10+0 验证 (west archive 构建)
- 历史采样使能并已保存 (产品默认态)
- 相关提交: 84f5f5f (FreeRTOS httpd 修复), 3824b66/e2698c8 (测试套件),
  bbee7f5 (测试项清单), af25d9a (chunked 兼容), 8eb8369 (双固件适配),
  本次新增: FreeRTOS 405 + 测试更新, apps 仓 Zephyr 同步修复
