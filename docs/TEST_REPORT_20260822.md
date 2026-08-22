# 深度测试报告

日期: 2026-08-21 ~ 2026-08-22
测试对象: io-edge-hub FreeRTOS 移植版 + Zephyr 原版 (对照)
测试套件: `tests/e2e/` (pytest, 92 项) + `tests/` 主机单测 (12 项)

## 1. 结论

| 项目 | 结果 |
|---|---|
| 主机单元测试 | **12/12 通过** (MSVC) |
| e2e 深度测试 (FreeRTOS v0.3.0) | **92/92 通过** (含 COM10 Modbus RTU), 全量 ~3 分钟 |
| e2e 压力测试 (Zephyr v0.2.2_4c57a2) | **10/10 通过** (同一套套件) |
| 深度测试发现固件 bug | **2 个, 均已修复并上机验证** (84f5f5f) |
| 30s 五协议混合负载 | 两版均**零错误**, 采样不中断, 无重启 |

两版性能画像互补: FreeRTOS 版 HTTP/FTP 读快 (原生 socket 短响应),
Zephyr 版 Modbus TCP 尾延迟更稳 (p95 3.5ms vs 12ms)。

## 2. 测试环境

- 设备: io_edge_f407vet6 (STM32F407VET6 + W5500), 静态 IP 192.168.12.101
- 测试机: Windows, Python 3.12.10 / pytest 9.1.1 / pyserial 3.5 /
  websocket-client, 物理网卡 192.168.12.150 (绑定源地址绕开本机 TUN 代理)
- 串口: COM9 (shell/日志 USART1 115200), COM10 (USB-RS485, Modbus RTU 9600)
- 烧写: ST-LINK_CLI; Zephyr 镜像 `build/output/images/full_output.bin` (0x08000000)
- 固件版本:
  - FreeRTOS: v0.3.0, 含 httpd 两项修复 (84f5f5f)
  - Zephyr: v0.2.2_4c57a2 (apps 仓库 HEAD, 2026-08-22 18:26 构建)

## 3. e2e 覆盖与结果 (FreeRTOS, 92 项)

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

## 4. 发现并修复的固件 bug (FreeRTOS httpd.c, 84f5f5f)

1. **WS 忙时 503 被 404 覆盖**: `/ws` 忙路径 respond 503 后缺 `return`,
   落入 `dispatch()` 被通用 404 覆盖; SPA 收到 404 而非 503。
   由 `test_ws_single_session` 发现。
2. **同段流水线 HTTP 请求被丢弃**: `conn_pump` 响应完成时先 `rx_len=0`
   再判断流水线残留, 第二个请求永远得不到响应。
   由 `test_pipelined_requests` 发现。

两者修复后均上机复验, 全量回归通过。

## 5. 压力测试双固件对比 (同一套件, 同一测试机)

| 指标 | FreeRTOS v0.3.0 | Zephyr v0.2.2 |
|---|---|---|
| HTTP 300 连接风暴 p50/p95/max | **35 / 54 / 136 ms** | 81 / 94 / 129 ms |
| HTTP keep-alive 300 次 p50/p95 | **3 / 13 ms** | 56 / 66 ms |
| FTP 100 连接风暴 | 通过 | 通过 |
| Modbus 500 轮询 p50/p95 | 3.0 / 12.0 ms | **2.4 / 3.5 ms** |
| Modbus 100 组流水线 | 通过 | 通过 |
| UDP 300 连发 (丢包 ≤2%) | 通过 | 通过 |
| FTP 3 并发 128KB (md5) | 通过 | 通过 |
| FTP 1MB STOR / RETR | 42 / **321 KB/s** | 45 / 230 KB/s |
| 30s 混合负载 (http/mb/udp/ftp/ws 帧) | 167/266/96/15/60 全零错误 | 132/266/96/15/64 全零错误 |
| 压力后健康 | 通过 | 通过 |
| 全模块耗时 | 89s | 123s |

解读:
- **Modbus TCP**: Zephyr 尾延迟显著更稳 (p95 3.5ms), 得益于独立线程 +
  事件驱动收发; FreeRTOS 版 p95 12ms 来自单连接串行 + 调度节拍。
- **HTTP**: FreeRTOS 版快一个量级 (keep-alive 3ms vs 56ms)。Zephyr 用
  chunked 传输 + 完整 TCP 栈开销, FreeRTOS 为原生 socket + 短 Content-Length
  响应、单次内存拷贝。
- **FTP**: 上传同为 NOR 写入瓶颈 (~42-45KB/s); 下载 FreeRTOS 快 ~40%。
- 两版在并发上限 (FTP 3 客户端/HTTP 2 连接/Modbus 2 主站) 与
  数据完整性 (md5/记录流校验) 上行为一致。

## 6. 过程记录

- **WS 单连接槽位事件 (Zephyr)**: 首轮混合负载 ws=0 帧, 根因是浏览器
  开着 SPA 页面占住单连接 WS 槽位 (`ws_io_setup` 忙则拒绝), 且 ws 线程
  无空闲超时, 经代理的半开连接释放不掉; 重启清槽后 8s 收到 18 帧,
  复测通过 (ws=64)。FreeRTOS 移植版语义相同。WS 测试前需关闭浏览器
  页面或重启设备。
- **双固件协议差异 (测试兼容性处理)**: Zephyr httpd 为 chunked 响应、
  状态行无理由短语 (`HTTP/1.1 200`), `/api/info` 的 board/sram_kb 字段
  值不同 (128 vs 192), UDP REBOOT 应答 1 字节 (FreeRTOS 为 `05 01`)。
  测试助手已兼容 chunked (af25d9a)。
- **已知非问题**: web `/api/time` 传超 int32 时间戳会被 strtol 钳到
  2038 (UDP 路径有完整范围校验, web 路径与 Zephyr 行为一致);
  版本串 git 段为 CMake 配置期注入, 增量编译不刷新 (当前显示 538a9b)。

## 7. 收尾状态

- 设备已恢复烧写 FreeRTOS 固件, stress 10/10 复验通过
- 历史采样使能并已保存 (产品默认态)
- 相关提交: 84f5f5f (固件修复), 3824b66/e2698c8 (测试套件),
  bbee7f5 (测试项清单), af25d9a (chunked 兼容)
