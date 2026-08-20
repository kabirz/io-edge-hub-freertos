# io-edge-hub-freertos

io-edge-hub 的 FreeRTOS 移植一期 (裸 STM32, 无 Zephyr)。目标: 在
STM32F407VET6 上以 FreeRTOS + STM32 HAL + W5500 原样复刻 Zephyr 版
(apps 仓库 `applications/io-edge-hub`) 的对外行为 —— Modbus TCP/RTU
从站、UDP 配置协议、DI/DO/AI 采样、CAN、历史记录 (littlefs)、RTC、
心跳/看门狗。协议帧格式与寄存器布局不变, Zephyr 版上位机/pytest 用例
可直接复用。与 Zephyr 版的实现差异汇总见
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
CMakeLists.txt          固件构建 (stm32-cmake + FreeRTOS + HAL + APP_RELEASE 选项)
FreeRTOSConfig.h        FreeRTOS 内核配置 (168MHz / tick 1ms / 优先级 5 级)
stm32f4xx_hal_conf.h    HAL 模块裁剪
VERSION                 版本号 (当前 0.3.0)
src/
  main.c                初始化序列 + 心跳/喂狗任务 (Zephyr main+SYS_INIT 合并)
  board/                时钟/GPIO/SPI2 底板 + HAL tick (TIM7)
  sys/                  日志/时间(RTC)/看门狗(IWDG)/重启/syscalls/os 锁
  util/                 字节序 + CRC16
  storage/              W25Qxx 驱动 / 配置存储 / littlefs 移植层
  history/              历史记录 (纯文件核心 + FreeRTOS 任务壳)
  modbus/               寄存器模型 / PDU 服务器 / TCP / RTU 传输
  net/                  W5500 网络层 / UDP 配置协议 / CAN
  io/                   DI/DO/ADC 采样
  include/              对外头文件
tests/                  主机 (Linux/WSL) 单元测试, 11 个目标, ctest
deps/                   子模块: stm32-cmake / FreeRTOS-Kernel / littlefs /
                        ioLibrary(W5500) / STM32CubeF4
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

产物: `build/fw.elf` / `build/fw.hex`。

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

## 主机单元测试 (WSL / Linux)

纯逻辑层 (字节序/CRC/配置/寄存器/Modbus 解析/RTU 状态机/UDP 命令/
时间门/littlefs 移植/ADC 换算/历史文件) 可脱离硬件在主机跑:

```bash
wsl -e bash -c "cd /mnt/c/Users/jxwaz/code/io-edge-hub-freertos \
  && cmake -S tests -B build-host -DCMAKE_BUILD_TYPE=Debug \
  && cmake --build build-host -j8 \
  && (cd build-host && ctest --output-on-failure)"
```

11 个测试目标全部通过为基线。

## 烧录

SWD (ST-Link / J-Link), 芯片内部 flash 基址 `0x08000000`, 烧
`build/fw.hex` (或 `build-rel/fw.hex`):

```bash
# STM32CubeProgrammer 示例
STM32_Programmer_CLI -c port=SWD -d build/fw.hex -v -rst
# 或 openocd
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
  -c "program build/fw.elf verify reset exit"
```

## 版本

`VERSION` 文件为唯一版本来源 (当前 0.3.0)。构建时 CMake 读取该文件并
注入 `git rev-parse --short=6 HEAD`, 生成 `fw_version.h`:

- 开机日志 banner: `version: v0.3.0_xxxxxx`
- Modbus 输入寄存器 `input_reg[0]` = `(MAJOR<<12 | MINOR<<8 | PATCH)`
  = `0x0300`

## 文档

- [docs/ACCEPTANCE.md](docs/ACCEPTANCE.md) —— 上机验收清单 (逐条勾选)
- [docs/KNOWN_DEVIATIONS.md](docs/KNOWN_DEVIATIONS.md) —— 与 Zephyr 版的已知偏差汇总
