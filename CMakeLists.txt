cmake_minimum_required(VERSION 3.16)

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
    src/board/stm32f4xx_hal_timebase_tim.c
    src/sys/syscalls.c
    src/util/io_bytes.c
    src/util/io_crc.c
    src/storage/config_store.c
    src/modbus/regmap.c
    src/modbus/mb_server.c
    src/sys/app_stubs.c
)
set(APP_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/board
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${CMAKE_BINARY_DIR}/generated
)

add_executable(fw.elf ${APP_SOURCES} stm32f4xx_hal_conf.h)
# CMAKE_EXECUTABLE_SUFFIX is ".elf" (stm32_gcc.cmake); OUTPUT_NAME keeps target
# name "fw.elf" for target_* commands while producing artifacts fw.elf / fw.hex.
set_target_properties(fw.elf PROPERTIES OUTPUT_NAME fw)

target_include_directories(fw.elf PRIVATE ${APP_INCLUDES})
target_compile_options(fw.elf PRIVATE -Wall -Wextra)
target_link_libraries(fw.elf PRIVATE
    FreeRTOS::ARM_CM4F
    FreeRTOS::Heap::4
    FreeRTOS::Timers
    HAL::STM32::F4::RCC
    HAL::STM32::F4::GPIO
    HAL::STM32::F4::CORTEX
    HAL::STM32::F4::UART
    HAL::STM32::F4::SPI
    HAL::STM32::F4::ADC
    HAL::STM32::F4::CAN
    HAL::STM32::F4::IWDG
    HAL::STM32::F4::RTC
    HAL::STM32::F4::TIM
    HAL::STM32::F4::TIMEx
    HAL::STM32::F4::PWR
    CMSIS::STM32::F407VE
    STM32::NoSys
)
stm32_print_size_of_target(fw.elf)
stm32_generate_hex_file(fw.elf)
