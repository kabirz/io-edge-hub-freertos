# io-edge-hub FreeRTOS 移植设计（第一期）

日期：2026-08-19
状态：已获用户批准
参考实现：`apps` 仓库（west 工作区 `C:\Users\jxwaz\code\app`）中的 Zephyr 版
`applications/io-edge-hub`（v0.2.2，板卡 `io_edge_f407vet6`，STM32F407VET6）

## 1. 背景与目标

将 io-edge-hub（数据采集卡固件）从 Zephyr 迁移到 FreeRTOS + STM32 HAL，构建系统使用
[ObKo/stm32-cmake](https://github.com/ObKo/stm32-cmake)。目标是在同一块硬件上用
FreeRTOS 实现与现有 release 固件对等的核心功能，**对外协议与寄存器映射保持二进制兼容**，
现有上位机和 pytest 测试套件无需改动。

### 非目标（第一期明确不做）

- bootloader 与一切 OTA/MCUboot 相关功能（UDP 升级命令 0x01–0x06 返回"不支持"）
- Web 服务器 / WebSocket / REST API / 内嵌页面
- FTP 服务器
- shell、Zephyr logging 子系统（仅保留可编译裁剪的 printf 串口日志）
- CAN 固件升级、CAN rescue bootloader

### 已批准的决策记录

| # | 决策 | 选择 |
|---|---|---|
| D1 | 项目位置 | 独立新仓库 `C:\Users\jxwaz\code\io-edge-hub-freertos` |
| D2 | 启动/升级 | 无 bootloader，app 从 0x08000000 直跑，SWD 烧录 |
| D3 | FTP | 第一期不纳入 |
| D4 | 依赖引入 | git submodule 浅克隆 |
| D5 | CAN | 纳入第一期。源码核查后修正范围：现版固件**没有** CAN 周期推送（`mod_can_send()` 无调用者，RX 匹配帧仅静默消费）。按行为对齐原则实现：bxCAN 按配置波特率初始化 + RX 过滤业务 ID 帧静默消费 + 提供 `mod_can_send()` API，无周期推送 |
| D6 | 工具链 | 复用 Zephyr SDK 0.17.0 的 arm-zephyr-eabi GCC 12.2（`STM32_TARGET_TRIPLET` 覆盖）；如 newlib 与 stm32-cmake 不兼容则回退 xpack arm-none-eabi |

## 2. 功能范围（第一期）

保留的对外行为（全部与 Zephyr 版对齐）：

- **Modbus TCP 从站**（端口 502）：FC 01/02/03/04/05/06/0F/10；unit-id 改写规则、
  广播（unit 0）不应答、事务 ID 匹配；TCP keepalive 30s。
- **Modbus RTU 从站**：USART2 + RS485（DE=PA1），波特率/从站号可配并持久化。
- **UDP 配置协议**（端口 8600）：应用命令 0x10+（SET/GET IP、SET/GET MODBUS、
  SET_TIME、两步确认 FACTORY_RESET）；跨网段广播应答白名单行为保持。
- **历史记录**：`his_data` 文件格式（DI 10B + AI 16B，小端，RT-Thread 兼容）、
  单文件 1MB 轮转、保留 10 份、`/data_MMDD_HHMMSS.raw` 命名。
- **IO**：16 路_DI（采样周期由保持寄存器 0x03 配置，10ms–5s，默认 200ms）、
  8 路 DO + 8 路 DO 指示灯（跟随）、4 路 AI（12bit，PC0–PC3，系数 7414/3704）、
  网络断开时 DO 安全清零。
- **CAN**：按配置波特率（reg 0x07，合法集合 {50,100,125,250,500,800,1000}×1000，否则回退 250k）初始化 bxCAN；RX 硬件过滤业务 ID（reg 0x06，默认 0x0111）帧并静默消费（对齐现版）；提供 `mod_can_send()` 发送 API；无周期推送（源码核查：现版无此行为）。
- **系统**：IWDG 30s（调试挂起时暂停）、RTC（LSE）+ 时间设置、状态灯心跳 300/2700ms、
  延迟重启（RTC 备份寄存器存标志）、恢复出厂（擦除配置与历史区后重启）、
  MAC 由 STM32 UID 派生（OUI 00:08:DC）、静态 IPv4（IP 来自保持寄存器）。
- **寄存器映射完全不变**：18 保持 + 6 输入，权威定义见 Zephyr 版
  `applications/io-edge-hub/include/init.h`；所有写入路径（Modbus/UDP/内部）
  收敛到 `io_write_holding()` / `io_write_do_bit()`，该架构原样保留。
- **版本上报**：输入寄存器中的版本打包值 + git 版本串，由构建系统从 `VERSION`
  文件与 git 注入（对应 Zephyr 的 `APP_VERSION_*` / `fw_gitver.h`）。

## 3. 硬件资源（来自 `boards/io_edge_f407vet6.dts` 与 app overlay，权威来源）

| 资源 | 引脚/参数 | 用途 |
|---|---|---|
| 时钟 | HSE 13MHz → PLL 168MHz；LSE 32.768kHz | 系统时钟 / RTC |
| USART1 | PA9/PA10，115200 | 调试日志输出 |
| USART2 + DE | PA2/PA3，DE/RE=PA1 | RS485 Modbus RTU |
| CAN1 | PA11/PA12 | 业务帧推送 |
| SPI2 + W5500 | SCK PB13 / MISO PB14 / MOSI PB15 / CS PB12 / RST PD0 / INT PD1，21MHz | 以太网（硬件协议栈） |
| SPI1 + W25Q128 | PA5/PA6/PA7，CS PA4，42MHz，16MB NOR | 历史文件 + 配置存储 |
| ADC1 IN10–13 | PC0–PC3，12bit，VREF+=3.3V | AI1/AI2 4–20mA（系数 7414）、AI3/AI4 0–10V（系数 3704） |
| 16 DI | PD3–PD6、PB5–PB11、PD2、PB0、PB1、PB3、PB4 | 光耦输入，下拉 |
| 8 DO | PD7–PD14 | 继电器输出 |
| 8 DO LED | PE8–PE15 | DO 指示 |
| 状态 LED | PE7 | 心跳 |
| IWDG | 30s | 看门狗 |

引脚/通道/系数从 devicetree 改为 `src/board/` 下的手写 C 表，不再有 devicetree 机制。

## 4. 外部依赖（submodule，浅克隆，位于 `deps/`）

| 依赖 | 上游 | 说明 |
|---|---|---|
| stm32-cmake | ObKo/stm32-cmake | 构建集成、链接脚本生成、hex/size |
| STM32CubeF4 | STMicroelectronics/STM32CubeF4 | 只使用 `Drivers/`（HAL + CMSIS），不用其中间件 |
| FreeRTOS-Kernel | FreeRTOS/FreeRTOS-Kernel | ARM_CM4F 端口 + heap_4 |
| littlefs | littlefs-project/littlefs | 文件系统（掉电安全，历史文件依赖） |
| ioLibrary_Driver | Wiznet/ioLibrary_Driver | W5500 驱动与 socket API |

`FreeRTOSConfig.h`、`stm32f4xx_hal_conf.h` 由本仓库提供。HAL 时基用 TIM
（SysTick 归 FreeRTOS）。

## 5. 工程结构

```
io-edge-hub-freertos/
├── CMakeLists.txt          # 工具链、find_package、目标 STM32F407VET6、host 测试
├── VERSION                 # 三段式版本，构建时注入
├── FreeRTOSConfig.h
├── stm32f4xx_hal_conf.h
├── deps/                   # 五个 submodule
├── src/
│   ├── main.c              # 显式初始化顺序（替代 Zephyr SYS_INIT）
│   ├── board/              # 时钟、引脚表、UID→MAC、外设句柄集中定义
│   ├── io/                 # dio.c adc.c
│   ├── modbus/             # function.c mb_server.c tcp.c rtu.c
│   ├── history/            # history.c
│   ├── net/                # w5500.c net_cfg.c udp_cfg.c can.c（CAN 归入通道类）
│   ├── storage/            # w25qxx.c littlefs_glue.c config_store.c
│   ├── sys/                # time.c watchdog.c log.c reboot.c
│   └── util/               # crc16.c bytes.c
└── tests/                  # 主机端单元测试（见 §9）
```

## 6. 平台层关键设计

### 6.1 内核

- 原生 FreeRTOS API，不用 CMSIS-OS 封装（stm32-cmake 官方也提示 Cube 的 CMSIS
  封装可能与新内核不兼容）。
- 线程与队列全部**静态创建**（对齐 `K_THREAD_DEFINE` 的静态性、便于栈审计），
  heap_4 仅 ~16KB 兜底 newlib。
- tick 1kHz，`configUSE_16_BIT_TICKS=0`，维持毫秒语义（对齐 `k_uptime`）。
- 优先级数值取反：Zephyr 数值小=优先级高，FreeRTOS 数值大=优先级高。
- 中断优先级：`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY=5`；使用 FreeRTOS
  API 的 ISR（UART RX、CAN RX、SPI DMA 完成等）抢占优先级数值 ≥5，数值 <5 的
  中断内禁止调用 FreeRTOS/HAL 带锁 API。实现时专项核查每处 NVIC 配置。
- 栈大小参考 Zephyr 版 Kconfig 各线程配置，落实到 FreeRTOSCreate 静态 TCB。

### 6.2 初始化顺序（替代 13 个 SYS_INIT）

```
复位 → SystemInit → HAL_Init(TIM 时基) → 时钟 168MHz
→ 板级引脚/外设句柄初始化
→ config_store 加载（含出厂默认）
→ RTC 时间读取
→ W25Qxx 探测 → littlefs 挂载（失败则格式化）
→ W5500 初始化 + IP/MAC 配置 → socket 池建立
→ 启动任务：DI 采样、ADC 采样、历史落盘、Modbus TCP、Modbus RTU、UDP 配置、CAN 推送
→ IWDG 启动（30s）
→ 主循环：喂狗 + 心跳灯 + 延迟重启检查
```

顺序原则与 Zephyr 版 SYS_INIT 级别一致：配置先于使用者、存储先于历史、网络最后。

### 6.3 网络（W5500 / ioLibrary）

- SPI2 阻读 + 片选控制，实现 ioLibrary 的 SPI 回调；RST 引脚复位时序；INT 引脚
  第一期不使用（轮询 socket 状态寄存器）。
- **Socket 池**（共 8 个，第一期用 4）：UDP 配置 ×1、Modbus TCP 监听 ×1、
  Modbus TCP 已连接客户端 ×2。池由 `net/w5500.c` 集中分配，未来 FTP/Web 预留 4 个。
- 缓冲区默认 2KB/socket（16KB/32KB），Modbus 数据 socket 可调 4KB。
- TCP keepalive：accepted socket 写 `Sn_KPALVTR=6`（30s）。**已知偏差（记录）**：
  (1) W5500 为单一硬件定时探测，现版 Zephyr 为 30s 空闲 + 5s 间隔 × 3 次探测，
  探测节奏略有差异；(2) Modbus TCP 并发客户端上限 2（W5500 socket 硬约束；
  现版为动态链表无上限）。
- Modbus TCP 用每 socket 非阻塞状态机轮询（不用 select）。
- 静态 IP：UID→MAC 写 SHAR；IP 写 SIPR（来源：保持寄存器 4 个八位组）；
  掩码/网关写 SUBR/GAR（取值方式以 Zephyr 版 `main.c` 为准）。
- IP 变更生效路径与 Zephyr 版一致：写寄存器 → 保存配置 → 重启网络（或整机重启）。

### 6.4 Modbus

- `function.c`（寄存器核心 + 写收敛 + 配置持久化挂钩）近乎原样移植，仅替换
  settings 调用为 config_store、日志宏替换。
- 新写 `mb_server.c`（~400 行）：FC 01/02/03/04/05/06/0F/10 解码、异常码、
  读写作到 `function.c` 模型；TCP（MBAP 头处理）与 RTU（CRC16、地址校验）
  共用同一分发层。
- RTU 传输：USART2 RX 中断逐字节 + IDLE 帧边界 + t3.5 软件定时确认；
  发送前置位 DE、TX 完成后复位。波特率/从站号修改的持久化与生效路径
  （是否需重启）以 Zephyr 版 `rtu.c`/`function.c` 实现为准，移植时对齐。

### 6.5 存储（W25Q128 / littlefs / config_store）

- `w25qxx.c`（~250 行）：JEDEC 探测、读、页编程、4K/32K/64K 擦除。
- `littlefs_glue.c`：littlefs 的 read/prog/erase/sync 回调 + 分区边界。
- **NOR 布局**：`0x000000–0x0FFFFF` 预留（将来升级镜像用，现在不使用）、
  `0x100000–0x10FFFF` 配置区 64KB、`0x110000–0xFFFFFF` littlefs（~14.9MB，历史文件）。
- `config_store.c`（~150 行）：替代 Zephyr settings/FCB。单一配置结构体
  （di_en、ai_en、di_si、ai_si、his、can_id、can_bps、rs485_bps、slave_id、ip[4]）+
  CRC32 + 世代号，配置区 A/B 双槽写入，加载时取合法且最新的一槽，两槽皆坏用出厂默认。
- 历史文件语义不变：1MB/文件、保留 10 份、掉电安全靠 littlefs。

### 6.6 其他系统服务

- `time.c`：RTC ↔ 应用 epoch，SET_TIME/寄存器时间戳共用；日志时间戳取自此处。
- `watchdog.c`：IWDG 30s，`__DBGMCU_FREEZE_IWDG`（调试挂起暂停）。
- `log.c`：printf 风格到 USART1，带 RTC 时间戳，级别编译期裁剪（release 关闭）。
- `reboot.c`：`NVIC_SystemReset`；延迟重启标志为普通 RAM `volatile bool`（源码核查：现版即如此，非备份寄存器）；恢复出厂 = 仅擦除配置区 A/B 两槽（对齐现版只擦 FCB `storage_partition`，**不清历史文件**），随后 `history_sync()` + 100ms + 冷重启。
- 版本：构建时由 CMake 读 `VERSION` + `git describe` 生成 `fw_version.h`。

## 7. 数据流（与 Zephyr 版一致）

```
DI 任务 ─┐
ADC 任务 ─┼→ 寄存器模型（function.c，互斥锁保护）
CAN 推送 ─┘        │
                   ├→ Modbus TCP / RTU 读
                   ├→ UDP 配置
                   └→ history 队列 → 专职任务写 littlefs
```

网络断开（链路状态轮询 W5500 PHY）→ DO 安全清零；回链后的行为以 Zephyr 版
`dio.c`/`function.c` 实现为准，移植时对齐。

## 8. 与 Zephyr 版的 API 映射速查

| Zephyr | FreeRTOS/HAL 方案 |
|---|---|
| k_thread / K_THREAD_DEFINE | xTaskCreateStatic |
| k_msgq | xQueue（静态） |
| k_sem / k_mutex | xSemaphoreBinary / xSemaphoreMutex |
| k_work + 专用 workq | 专职任务 + 队列 |
| k_msleep / k_uptime | vTaskDelay / xTaskGetTickCount |
| SYS_INIT | main() 显式顺序 |
| gpio/adc/rtc/iwdg/can/spi 驱动 | HAL 对应外设 |
| BSD socket（Zephyr net） | ioLibrary socket |
| net_mgmt / net_if | W5500 寄存器直写 + PHY 状态轮询 |
| settings/FCB | config_store（A/B + CRC） |
| fs_*（littlefs VFS） | littlefs 原生 lfs_* API |
| LOG_* | log.h 宏（可裁剪） |
| sys_reboot / retained | NVIC_SystemReset / RTC 备份寄存器 |
| hwinfo UID | HAL UID 寄存器 |
| sys_get_be16/put_le32 等 | util/bytes.c |

## 9. 验证策略

1. **构建验证（本机执行）**：arm-zephyr-eabi 全量编译 + size（预算：flash < 300KB、
   SRAM < 128KB，均远低于 512K/192K）。
2. **主机端单元测试（本机执行，CMake host target）**：
   `mb_server` FC 编解码与异常、`config_store` A/B 切换/CRC 损坏/出厂默认、
   UDP 配置协议状态机（含两步恢复出厂）、littlefs RAM 后端上的历史轮转。
3. **上机验收（用户执行，仓库提供 checklist 文档）**：
   现有 pytest 套件中 Modbus TCP 与 UDP 用例直接复用（协议不变）；RTU 回环、
   CAN 抓包、历史断电恢复、看门狗、DO 安全清零等手工项逐条列出。

## 10. 风险与缓解

| 风险 | 缓解 |
|---|---|
| arm-zephyr-eabi 的 newlib 与 stm32-cmake 假设不符 | D6 回退 xpack；构建脚本两者可切换 |
| FreeRTOS 中断优先级配置错误（静默断言/死机） | §6.1 专项规则 + 单元评审时逐个 NVIC 检查 + `configASSERT` 开启 |
| W5500 socket 语义差异（半关闭、对端 RST 的状态迁移） | Modbus TCP 状态机按 Sn_SR 全状态处理；上机验收含异常断连项 |
| RTU t3.5 与 IDLE 检测在高波特率下的边界 | 以字节间超时兜底；RTU 验收用 Modbus Poll/主机工具回归 |
| littlefs 挂载损坏（首次擦除不完整等） | 挂载失败自动格式化 + 验收含断电恢复项 |
| 与 Zephyr 版行为漂移 | 协议权威以 Zephyr 版源码为准，移植不改语义；pytest 复用兜底 |

## 11. 二期预留（本期只留缝隙，不实现）

- socket 池预留 4 个空闲位给 FTP（2）与 Web/WS（2）。
- NOR 前 1MB 预留升级镜像区。
- UDP 协议 0x01–0x06 命令号保留，返回"不支持"，未来接自研升级流程。
- `fw_upgrade_state` 三通道互斥模型在二期引入，本期无升级通道。
