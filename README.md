# io-edge-hub-freertos

io-edge-hub 的 FreeRTOS 移植一期 (裸 STM32, 无 Zephyr)。目标: 在
STM32F407VET6 上以 FreeRTOS + STM32 HAL + W5500 原样复刻 Zephyr 版
(apps 仓库 `applications/io-edge-hub`) 的对外行为 —— Modbus TCP/RTU
从站、UDP 配置协议、DI/DO/AI 采样、CAN、历史记录 (littlefs)、RTC、
心跳/看门狗、**MCUboot 双镜像 + 多通道固件升级** (UDP/CAN/WebSocket/
boot CAN 紧急救援)。协议帧格式与寄存器布局不变, Zephyr 版上位机/pytest
用例可直接复用。与 Zephyr 版的实现差异汇总见
[docs/KNOWN_DEVIATIONS.md](docs/KNOWN_DEVIATIONS.md), 上机验收步骤见
[docs/ACCEPTANCE.md](docs/ACCEPTANCE.md)。

## 硬件

| 部件 | 说明 |
|---|---|
| MCU | STM32F407VET6, 168 MHz, 512KB flash / 192KB RAM |
| 以太网 | W5500 (硬件协议栈, SPI2), MAC 由 MCU 96-bit UID 派生 |
| NOR | W25Q128 16MB SPI NOR (配置 + littlefs 历史分区) |
| RS485 | Modbus RTU 从站 (USART2, 默认 9600 8N1, 从站号 1) |
| RTC | LSE + VBAT 备份域 |
| 心跳 LED | PE7, 300ms 亮 / 2700ms 灭 (3s 周期) |
| 日志 | USART1 115200 8N1 (PA9 TX) |

默认网络: 静态 IP `192.168.12.101/24` (网关 `x.x.x.1`), Modbus TCP 端口
502, UDP 配置端口 8600。

## 目录结构

```
CMakeLists.txt          双镜像构建: boot.elf (MCUboot 域) + fw.elf (app 域)
FreeRTOSConfig.h        FreeRTOS 内核配置 (168MHz / tick 1ms / 优先级 7 级 0-6, 当前用到 6)
stm32f4xx_hal_conf.h    HAL 模块裁剪
VERSION                 版本号 (当前 0.3.0)
src/
  main.c                初始化序列 + 心跳/喂狗任务 (Zephyr main+SYS_INIT 合并)
  boot/                 MCUboot 引导 (裸机 main) + bxCAN 紧急救援 + 移植层
  fw/                   固件升级核心 (fw_upg) + UDP/CAN 通道
  board/                时钟/GPIO/SPI2 底板 + HAL tick (TIM7)
  sys/                  日志/时间(RTC)/看门狗(IWDG)/重启/syscalls/os 锁
  util/                 字节序 + CRC16
  storage/              W25Qxx 驱动 / 片内 flash 驱动 / 配置存储 / littlefs 移植层
  history/              历史记录 (纯文件核心 + FreeRTOS 任务壳)
  modbus/               寄存器模型 / PDU 服务器 / TCP / RTU 传输
  net/                  W5500 网络层 / UDP 配置协议 / CAN
  io/                   DI/DO/ADC 采样
  web/                  HTTP 服务器 / WebSocket 实时通道 / 升级 UI 页面
  include/              对外头文件
tests/                  主机 (Windows/WSL) 单元测试, 12 个目标, ctest
tools/                  签名/升级/烧录 python 工具 (见下)
deps/                   子模块: stm32-cmake / FreeRTOS-Kernel / littlefs /
                        ioLibrary(W5500) / STM32CubeF4 / mcuboot(bootutil
                        + ext/mbedtls)
docs/                   ACCEPTANCE.md (上机验收) / KNOWN_DEVIATIONS.md (已知偏差)
```

## 依赖初始化

```bash
git submodule update --init --recursive
```

必须带 `--recursive`: `deps/STM32CubeF4` 自身还有嵌套子模块
(`Drivers/STM32F4xx_HAL_Driver`、`Drivers/CMSIS/Device/ST/STM32F4xx`,
构建必需)。不带 recursive 时 HAL/CMSIS 头文件缺失, 配置阶段即报错。

## 固件构建 (Windows CMD)

工具链: Zephyr SDK 0.17.0 的 arm-zephyr-eabi GCC 12.2 (与 Zephyr 版
开发环境共用一套 SDK; 全 newlib, `STM32::NoSys` 链接)。SDK 不在 PATH
时通过 `STM32_TOOLCHAIN_PATH` / `STM32_TARGET_TRIPLET` 指定:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug ^
  -DCMAKE_TOOLCHAIN_FILE=deps/stm32-cmake/cmake/stm32_gcc.cmake ^
  -DSTM32_TOOLCHAIN_PATH=C:/Users/jxwaz/zephyr-sdk-0.17.0/arm-zephyr-eabi ^
  -DSTM32_TARGET_TRIPLET=arm-zephyr-eabi ^
  -DSTM32_CUBE_F4_PATH=deps/STM32CubeF4 ^
  -DFREERTOS_PATH=deps/FreeRTOS-Kernel
cmake --build build
```

产物: `build/boot.elf` (MCUboot 引导, 裸机) + `build/fw.elf` (app, 链接在
slot0 镜像头之后, 不可直接烧录 —— 须经签名流水线)。

Release 构建 (整体关闭日志, `LOG_*` 编译为空):

```bat
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE=deps/stm32-cmake/cmake/stm32_gcc.cmake ^
  -DSTM32_TOOLCHAIN_PATH=C:/Users/jxwaz/zephyr-sdk-0.17.0/arm-zephyr-eabi ^
  -DSTM32_TARGET_TRIPLET=arm-zephyr-eabi ^
  -DSTM32_CUBE_F4_PATH=deps/STM32CubeF4 ^
  -DFREERTOS_PATH=deps/FreeRTOS-Kernel ^
  -DAPP_RELEASE=ON
cmake --build build-rel
```

工具链回退: 若无 Zephyr SDK, 任何 arm-none-eabi GCC (如 xpack
arm-none-eabi-gcc) 均可 —— stm32-cmake 按目标三元组找编译器, 把
`STM32_TOOLCHAIN_PATH` 指向 xpack 的安装根目录 (含 `bin/`、
`arm-none-eabi/`), `STM32_TARGET_TRIPLET=arm-none-eabi` 即可; 代码未用
Zephyr 专属工具, newlib/nosys 链接路径一致。(本项目实际构建一直用
Zephyr SDK, 回退路径未逐版本验证。)

## 主机单元测试

纯逻辑层 (字节序/CRC/配置/寄存器/Modbus 解析/RTU 状态机/UDP 命令/
时间门/littlefs 移植/ADC 换算/历史文件) 可脱离硬件在主机跑。

**Windows 原生 (推荐)** —— 需已安装 Visual Studio (MSVC), CMake 自动
定位 VS 生成器与工具链:

```bat
cmake -S tests -B build-host-win
cmake --build build-host-win --config Debug
ctest --test-dir build-host-win -C Debug --output-on-failure
```

**WSL / Linux 备选**:

```bash
wsl -e bash -c "cd /mnt/c/Users/jxwaz/code/io-edge-hub-freertos \
  && cmake -S tests -B build-host -DCMAKE_BUILD_TYPE=Debug \
  && cmake --build build-host -j8 \
  && (cd build-host && ctest --output-on-failure)"
```

11 个测试目标全部通过为基线 (MSVC 与 GCC 双编译器均通过);
加入 fw_upg 核心测试后为 **12 个**。

## MCUboot 双镜像与分区布局

与 Zephyr 版逐一对齐:

```
内部 flash 512KB @0x08000000:
  [0x000000, 0x010000)  boot   (MCUboot 引导, ~34KB, 上限 64KB)
  [0x010000, 0x080000)  slot0  (主应用 448KB; app 链接基址 0x08010200,
                         镜像头 0x200, VTOR 偏移 0x10200)
外部 W25Q128 16MB @SPI1:
  [0x000000, 0x070000)  slot1  (升级镜像暂存 448KB)
  [0x070000, 0x0E0000)  scratch (SWAP_SCRATCH 交换区 448KB)
  [0x0E0000, 0x0F0000)  storage (config A/B 双槽 64KB)
  [0x0F0000, ...)        littlefs (历史记录)
```

boot 域: bootutil v2.1.0 SWAP_SCRATCH + RSA-2048 (mbedTLS 子集,
MCUBOOT_VALIDATE_PRIMARY_SLOT 开), 中断 swap 可跨复位续传恢复。
app 域: `boot_set_pending` 写 slot1 trailer 请求换机。

## 签名流水线与密钥

```bat
python tools\gen_keys.py     & :: 首次生成 tools/keys/root-rsa2048.pem
python tools\sign_fw.py      & :: fw.elf -> fw.bin -> 签名 -> full.hex
```

- `sign_fw.py` 输出 `build/fw.bin` (原始 app) / `build/fw.signed.bin`
  (MCUboot 镜像) / `build/full.hex` (boot + 填充 + 签名 app, 全片烧录用)。
- 私钥 `tools/keys/*.pem` 已被 .gitignore, **永不入库**。
- 公钥指纹 (SHA256 of DER) 由 `tools/gen_keyhash.py` 生成
  `build/generated/fw_keyhash.h`, 所有升级通道在擦 flash 前比对。
- 镜像内 TLV 自带 KEYHASH, 签名验证在 MCUboot 换机/启动时完成。

## 固件升级 (四通道)

| 通道 | 工具 | 协议 | 实测 |
|---|---|---|---|
| UDP (V2 窗口) | `tools\firmware_upgrade.py --ip <ip> -f x.bin` | 0x01/0x06/0x03 + REBOOT, keyhash 预校验 | ~98 KB/s |
| UDP (legacy 停等) | 同上加 `--legacy` | 0x01/0x02/0x03 | ~45 KB/s |
| CAN (app 域) | `tools\firmware_upgrade_can.py upgrade -f x.bin` | 0x101-0x105 + PCAN | ~10 KB/s @250k |
| CAN (boot 紧急救援) | `tools\firmware_upgrade_can.py bootupgrade -f x.bin` | REBOOT→0x106 探测→0x107 应答→slot0 直写 | 同上, 免 swap |
| CAN (砖机救援) | `tools\firmware_upgrade_can.py rescue -f x.bin` | slot0 无有效镜像时 boot 永久探测 | 同上 |
| WebSocket (浏览器) | 设备 Web 页 "固件升级" 标签 | fw_start(keyhash b64)/二进制帧/fw_end | ~75 KB/s |

boot CAN 救援机制: 每次上电 500ms 探测窗口 (0x106 "BTO1"+版本,
200ms 周期), 上位机 0x107 应答即进入 slot0 直写会话, CONFIRM 后
本会话内验签启动新镜像 (无 swap 标记); 15s 无固件帧正常引导。
设备变砖 (slot0 损坏) 时探测永久重试, rescue 命令随时可救回。

端到端验证脚本: `tools/upgrade_e2e.py` (UDP+COM9 抓取),
`tools/can_e2e.py` (CAN), `tools/ws_e2e.py` (WS),
`tools/boot_rescue_e2e.py` (boot 救援/砖机)。

## 烧录

SWD (ST-Link / J-Link), 芯片内部 flash 基址 `0x08000000`。

首次烧录 / 全片恢复 (boot + 签名 app 一次写入):

```bat
python tools\sign_fw.py
python tools\flash_dual.py --me --full
```

只更新 boot: `ST-LINK_CLI -c SWD SWCLK=4000 -P build\boot.hex -V -Rst`
(注意: `-P` 后的 `-Rst` 实测不放行核心, 需再补一次独立 `-Rst`;
`flash_dual.py` 已内置)。外部 NOR 的 slot1/scratch/config/littlefs
分区不受 mass erase 影响, 升级残留状态由 MCUboot 自动恢复或忽略。

## 版本

`VERSION` 文件为唯一版本来源 (当前 0.3.0)。构建时 CMake 读取该文件并
注入 `git rev-parse --short=6 HEAD`, 生成 `fw_version.h`:

- 开机日志 banner: `version: v0.3.0_xxxxxx`
- Modbus 输入寄存器 `input_reg[0]` = `(MAJOR<<12 | MINOR<<8 | PATCH)`
  = `0x0300`

## 文档

- [docs/ACCEPTANCE.md](docs/ACCEPTANCE.md) —— 上机验收清单 (逐条勾选)
- [docs/KNOWN_DEVIATIONS.md](docs/KNOWN_DEVIATIONS.md) —— 与 Zephyr 版的已知偏差汇总
