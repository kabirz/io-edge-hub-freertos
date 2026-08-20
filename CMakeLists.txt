cmake_minimum_required(VERSION 3.16)

# Release 构建: ON 时固件整体关闭日志 (LOG_* 编译为空, log_write 不再
# 被引用; log.c 仍编译 -- log_init/_write 保持链接可用)。默认 OFF。
option(APP_RELEASE "Disable logging for release" OFF)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ver_file)
string(REGEX MATCH "VERSION_MAJOR = ([0-9]+)" _ ${ver_file})
set(FW_VERSION_MAJOR ${CMAKE_MATCH_1})
string(REGEX MATCH "VERSION_MINOR = ([0-9]+)" _ ${ver_file})
set(FW_VERSION_MINOR ${CMAKE_MATCH_1})
string(REGEX MATCH "PATCHLEVEL = ([0-9]+)" _ ${ver_file})
set(FW_VERSION_PATCH ${CMAKE_MATCH_1})
execute_process(COMMAND git rev-parse --short=6 HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE FW_GIT_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT FW_GIT_VERSION)
  set(FW_GIT_VERSION "000000")
endif()
configure_file(src/fw_version.h.in ${CMAKE_BINARY_DIR}/generated/fw_version.h @ONLY)

project(io-edge-hub-freertos C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_INCLUDE_CURRENT_DIR TRUE)

find_package(CMSIS COMPONENTS STM32F4 REQUIRED)
find_package(HAL COMPONENTS STM32F4 REQUIRED)
find_package(FreeRTOS COMPONENTS ARM_CM4F REQUIRED)

set(APP_SOURCES
    src/main.c
    src/board/board.c
    src/board/spi.c
    src/board/stm32f4xx_hal_timebase_tim.c
    src/sys/syscalls.c
    src/sys/time.c
    src/sys/watchdog.c
    src/sys/reboot.c
    src/sys/log.c
    src/sys/os.c
    src/util/io_bytes.c
    src/util/io_crc.c
    src/storage/config_store.c
    src/storage/w25qxx.c
    src/storage/lfs_port.c
    src/history/history.c
    src/history/history_file.c
    src/modbus/regmap.c
    src/modbus/mb_server.c
    src/modbus/mbtcp_adu.c
    src/modbus/tcp.c
    src/modbus/rtu_frame.c
    src/modbus/rtu.c
    src/net/w5500.c
    src/net/w5500_macraw.c
    src/net/can.c
    src/net/udp_cfg.c
    src/net/udp_task.c
    src/net/sys_arch.c
    src/io/dio.c
    src/io/adc.c
    deps/littlefs/lfs.c
    deps/littlefs/lfs_util.c
    deps/ioLibrary/Ethernet/wizchip_conf.c
    deps/ioLibrary/Ethernet/W5500/w5500.c
    deps/ioLibrary/Ethernet/socket.c
)
# LwIP 2.1.3 core sources (FreeRTOS + NO_SYS=0 mode)
set(LWIP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/lwip)
list(APPEND APP_SOURCES
    ${LWIP_DIR}/src/core/init.c
    ${LWIP_DIR}/src/core/def.c
    ${LWIP_DIR}/src/core/dns.c
    ${LWIP_DIR}/src/core/inet_chksum.c
    ${LWIP_DIR}/src/core/ip.c
    ${LWIP_DIR}/src/core/mem.c
    ${LWIP_DIR}/src/core/memp.c
    ${LWIP_DIR}/src/core/netif.c
    ${LWIP_DIR}/src/core/pbuf.c
    ${LWIP_DIR}/src/core/raw.c
    ${LWIP_DIR}/src/core/stats.c
    ${LWIP_DIR}/src/core/sys.c
    ${LWIP_DIR}/src/core/tcp.c
    ${LWIP_DIR}/src/core/tcp_in.c
    ${LWIP_DIR}/src/core/tcp_out.c
    ${LWIP_DIR}/src/core/timeouts.c
    ${LWIP_DIR}/src/core/udp.c
    ${LWIP_DIR}/src/core/ipv4/ip4.c
    ${LWIP_DIR}/src/core/ipv4/ip4_addr.c
    ${LWIP_DIR}/src/core/ipv4/ip4_frag.c
    ${LWIP_DIR}/src/core/ipv4/icmp.c
    ${LWIP_DIR}/src/core/ipv4/etharp.c
    ${LWIP_DIR}/src/core/ipv4/autoip.c
    ${LWIP_DIR}/src/netif/ethernet.c
)
set(APP_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/board
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/littlefs
    ${CMAKE_BINARY_DIR}/generated
    # LwIP include paths (user arch headers before LwIP's own arch/)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${LWIP_DIR}/src/include
    # 仅 Ethernet 根上路径: "wizchip_conf.h"/"socket.h" 直名包含;
    # ioLibrary 的 w5500.h 只经 "W5500/w5500.h" 路径限定访问,
    # 未限定 "w5500.h" 一律解析到 src/include (本模块对外头)。
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/ioLibrary/Ethernet
)

add_executable(fw.elf ${APP_SOURCES} stm32f4xx_hal_conf.h)
# CMAKE_EXECUTABLE_SUFFIX is ".elf" (stm32_gcc.cmake); OUTPUT_NAME keeps target
# name "fw.elf" for target_* commands while producing artifacts fw.elf / fw.hex.
set_target_properties(fw.elf PROPERTIES OUTPUT_NAME fw)

target_include_directories(fw.elf PRIVATE ${APP_INCLUDES})
target_compile_options(fw.elf PRIVATE -Wall -Wextra)
# ioLibrary 选型: W5500, SPI 模式 (wizchip_conf.h 默认 _WIZCHIP_=W6300)
target_compile_definitions(fw.elf PRIVATE _WIZCHIP_=5500)
# LwIP: enable lwipopts.h via -I (src/include is already in path),
# define LWIP_LITE=1 for reduced feature set, ETHARP_SUPPORT_STATIC_ENTRIES=0
target_compile_definitions(fw.elf PRIVATE
    LWIP_LITE=1
    ETHARP_SUPPORT_STATIC_ENTRIES=0
)
# Release: 仅固件目标关日志 (host 测试在 tests/CMakeLists.txt 自带
# LOG_ENABLE=0, 不受此开关影响)
if(APP_RELEASE)
    target_compile_definitions(fw.elf PRIVATE LOG_ENABLE=0)
endif()
# ioLibrary 上游不按本项目 -Wall -Wextra 级别维护, 仅这些文件关警告
# (GCC 用 -w 全关, 排在 -Wall -Wextra 之后生效)
set_source_files_properties(
    deps/ioLibrary/Ethernet/wizchip_conf.c
    deps/ioLibrary/Ethernet/W5500/w5500.c
    deps/ioLibrary/Ethernet/socket.c
    src/net/w5500_macraw.c
    PROPERTIES COMPILE_OPTIONS "-w"
)
# LwIP upstream: suppress warnings for vendored sources
set_source_files_properties(
    ${LWIP_DIR}/src/core/init.c
    ${LWIP_DIR}/src/core/def.c
    ${LWIP_DIR}/src/core/dns.c
    ${LWIP_DIR}/src/core/inet_chksum.c
    ${LWIP_DIR}/src/core/ip.c
    ${LWIP_DIR}/src/core/mem.c
    ${LWIP_DIR}/src/core/memp.c
    ${LWIP_DIR}/src/core/netif.c
    ${LWIP_DIR}/src/core/pbuf.c
    ${LWIP_DIR}/src/core/raw.c
    ${LWIP_DIR}/src/core/stats.c
    ${LWIP_DIR}/src/core/sys.c
    ${LWIP_DIR}/src/core/tcp.c
    ${LWIP_DIR}/src/core/tcp_in.c
    ${LWIP_DIR}/src/core/tcp_out.c
    ${LWIP_DIR}/src/core/timeouts.c
    ${LWIP_DIR}/src/core/udp.c
    ${LWIP_DIR}/src/core/ipv4/ip4.c
    ${LWIP_DIR}/src/core/ipv4/ip4_addr.c
    ${LWIP_DIR}/src/core/ipv4/ip4_frag.c
    ${LWIP_DIR}/src/core/ipv4/icmp.c
    ${LWIP_DIR}/src/core/ipv4/etharp.c
    ${LWIP_DIR}/src/core/ipv4/autoip.c
    ${LWIP_DIR}/src/netif/ethernet.c
    PROPERTIES COMPILE_OPTIONS "-w"
)
# tcp.c / udp_task.c 包含 ioLibrary socket.h: 上游头声明了 W6x00 专用
# static 函数却不定义 -> -Wunused-function (GCC 在 TU 末尾发出, pragma
# 包不住)。仅对这些文件关该警告, 其余 -Wall -Wextra 照常。
set_source_files_properties(
    src/modbus/tcp.c
    src/net/udp_task.c
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-function"
)
# littlefs 仅在 Release (-DNDEBUG) 警告: lfs_mlist_isopen 等静态助手仅被
# LFS_ASSERT(=assert) 引用, NDEBUG 下 assert 展开为空 -> "defined but not
# used"。上游源码, 同前述策略仅对该文件关此警告。
#
# littlefs 每文件 1KB 缓存路由到 heap_4: 固件 newlib 桩 _sbrk 恒返回
# ENOMEM, littlefs 默认 malloc 路径上机必 LFS_ERR_NOMEM (历史记录静默
# 失效)。LFS_MALLOC/LFS_FREE 仅对 littlefs 两个源文件定义 (lfs_util.h
# 内联 lfs_malloc/lfs_free 消费; 定义若扩散到全目标, 所有含 lfs.h 的
# 应用编译单元都会因缺原型报 implicit-declaration), 原型经 -include
# 的 lfs_heap.h 前置; 适配实现在 lfs_port.c (LFS_HEAP_FW 门控)。host
# 测试是独立项目, 不带该定义, 照常用系统 malloc。
set_source_files_properties(
    deps/littlefs/lfs.c
    PROPERTIES COMPILE_DEFINITIONS "LFS_MALLOC=lfs_heap_alloc;LFS_FREE=lfs_heap_free"
               COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/src/include/lfs_heap.h;-Wno-unused-function")
set_source_files_properties(
    deps/littlefs/lfs_util.c
    PROPERTIES COMPILE_DEFINITIONS "LFS_MALLOC=lfs_heap_alloc;LFS_FREE=lfs_heap_free"
               COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/src/include/lfs_heap.h")
set_source_files_properties(
    src/storage/lfs_port.c
    PROPERTIES COMPILE_DEFINITIONS "LFS_HEAP_FW")
target_link_libraries(fw.elf PRIVATE
    FreeRTOS::ARM_CM4F
    FreeRTOS::Heap::4
    FreeRTOS::Timers
    HAL::STM32::F4::RCC
    HAL::STM32::F4::GPIO
    HAL::STM32::F4::CORTEX
    HAL::STM32::F4::UART
    HAL::STM32::F4::DMA
    HAL::STM32::F4::SPI
    HAL::STM32::F4::ADC
    HAL::STM32::F4::CAN
    HAL::STM32::F4::IWDG
    HAL::STM32::F4::RTC
    HAL::STM32::F4::RTCEx # HAL_RTCEx_BKUPRead/Write (time.c 备份域标志)
    HAL::STM32::F4::TIM
    HAL::STM32::F4::TIMEx
    HAL::STM32::F4::PWR
    CMSIS::STM32::F407VE
    STM32::NoSys
)
stm32_print_size_of_target(fw.elf)
stm32_generate_hex_file(fw.elf)
