# 深度测试报告

日期: 2026-08-21 ~ 2026-08-22
测试对象: io-edge-hub FreeRTOS 移植版 + Zephyr 原版 (对照)
测试套件: `tests/e2e/` (pytest, 93 项, 双固件自适应) + `tests/` 主机单测 (12 项)

## 1. 结论

| 项目 | FreeRTOS v0.3.0 | Zephyr v0.2.2_4c57a2 |
|---|---|---|
| 主机单元测试 | 12/12 通过 | — |
| e2e 全量套件 | **92/92 通过** (含 RTU) | **80 通过 / 11 有据跳过 / 2 失败** |
| 30s 五协议混合负载 | 零错误 | 零错误 |
| 深度测试发现固件 bug | 2 个 httpd bug, 已修复 (84f5f5f) | **1 个数据损坏级 bug (FTP RETR), 未修复** |

两版性能画像互补: FreeRTOS 版 HTTP/FTP 读快, Zephyr 版 Modbus TCP
尾延迟更稳。Zephyr 版存在可复现的 FTP 下载内容损坏 (见 §6), 其余
2 项失败即由此引起; FreeRTOS 移植版 (重写的单线程 select 实现)
多次复验不受影响。

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

## 6. Zephyr 侧发现的问题 (未修复, 移植版不受影响)

### 6.1 FTP RETR 数据损坏 (严重, 即 2 项失败根因)

- 现象: 大文件 RETR 返回内容与写入不符; 同一文件连续两次 RETR 内容
  互不相同, 首个差异偏移随机 (31241/57865/223497/262409/375817/684297...)
- **存储内容完好**: 同一文件经 `/api/history/download` 下载逐字节一致,
  排除写路径与 littlefs
- 复现: 全量套件后 3/3 复现; 全新重启后第一笔 1MB 传输正常,
  第二笔 (256KB) 即损坏 —— 非运行时间劣化, 为固有缺陷
- 影响: `test_ftp_three_clients_parallel` / `test_ftp_large_file_1mb`
  失败 (并发 md5 与 1MB 逐字节比对)
- FreeRTOS 移植版 ftpd 为重写的单线程 select 实现, 多轮 1MB/3 并发
  md5 校验全部通过

### 6.2 行为差异清单 (测试已按固件条件化, 均记录在案)

| 项 | FreeRTOS | Zephyr |
|---|---|---|
| HTTP API 响应 | Content-Length 短响应 | Transfer-Encoding: chunked |
| `/api/cfg` 路由 | 有 | **无** (cfg 仅 WS 命令) |
| 未知方法 (DELETE /api/io) | 404 | 405 Method Not Allowed |
| 请求行 Tab 分隔 | 200 (sscanf 语义) | 500 |
| 同段流水线两请求 | 两响应 | 不支持 (首请求即 404) |
| HTTP 并发连接上限 | 2 | 无固定上限 |
| Modbus 主站数上限 | 2 | 无上限 (4 并发全服务) |
| Modbus 未知 FC 异常码 | 0x01 | 0x04 |
| 截断 PDU | 静默丢弃 | 回错误帧 |
| UDP REBOOT 应答 | `05 01` | `05` (1 字节) |
| WS 忙 (第 2 条 /ws) | 503 ws busy | 先 101 再断连 (库先应答后回调) |
| 超长 body (>128) | 400 且不执行 | 400 但响应含两段 JSON 且 handler 仍执行 |
| sram_kb 字段 | 192 (含 CCM) | 128 |

### 6.3 过程记录

- WS 单连接槽位事件: 浏览器开着 SPA 占住 Zephyr 单 WS 槽位
  (`CONFIG_IO_WEB_WS_HANDLERS=1`), 经代理的半开连接释放不掉, 需重启清槽;
  WS 相关测试前需关闭浏览器页面
- Zephyr 版 `web /api/time` 超 int32 时间戳同样被 strtol 钳到 2038
  (与 FreeRTOS 一致); 版本串同为构建期注入

## 7. 收尾状态

- 设备已恢复烧写 FreeRTOS 固件, stress 10/10 复验通过 (FTP 1MB 内容一致)
- 历史采样使能并已保存 (产品默认态)
- 相关提交: 84f5f5f (固件修复), 3824b66/e2698c8 (测试套件),
  bbee7f5 (测试项清单), af25d9a (chunked 兼容), 本次新增双固件条件化
