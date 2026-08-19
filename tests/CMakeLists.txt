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
