# 测试项清单

共 **104 项**: 主机单元测试 12 项 (ctest, 脱离硬件) + 设备 e2e 深度测试
92 项 (pytest, `tests/e2e/`, 需真机)。

## 设备 e2e 深度测试 (tests/e2e/, 92 项)

标记: `basic` 基础 / `functional` 功能 / `stress` 压力;
`serial` 需串口 / `rtu` 需 RS485 / `reboot` 会重启设备 / `upgrade` 会换机。

### 基础测试 test_basic.py (9 项, marker: basic)

| # | 测试项 | 验证内容 |
|---|--------|----------|
| 1 | test_ping | ICMP ping 可达 |
| 2 | test_tcp_ftp_banner | TCP 21 端口连接, banner 为 `220 io-edge-hub FTP service ready` |
| 3 | test_tcp_http_responds | TCP 80 端口 `/api/info` 返回 200 JSON |
| 4 | test_tcp_modbus_responds | TCP 502 端口 FC03 请求有正常响应 |
| 5 | test_udp_get_version | UDP 8600 `GET_VERSION(0x04)` 回 `vM.m.p_git`(14 字节定长) |
| 6 | test_udp_get_ip | UDP `GET_IP(0x11)` 返回的地址与 `--ip` 一致 |
| 7 | test_http_info_fields | `/api/info` 全字段: board/hclk 168MHz/flash 512K/sram 192K/ip/net_up/uptime/lfs 等 |
| 8 | test_version_consistent_across_surfaces | 版本三面一致: UDP 版本前缀 = HTTP info.version; Modbus 输入寄存器 0 = MAJOR<<12\|MINOR<<8\|PATCH |
| 9 | test_history_list_reachable | `/api/history` 可访问, 文件名 `data_*` 且 size ≥ 0 |

### FTP 功能 test_ftp.py (14 项, marker: functional, 端口 21)

| # | 测试项 | 验证内容 |
|---|--------|----------|
| 10 | test_login_pwd_syst_feat | admin 登录, PWD=/, SYST=UNIX Type: L8, FEAT 含 SIZE/PASV/EPSV/PORT/EPRT/REST STREAM/TYPE A;I/NLST/MKD/RMD |
| 11 | test_stor_size_retr_roundtrip | STOR 8KB 随机数 → SIZE 一致 → RETR 逐字节一致 |
| 12 | test_appe | APPE 追加 4 字节后 SIZE 与内容一致 |
| 13 | test_ascii_mode | TYPE A 存取 CRLF 文本行无损往返 |
| 14 | test_mkd_rmd_nlst_rename_delete | MKD 后 NLST 可见; RNFR/RNTO 后大小保持; DELE/RMD 清理 |
| 15 | test_rest_resume_stor | STOR 前半 + REST 2048 + STOR 后半 = 原文件 |
| 16 | test_rest_retr_offset | REST 1024 后 RETR 只取后半段 |
| 17 | test_epsv_manual | 手工 EPSV 握手: 解析 229 端口, 数据连接收满 4096B + 150/226 应答 |
| 18 | test_port_active_mode | PORT 主动模式: 设备反向连到测试机监听端口取数 |
| 19 | test_path_traversal_neutralized | `..` 被钳制在根内 (STOR ../x 落在 /x); 越界读/删/查返回 550 |
| 20 | test_anonymous_readonly | anonymous 可登录可读; STOR/DELE/MKD 一律 530 |
| 21 | test_wrong_password_rejected | admin 错误密码 530 |
| 22 | test_unknown_command_502 | 未知命令 502 |
| 23 | test_fourth_client_rejected | 3 会话并发正常; 第 4 个 421 Too many users |

### 固件升级 test_fw_upgrade.py (3 项, marker: functional+upgrade, ~30-90s/项)

| # | 测试项 | 验证内容 |
|---|--------|----------|
| 24 | test_upgrade_same_image | UDP 全流程: START(keyhash 校验)→DATA_V2 窗口传输→END(CRC16)→REBOOT→MCUboot 换机→同版本回线 |
| 25 | test_upgrade_bad_keyhash_rejected | 错误 keyhash 的 START 被拒且设备不受影响 |
| 26 | test_upgrade_over_ws | SPA 同款 WS 通道: fw_start→10KB 二进制帧→fw_end→自动换机重启→同版本回线 |

### 历史记录 test_history.py (5 项, marker: functional)

| # | 测试项 | 验证内容 |
|---|--------|----------|
| 27 | test_file_present_and_growing | 文件名 `data_MMDD_HHMMSS.raw`, 同一文件持续追加 |
| 28 | test_web_download_consistent | web 下载字节数 ≥ 列表 size (先 sync), 内容可按 DI=10B/AI=16B 记录流完整解析 |
| 29 | test_ftp_view_consistent | FTP RETR 与 web 视图一致 (字节 ≥ 列表 size, 记录流可解析) |
| 30 | test_disable_resume_same_file | reg5=0 停止增长; 重新使能后续写**同一文件**而非新建 |
| 31 | test_delete_fake_history_file | 伪造 `data_*.raw` 经 `/api/history/delete` 删除, 列表与 FTP 双确认 |

### Modbus RTU test_modbus_rtu.py (3 项, marker: functional+serial+rtu, 需 `--rs485-port`)

| # | 测试项 | 验证内容 |
|---|--------|----------|
| 32 | test_rtu_fc03_read | FC03 读 18 个保持寄存器, CRC16 校验通过 |
| 33 | test_rtu_fc04_version | FC04 输入寄存器 0 版本字与 UDP 版本跨通道一致 |
| 34 | test_rtu_fc06_same_value_write | FC06 原值回写, 回显 addr/value 正确 (对现场无副作用) |

### Modbus TCP test_modbus_tcp.py (14 项, marker: functional, 端口 502)

| # | 测试项 | 验证内容 |
|---|--------|----------|
| 35 | test_read_all_holding | FC03 全 18 寄存器; reg 0x09=slave id, 0x0A-0x0D=IP 四段 |
| 36 | test_read_all_input | FC04 全 6 输入寄存器 |
| 37 | test_time_registers_live | holding 0x0E/0x0F 组合的活时间与 `/api/info` 一致 (±3s) |
| 38 | test_coils_and_discrete_mirror_io | FC01 线圈 = web do 数组; FC02 离散输入 = di 数组 |
| 39 | test_fc05_coil_write_readback | FC05 写线圈 5 → 线圈与 web do 双回读, 恢复 0 |
| 40 | test_fc06_write_readback | FC06 写 reg0=0x55 → 回读与 web do 位模式双确认, 恢复 0 |
| 41 | test_fc16_write_multiple | FC16 原值多写, 回显 addr/count 且寄存器不变 |
| 42 | test_fc08_diagnostics | FC08 子功能 0x0000 回显 / 0x000A 清计数 / 0x000B 计数增长 |
| 43 | test_exceptions | 越界地址→0x02; 数量超限→0x03; 未知 FC→0x01; FP 扩展区→0x02/0x01; proto≠0→异常 |
| 44 | test_broadcast_no_reply | unit=0 广播无响应但连接存活 |
| 45 | test_truncated_pdu_silent_drop | PDU 长度不符静默丢弃, 连接存活 |
| 46 | test_pipelined_requests | 单段两帧 ADU → 两帧正确 tid/FC 响应 |
| 47 | test_max_two_masters | 2 主站并发正常; 第 3 个被拒 |

### 重启 test_reboot.py (3 项, marker: functional+reboot, ~25s/项)

| # | 测试项 | 验证内容 |
|---|--------|----------|
| 48 | test_web_reboot | POST /api/reboot → 先离线后回线; history 先落盘, **续写同一文件**且增长; 版本不变; 服务恢复 |
| 49 | test_udp_reboot | UDP `REBOOT(0x05)` 同上验证 |
| 50 | test_shell_reboot | 串口 `reboot` 命令同上验证 |

### 压力 test_stress.py (10 项, marker: stress)

| # | 测试项 | 验证内容 |
|---|--------|----------|
| 51 | test_http_churn_300 | 300 次全新 TCP 连接 GET 全部 200 (实测 p50≈31ms) |
| 52 | test_http_keepalive_burst_300 | 单连接 300 次 keep-alive 请求 (p50≈3ms) |
| 53 | test_ftp_connect_churn_100 | FTP 100 次连接/banner/断开 |
| 54 | test_modbus_burst_500 | Modbus 500 次轮询全对 (p50≈3ms) |
| 55 | test_modbus_pipelined_burst_100 | 100 组双帧流水线 |
| 56 | test_udp_burst_300 | UDP 300 连发, 回复率 ≥ 98% |
| 57 | test_ftp_three_clients_parallel | 3 客户端并行 128KB STOR/RETR/删除, md5 全对; 期间第 4 连接 421 |
| 58 | test_ftp_large_file_1mb | 1MB 随机文件 STOR/SIZE/RETR 逐字节一致 (实测 RETR≈320KB/s) |
| 59 | test_mixed_workload_30s | 30s 五协议混合负载: HTTP+Modbus+UDP+FTP+WS 零错误, 采样不中断, 无重启 |
| 60 | test_health_after_stress | 压力后全服务健康 + 历史仍在记录 |

### 串口 shell test_uart.py (6 项, marker: functional+serial, USART1 `--serial`)

| # | 测试项 | 验证内容 |
|---|--------|----------|
| 61 | test_prompt_and_help | `io> ` 提示符; help 列出 help/tasks/reboot/io |
| 62 | test_tasks | tasks 命令输出任务表 |
| 63 | test_io_info_version | `io info` 显示 v0.x 版本 |
| 64 | test_unknown_command | 未知命令提示 `unknown command` |
| 65 | test_garbage_line | 80 字符随机垃圾行后 shell 存活 |
| 66 | test_long_line_truncated | 300 字符超长行被安全截断 |

### UDP 配置协议 test_udp.py (8 项, marker: functional, 端口 8600)

| # | 测试项 | 验证内容 |
|---|--------|----------|
| 67 | test_get_modbus_matches_info | GET_MODBUS(0x13) 的 slave/baud 与 `/api/info` 一致 |
| 68 | test_set_ip_invalid_rejected | SET_IP 非法地址 (0/127/≥224 首段, 0/255 末段) 一律 ok=0 且状态不变 |
| 69 | test_set_ip_same_value_accepted | SET_IP 写当前地址 ok=1, GET_IP 不变 |
| 70 | test_set_time_invalid_rejected | SET_TIME 超界 (0/TS_MIN-1/TS_MAX+1) ok=0 |
| 71 | test_set_time_valid_and_readback | SET_TIME 当前时间 ok=1, `/api/info` 回读 ±10s |
| 72 | test_set_modbus_same_value_accepted | SET_MODBUS 原值写入 ok=1, GET_MODBUS 不变 |
| 73 | test_unknown_command_silent | 未知命令码静默无响应 |
| 74 | test_factory_reset_first_step_rejected | FACTORY_RESET(0x19) 首步 ok=0 且设备不被复位 |

### Web/HTTP test_web.py (17 项, marker: functional, 端口 80)

| # | 测试项 | 验证内容 |
|---|--------|----------|
| 75 | test_index_page_gzip | GET / 返回 gzip 页 (>20KB, Content-Encoding 正确) |
| 76 | test_keepalive_two_requests | 同连接连续两请求均 200 |
| 77 | test_api_io_shape | `/api/io` 结构: di×16/do×8/ai×4 |
| 78 | test_api_regs_shape | `/api/regs` 结构: holding×18/input×6 |
| 79 | test_do_roundtrip | /api/do 0→1→0 写读回环 |
| 80 | test_do_invalid | index 越界/缺失/非法 → 400 |
| 81 | test_reg_invalid | addr 越界/value 超限/缺失 → 400 |
| 82 | test_time_endpoint | /api/time 越界 400, 有效时间 ok |
| 83 | test_cfg_validation | /api/cfg: 原值写 ok; 非法 ip/485 波特/sid/can 波特/can id 12 组全拒; sid 改动恢复 |
| 84 | test_save | /api/save ok |
| 85 | test_history_download_invalid_name | 路径穿越/非法名下载 → 400 |
| 86 | test_404_and_method | 未知路径 404; 非法方法 404 |
| 87 | test_body_too_large | Content-Length>128 → 400 body too large |
| 88 | test_request_line_parser_edges | 双空格/Tab 分隔/查询串/超长路径解析正确 |
| 89 | test_pipelined_requests | 单段两请求 → 两响应 (io + regs 各自正确) |
| 90 | test_max_two_connections | 2 空闲连接占用时第 3 个被拒 |

### WebSocket test_ws.py (3 项, marker: functional)

| # | 测试项 | 验证内容 |
|---|--------|----------|
| 91 | test_ws_push_and_cmd | 101 握手; 6s 内收到 io+regs 推送; reg 命令 ack ok |
| 92 | test_ws_single_session | 已有会话时第二个 /ws 升级 → 503 ws busy |
| 93 | test_ws_close_frees_session | 关闭后立即可再次接入 |

## 主机单元测试 (tests/, 12 项, ctest)

| # | 目标 | 覆盖 |
|---|------|------|
| 1 | test_bytes_crc | 大小端读写 / CRC16 |
| 2 | test_config_store | 配置存储读写擦 |
| 3 | test_regmap | 保持/输入寄存器映射与副作用 |
| 4 | test_mb_server | Modbus 服务端功能码与异常 |
| 5 | test_mbtcp_adu | MBAP 帧解析/组包 |
| 6 | test_rtu_frame | RTU 帧状态机 |
| 7 | test_udp_cfg | UDP 配置命令处理 |
| 8 | test_fw_upg | 固件升级状态机 |
| 9 | test_sys_time | 时间门/时间换算 |
| 10 | test_lfs_port | littlefs NOR 移植层 |
| 11 | test_adc_math | ADC 换算 |
| 12 | test_history | 历史文件命名/追加/回读 |

## 运行方式

```bat
REM e2e (需真机)
pip install -r tests\e2e\requirements.txt
python -m pytest tests\e2e -m basic|functional|stress
python -m pytest tests\e2e --ip 192.168.12.101 --serial COM9 --rs485-port COM10

REM 主机单测 (无需硬件)
cmake -S tests -B build-host-win && cmake --build build-host-win --config Debug
ctest --test-dir build-host-win -C Debug --output-on-failure
```

最近一次全量实测 (2026-08-22): e2e 92/92 通过 (RTU 经 COM10),
主机 12/12 通过, 全量耗时约 3 分钟。
