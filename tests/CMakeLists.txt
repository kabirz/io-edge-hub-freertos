cmake_minimum_required(VERSION 3.16)
project(io_host_tests C)
set(CMAKE_C_STANDARD 11)
get_filename_component(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
set(SRC ${REPO_ROOT}/src)
enable_testing()

function(io_test name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE ${SRC}/include ${SRC} ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(${name} PRIVATE -Wall -Wextra -O1)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

io_test(test_bytes_crc test_bytes_crc.c ${SRC}/util/io_bytes.c ${SRC}/util/io_crc.c)
io_test(test_config_store test_config_store.c fake_flash.c
  ${SRC}/storage/config_store.c ${SRC}/util/io_crc.c ${SRC}/util/io_bytes.c)

# regmap 版本注入 (固件构建走 generated fw_version.h; regmap.c 仅在
# FW_VERSION_MAJOR 未定义时才 include 它, 故此处 -D 注入即可)
io_test(test_regmap test_regmap.c fake_flash.c
  ${SRC}/modbus/regmap.c ${SRC}/storage/config_store.c
  ${SRC}/util/io_crc.c ${SRC}/util/io_bytes.c)
target_compile_definitions(test_regmap PRIVATE
  FW_VERSION_MAJOR=0 FW_VERSION_MINOR=3 FW_VERSION_PATCH=0
  FW_GIT_VERSION="\"test\"")

# mb_server: PDU 解码器直连 regmap (版本注入供 regmap.c 的 input_reg[0])
io_test(test_mb_server test_mb_server.c fake_flash.c
  ${SRC}/modbus/mb_server.c ${SRC}/modbus/regmap.c ${SRC}/storage/config_store.c
  ${SRC}/util/io_crc.c ${SRC}/util/io_bytes.c)
target_compile_definitions(test_mb_server PRIVATE
  FW_VERSION_MAJOR=0 FW_VERSION_MINOR=3 FW_VERSION_PATCH=0
  FW_GIT_VERSION="\"test\"")

# mbtcp_adu: TCP ADU 层 (MBAP 解析/广播抑制/响应合并) 全帧进出,
# 内部直连 mb_server+regmap (诊断计数经 FC08 交叉验证)
io_test(test_mbtcp_adu test_mbtcp_adu.c fake_flash.c
  ${SRC}/modbus/mbtcp_adu.c ${SRC}/modbus/mb_server.c ${SRC}/modbus/regmap.c
  ${SRC}/storage/config_store.c ${SRC}/util/io_crc.c ${SRC}/util/io_bytes.c)
target_compile_definitions(test_mbtcp_adu PRIVATE
  FW_VERSION_MAJOR=0 FW_VERSION_MINOR=3 FW_VERSION_PATCH=0
  FW_GIT_VERSION="\"test\"")

# rtu_frame: RTU 帧状态机 (拼帧/CRC 校验/单播过滤/广播抑制/溢出保护),
# 内部直连 mb_server+regmap; 帧构造用 crc16_modbus, 诊断计数组合经
# FC08 子功能交叉验证 (bus_msg 含被丢弃帧, 对齐 Zephyr handler 入口)
io_test(test_rtu_frame test_rtu_frame.c fake_flash.c
  ${SRC}/modbus/rtu_frame.c ${SRC}/modbus/mb_server.c ${SRC}/modbus/regmap.c
  ${SRC}/storage/config_store.c ${SRC}/util/io_crc.c ${SRC}/util/io_bytes.c)
target_compile_definitions(test_rtu_frame PRIVATE
  FW_VERSION_MAJOR=0 FW_VERSION_MINOR=3 FW_VERSION_PATCH=0
  FW_GIT_VERSION="\"test\"")

# udp_cfg: UDP 配置协议命令层 (0x10-0x19 逐字节应答/总是应答规则/
# 未知命令静默/FACTORY_RESET 两步确认 + 开机 5s 单命令怪癖),
# 内部直连 regmap+config_store; 时间钩子 udp_now_ms 绑可控计数器
io_test(test_udp_cfg test_udp_cfg.c fake_flash.c
  ${SRC}/net/udp_cfg.c ${SRC}/modbus/regmap.c ${SRC}/storage/config_store.c
  ${SRC}/util/io_crc.c ${SRC}/util/io_bytes.c)
target_compile_definitions(test_udp_cfg PRIVATE
  FW_VERSION_MAJOR=0 FW_VERSION_MINOR=3 FW_VERSION_PATCH=0
  FW_GIT_VERSION="\"test\"")

# lfs_port: littlefs 移植层, RAM 假件上挂载/读写/脏分区恢复。
# littlefs 本体 (deps/littlefs) 可直接 host 编译; LFS_PORT_TEST_SMALL 把
# 分区缩到 256 KiB (假件仅 1 MiB), LFS_OFFSET 仍为真实值。
io_test(test_lfs_port test_lfs_port.c fake_flash.c
  ${SRC}/storage/lfs_port.c
  ${REPO_ROOT}/deps/littlefs/lfs.c ${REPO_ROOT}/deps/littlefs/lfs_util.c)
target_include_directories(test_lfs_port PRIVATE ${REPO_ROOT}/deps/littlefs)
target_compile_definitions(test_lfs_port PRIVATE LFS_PORT_TEST_SMALL)

# history: 纯文件核心 (history_file.c) 直测 — DI/AI 字节布局、4096B 轮转、
# 保留 10 份清理、close 续写、重启续写字典序最大文件。history.c (FreeRTOS
# 队列/任务壳) 不参与 host 链接。HIST_FILE_MAX 缩到 4096 让轮转可触发。
io_test(test_history test_history.c fake_flash.c
  ${SRC}/history/history_file.c ${SRC}/storage/lfs_port.c
  ${REPO_ROOT}/deps/littlefs/lfs.c ${REPO_ROOT}/deps/littlefs/lfs_util.c)
target_include_directories(test_history PRIVATE ${REPO_ROOT}/deps/littlefs)
target_compile_definitions(test_history PRIVATE LFS_PORT_TEST_SMALL HIST_FILE_MAX=4096)
