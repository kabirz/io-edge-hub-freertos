cmake_minimum_required(VERSION 3.16)

# Release 构建: ON 时固件整体关闭日志 (LOG_* 编译为空, log_write 不再
# 被引用; log.c 仍编译 -- log_init/_write 保持链接可用)。默认 OFF。
option(APP_RELEASE "Disable logging for release" OFF)

# ---- 子模块自动初始化 (首次 cmake 配置时执行) ----
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/deps/FreeRTOS-Kernel/include/FreeRTOS.h)
    message(STATUS "初始化 git 子模块...")
    execute_process(COMMAND git submodule update --init
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} RESULT_VARIABLE _sm_rc)
    if(_sm_rc EQUAL 0)
        execute_process(COMMAND git -C deps/STM32CubeF4 submodule update --init
            Drivers/STM32F4xx_HAL_Driver Drivers/CMSIS/Device/ST/STM32F4xx
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        execute_process(COMMAND git -C deps/mcuboot submodule update --init ext/mbedtls
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
endif()

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

# ---- 默认路径 (无需 -D 指定) ----
set(CMAKE_TOOLCHAIN_FILE
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/stm32-cmake/cmake/stm32_gcc.cmake
    CACHE FILEPATH "stm32-cmake toolchain file")
set(STM32_CUBE_F4_PATH
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/STM32CubeF4
    CACHE PATH "STM32CubeF4 path")
set(FREERTOS_PATH
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/FreeRTOS-Kernel
    CACHE PATH "FreeRTOS kernel path")

project(io-edge-hub-freertos C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_INCLUDE_CURRENT_DIR TRUE)

find_package(CMSIS COMPONENTS STM32F4 REQUIRED)
find_package(HAL COMPONENTS STM32F4 REQUIRED)
find_package(FreeRTOS COMPONENTS ARM_CM4F REQUIRED)

# ---- 双镜像 flash 布局 (与 Zephyr io-edge-hub 板级分区一致,
# src/include/flash_layout.h): boot @0x08000000/64K + app(slot0)
# @0x08010000/448K。链接脚本用 stm32-cmake 官方模板按 target 生成;
# 可执行目标不链 CMSIS::STM32::F407VE (该 interface 自动注入整片
# 512K 默认 .ld), startup/system 源与 -T 由各 target 显式指定。
function(gen_ld_script TARGET NAME FLASH_ORIGIN FLASH_SIZE)
    set(LD_OUT "${CMAKE_BINARY_DIR}/${NAME}.ld")
    add_custom_command(
        OUTPUT "${LD_OUT}"
        COMMAND ${CMAKE_COMMAND}
            -DFLASH_ORIGIN="${FLASH_ORIGIN}" -DRAM_ORIGIN="0x20000000"
            -DCCRAM_ORIGIN="0x10000000" -DRAM_SHARE_ORIGIN="0x20030000"
            -DFLASH_SIZE="${FLASH_SIZE}" -DRAM_SIZE="128K" -DCCRAM_SIZE="64K"
            -DRAM_SHARE_SIZE="" -DSTACK_SIZE="0x400" -DHEAP_SIZE="0x200"
            -DLINKER_SCRIPT="${LD_OUT}" -DFORCE_READONLY_SECTIONS="TRUE"
            -P "${STM32_CMAKE_DIR}/stm32/linker_ld.cmake")
    add_custom_target(gen_${NAME}_ld DEPENDS "${LD_OUT}")
    add_dependencies(${TARGET} gen_${NAME}_ld)
    stm32_add_linker_script(${TARGET} PRIVATE "${LD_OUT}")
endfunction()

set(CMSIS_DEV_DIR ${STM32_CUBE_F4_PATH}/Drivers/CMSIS/Device/ST/STM32F4xx)
set(STM32_STARTUP_S ${CMSIS_DEV_DIR}/Source/Templates/gcc/startup_stm32f407xx.s)
set(STM32_SYSTEM_C ${CMSIS_DEV_DIR}/Source/Templates/system_stm32f4xx.c)

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
    src/web/httpd.c
    src/web/web_cmds.c
    src/web/ws.c
    deps/littlefs/lfs.c
    deps/littlefs/lfs_util.c
    deps/ioLibrary/Ethernet/wizchip_conf.c
    deps/ioLibrary/Ethernet/W5500/w5500.c
    deps/ioLibrary/Ethernet/socket.c
    ${STM32_STARTUP_S}
    ${STM32_SYSTEM_C}
)
# Web SPA: gzip 为 C 数组头 (确定性输出 mtime=0)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(WEB_INDEX_GZ_H ${CMAKE_CURRENT_BINARY_DIR}/generated/web_index_gz.h)
add_custom_command(
    OUTPUT ${WEB_INDEX_GZ_H}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_gz.py
            ${CMAKE_CURRENT_SOURCE_DIR}/src/web/index.html
            ${WEB_INDEX_GZ_H} WEB_INDEX_GZ
    DEPENDS src/web/index.html tools/gen_gz.py
    COMMENT "gzip index.html -> web_index_gz.h"
)
# 生成头列入目标源: 编译前自动执行 custom command
list(APPEND APP_SOURCES ${WEB_INDEX_GZ_H})

# 固件升级: fw 核心/UDP/CAN 通道 + bootutil_public (boot_set_pending 写
# slot1 trailer) + 共用 flash_map_backend (slot1 经 W25Q)
set(MCUBOOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/mcuboot)
set(BOOTUTIL_DIR ${MCUBOOT_DIR}/boot/bootutil)
set(BOOT_PORT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/boot/mcuboot)
list(APPEND APP_SOURCES
    src/fw/fw_upg.c
    src/fw/fw_udp.c
    src/fw/fw_can.c
    src/storage/intflash.c
    ${BOOT_PORT_DIR}/flash_map_backend.c
    ${BOOTUTIL_DIR}/src/bootutil_public.c
)
# keyhash 头 (SHA256(RSA pub DER), 与镜像 TLV KEYHASH 一致)
set(FW_KEYHASH_H ${CMAKE_CURRENT_BINARY_DIR}/generated/fw_keyhash.h)
add_custom_command(
    OUTPUT ${FW_KEYHASH_H}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_keyhash.py
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/keys/root-rsa2048.pem
            ${FW_KEYHASH_H}
    DEPENDS tools/gen_keyhash.py
    COMMENT "pem -> fw_keyhash.h"
)
list(APPEND APP_SOURCES ${FW_KEYHASH_H})

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
    ${LWIP_DIR}/src/api/tcpip.c
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
# 固件升级 (fw_upg/fw_udp + bootutil_public) 的移植层头路径
list(APPEND APP_INCLUDES ${BOOT_PORT_DIR} ${BOOTUTIL_DIR}/include)

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
# app 固件位于 slot0 (@0x08010000) 镜像头 (0x200) 之后:
# SystemInit 必须按偏移重设 VTOR (system_stm32f4xx.c 默认写
# FLASH_BASE=0x08000000, 会指回 boot)
target_compile_definitions(fw.elf PRIVATE
    USER_VECT_TAB_ADDRESS
    VECT_TAB_OFFSET=0x10200U
)
gen_ld_script(fw.elf fw 0x8010200 0x6FE00)
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
    ${LWIP_DIR}/src/api/tcpip.c
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
# 固件升级: 核心启用 FreeRTOS 锁; 上游文件关警告
set_source_files_properties(
    src/fw/fw_upg.c
    PROPERTIES COMPILE_DEFINITIONS "FW_UPG_FW")
set_source_files_properties(
    ${BOOT_PORT_DIR}/flash_map_backend.c
    ${BOOTUTIL_DIR}/src/bootutil_public.c
    PROPERTIES COMPILE_OPTIONS "-w")
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
    HAL::STM32::F4::FLASH   # intflash (flash_map_backend slot0 路径)
    HAL::STM32::F4::FLASHEx
    STM32::F407xx
    STM32::NoSys
)
stm32_print_size_of_target(fw.elf)
stm32_generate_hex_file(fw.elf)
stm32_generate_binary_file(fw.elf)

# ==================== 签名 + full 镜像生成 ====================
# imgtool 签名: fw.bin -> fw.signed.bin (MCUboot RSA-2048)
set(FW_KEY_PEM ${CMAKE_CURRENT_SOURCE_DIR}/tools/keys/root-rsa2048.pem)
set(FW_VERSION "${FW_VERSION_MAJOR}.${FW_VERSION_MINOR}.${FW_VERSION_PATCH}")
set(FW_SIGNED_BIN ${CMAKE_CURRENT_BINARY_DIR}/fw.signed.bin)
set(FULL_BIN ${CMAKE_CURRENT_BINARY_DIR}/full.bin)
set(FULL_HEX ${CMAKE_CURRENT_BINARY_DIR}/full.hex)

# ==================== boot 引导镜像 (MCUboot 域, 裸机无 RTOS) ====================
# imgtool 可执行路径 (优先 PATH, 其次 python -m imgtool)
find_program(IMGTOOL imgtool)
if(NOT IMGTOOL)
    set(IMGTOOL ${Python3_EXECUTABLE} -m imgtool)
endif()

add_custom_command(
    OUTPUT ${FW_SIGNED_BIN}
    COMMAND ${IMGTOOL} sign
            ${CMAKE_CURRENT_BINARY_DIR}/fw.bin
            ${FW_SIGNED_BIN}
            --key ${FW_KEY_PEM}
            --header-size 512 --pad-header
            --align 8 --version ${FW_VERSION}
            --slot-size 0x70000 --max-sectors 120
            --erased-val 0xff
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/fw.bin ${FW_KEY_PEM}
    COMMENT "imgtool sign -> fw.signed.bin"
)

# full.bin = boot.bin + 0xFF 填充到 0x10000 + fw.signed.bin
add_custom_command(
    OUTPUT ${FULL_BIN}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/merge_image.py
            ${CMAKE_CURRENT_BINARY_DIR}/boot.bin
            ${FW_SIGNED_BIN} 65536 ${FULL_BIN}
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/boot.bin ${FW_SIGNED_BIN}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/merge_image.py
    COMMENT "merge boot + padding + signed app -> full.bin"
)

add_custom_command(
    OUTPUT ${FULL_HEX}
    COMMAND ${CMAKE_OBJCOPY} -I binary -O ihex
            --change-addresses=0x8000000
            ${FULL_BIN} ${FULL_HEX}
    DEPENDS ${FULL_BIN}
    COMMENT "objcopy -> full.hex"
)

add_custom_target(fw_sign ALL DEPENDS ${FW_SIGNED_BIN} ${FULL_HEX})

# ==================== 烧录目标 ====================
find_program(STFLASH st-flash)
find_program(STLINK_CLI ST-LINK_CLI)
find_program(PYOCD pyocd)

if(STFLASH)
    add_custom_target(flash
        COMMAND ${STFLASH} --reset write ${FULL_BIN} 0x8000000
        DEPENDS fw_sign
        USES_TERMINAL
        COMMENT "st-flash write full.bin -> 0x08000000"
    )
endif()

if(STLINK_CLI)
    add_custom_target(stlink_flash
        COMMAND ${STLINK_CLI} -c SWD SWCLK=4000 -P ${FULL_HEX} -V -Rst
        COMMAND ${STLINK_CLI} -c SWD SWCLK=4000 -Rst
        DEPENDS fw_sign
        USES_TERMINAL
        COMMENT "ST-LINK_CLI flash full.hex"
    )
endif()

if(PYOCD)
    add_custom_target(pyocd_flash
        COMMAND ${PYOCD} flash -t stm32f407vetx ${FULL_HEX}
        DEPENDS fw_sign
        USES_TERMINAL
        COMMENT "pyocd flash full.hex"
    )
    add_custom_target(pyocd_reset
        COMMAND ${PYOCD} reset -t stm32f407vetx
        COMMENT "pyocd reset"
    )
endif()

set(BOOT_KEYS_C ${CMAKE_CURRENT_BINARY_DIR}/generated/boot_keys.c)
add_custom_command(
    OUTPUT ${BOOT_KEYS_C}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_boot_keys.py
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/keys/root-rsa2048.pem
            ${BOOT_KEYS_C}
    DEPENDS tools/gen_boot_keys.py tools/keys/root-rsa2048.pem
    COMMENT "pem -> boot_keys.c"
)

# MCUboot bootutil (deps/mcuboot @v2.1.0) + mbedTLS 3.0 (RSA 验签子集)
set(MBEDTLS_DIR ${MCUBOOT_DIR}/ext/mbedtls)

set(BOOT_SOURCES
    src/boot/boot.c
    src/boot/boot_uart.c
    src/boot/boot_can.c
    src/board/board.c
    src/board/spi.c
    src/board/stm32f4xx_hal_timebase_tim.c
    src/storage/w25qxx.c
    src/storage/intflash.c
    ${BOOT_PORT_DIR}/flash_map_backend.c
    ${BOOTUTIL_DIR}/src/bootutil_misc.c
    ${BOOTUTIL_DIR}/src/bootutil_public.c
    ${BOOTUTIL_DIR}/src/caps.c
    ${BOOTUTIL_DIR}/src/fault_injection_hardening.c
    ${BOOTUTIL_DIR}/src/image_rsa.c
    ${BOOTUTIL_DIR}/src/image_validate.c
    ${BOOTUTIL_DIR}/src/loader.c
    ${BOOTUTIL_DIR}/src/swap_misc.c
    ${BOOTUTIL_DIR}/src/swap_scratch.c
    ${BOOTUTIL_DIR}/src/tlv.c
    ${MBEDTLS_DIR}/library/platform_util.c
    ${MBEDTLS_DIR}/library/platform.c
    ${MBEDTLS_DIR}/library/bignum.c
    ${MBEDTLS_DIR}/library/asn1parse.c
    ${MBEDTLS_DIR}/library/oid.c
    ${MBEDTLS_DIR}/library/rsa.c
    ${MBEDTLS_DIR}/library/md.c
    ${MBEDTLS_DIR}/library/sha256.c
    ${MBEDTLS_DIR}/library/memory_buffer_alloc.c
    ${STM32_STARTUP_S}
    ${STM32_SYSTEM_C}
    ${BOOT_KEYS_C}
    ${FW_KEYHASH_H}
    stm32f4xx_hal_conf.h
)
add_executable(boot.elf ${BOOT_SOURCES})
set_target_properties(boot.elf PROPERTIES OUTPUT_NAME boot)

target_include_directories(boot.elf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/board
    ${CMAKE_CURRENT_SOURCE_DIR}/src/boot
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${CMAKE_BINARY_DIR}/generated
    ${BOOT_PORT_DIR}
    ${BOOT_PORT_DIR}/mbedtls_conf
    ${BOOTUTIL_DIR}/include
    ${MBEDTLS_DIR}/include
)
target_compile_options(boot.elf PRIVATE -Wall -Wextra -Os)
target_compile_definitions(boot.elf PRIVATE BOOT_DOMAIN)
# 上游代码不按本项目警告级别维护
set_source_files_properties(
    ${BOOTUTIL_DIR}/src/bootutil_misc.c
    ${BOOTUTIL_DIR}/src/bootutil_public.c
    ${BOOTUTIL_DIR}/src/caps.c
    ${BOOTUTIL_DIR}/src/fault_injection_hardening.c
    ${BOOTUTIL_DIR}/src/image_rsa.c
    ${BOOTUTIL_DIR}/src/image_validate.c
    ${BOOTUTIL_DIR}/src/loader.c
    ${BOOTUTIL_DIR}/src/swap_misc.c
    ${BOOTUTIL_DIR}/src/swap_scratch.c
    ${BOOTUTIL_DIR}/src/tlv.c
    ${MBEDTLS_DIR}/library/platform_util.c
    ${MBEDTLS_DIR}/library/platform.c
    ${MBEDTLS_DIR}/library/bignum.c
    ${MBEDTLS_DIR}/library/asn1parse.c
    ${MBEDTLS_DIR}/library/oid.c
    ${MBEDTLS_DIR}/library/rsa.c
    ${MBEDTLS_DIR}/library/md.c
    ${MBEDTLS_DIR}/library/sha256.c
    ${MBEDTLS_DIR}/library/memory_buffer_alloc.c
    PROPERTIES COMPILE_OPTIONS "-w"
)
target_link_libraries(boot.elf PRIVATE
    HAL::STM32::F4::RCC
    HAL::STM32::F4::GPIO
    HAL::STM32::F4::CORTEX
    HAL::STM32::F4::UART
    HAL::STM32::F4::SPI
    HAL::STM32::F4::TIM
    HAL::STM32::F4::TIMEx # HAL_TIM_IRQHandler 引用 TIMEx 弱回调
    HAL::STM32::F4::FLASH
    HAL::STM32::F4::FLASHEx # HAL_FLASHEx_Erase (intflash 扇区擦除)
    STM32::F407xx
    STM32::NoSys
)
gen_ld_script(boot.elf boot 0x8000000 64K)
stm32_print_size_of_target(boot.elf)
stm32_generate_hex_file(boot.elf)
stm32_generate_binary_file(boot.elf)
