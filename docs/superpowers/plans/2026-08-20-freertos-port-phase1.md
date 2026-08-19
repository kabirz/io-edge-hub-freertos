# io-edge-hub FreeRTOS 移植（第一期）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在独立仓库用 FreeRTOS + STM32 HAL（stm32-cmake 构建）重实现 io-edge-hub 第一期功能集，对外协议与寄存器映射与 Zephyr 版二进制兼容。

**Architecture:** 纯逻辑核心（寄存器模型、Modbus 解码、RTU 成帧、UDP 命令、历史文件、配置存储）与硬件/OS 层（HAL、W5500、FreeRTOS 任务）分离：核心模块无 OS/硬件依赖，在 WSL 主机端用 gcc 做单元测试；硬件层仅做编译验证 + 上机验收。Zephyr 版源码（`C:\Users\jxwaz\code\app\apps\applications\io-edge-hub`）是行为权威。

**Tech Stack:** C11 / FreeRTOS v11 (ARM_CM4F) / STM32CubeF4 HAL / littlefs v2.11 / WIZnet ioLibrary (W5500) / ObKo stm32-cmake / arm-zephyr-eabi GCC 12.2 (Zephyr SDK 0.17.0)

**设计文档:** `docs/superpowers/specs/2026-08-19-freertos-port-phase1-design.md`（含已批准决策与已知偏差记录）

## Global Constraints

- 仓库：`C:\Users\jxwaz\code\io-edge-hub-freertos`（已 git init，main 分支，含设计文档）
- 目标芯片：STM32F407VET6（512K flash / 128K SRAM + 64K CCMRAM）
- 固件构建（Windows，仓库根目录）：
  ```
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug ^
    -DCMAKE_TOOLCHAIN_FILE=deps/stm32-cmake/cmake/stm32_gcc.cmake ^
    -DSTM32_TOOLCHAIN_PATH=C:/Users/jxwaz/zephyr-sdk-0.17.0/arm-zephyr-eabi ^
    -DSTM32_TARGET_TRIPLET=arm-zephyr-eabi ^
    -DSTM32_CUBE_F4_PATH=deps/STM32CubeF4 ^
    -DFREERTOS_PATH=deps/FreeRTOS-Kernel
  cmake --build build
  ```
- 主机单元测试（WSL Ubuntu，gcc 13.3，仓库根目录）：
  ```
  wsl -e bash -c "cd /mnt/c/Users/jxwaz/code/io-edge-hub-freertos && cmake -S tests -B build-host -DCMAKE_BUILD_TYPE=Debug && cmake --build build-host -j8 && (cd build-host && ctest --output-on-failure)"
  ```
- 代码规范：C11，`-Wall -Wextra`；应用代码**禁止** malloc/newlib 动态分配、禁止 `%f` 打印；所有 FreeRTOS 线程/队列静态创建；沿用 apps 仓库的 `.clang-format`（任务 1 拷入）
- 返回值约定：0 成功，-1 失败（对齐不改变；mb_server 对任何非 0 写/读失败统一映射 Modbus 异常 0x02）
- 中断规则：使用 FreeRTOS FromISR API 的 ISR（USART2、CAN1）抢占优先级数值必须 ≥ `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`(5)；本计划统一配 6
- 线程优先级表（FreeRTOS，数值大=优先级高）：DI=6, ADC=6, RTU=5, UDP=4, TimerSvc=4, history=2, mb_tcp=3, heartbeat=1
- 行为权威：Zephyr 版 `applications/io-edge-hub/` 源码；本计划引用的寄存器语义/协议格式均提取自该源码，冲突时以它为准
- 版本注入：`VERSION` 文件三段式 + `git rev-parse --short=6 HEAD`（对齐 Zephyr 版 `fw_gitver.h`）

---

### Task 1: 仓库脚手架 + submodule + 构建链打通

**Files:**
- Create: `.gitignore`, `.clang-format`（从 `C:\Users\jxwaz\code\app\apps\.clang-format` 拷贝）
- Create: `CMakeLists.txt`, `VERSION`, `src/main.c`, `FreeRTOSConfig.h`, `stm32f4xx_hal_conf.h`, `src/sys/syscalls.c`, `src/fw_version.h.in`
- Create (submodules): `deps/stm32-cmake`, `deps/STM32CubeF4`, `deps/FreeRTOS-Kernel`, `deps/littlefs`, `deps/ioLibrary`

**Interfaces:**
- Produces: 可构建的固件骨架（FreeRTOS 调度器空转 + 版本宏）；后续任务直接在此 CMake 上加源文件与链接库

- [ ] **Step 1: 添加 submodule（浅克隆，仓库根目录）**

```bash
git submodule add --depth 1 https://github.com/ObKo/stm32-cmake deps/stm32-cmake
git submodule add --depth 1 https://github.com/STMicroelectronics/STM32CubeF4 deps/STM32CubeF4
git submodule add --depth 1 -b v11.2.0 https://github.com/FreeRTOS/FreeRTOS-Kernel deps/FreeRTOS-Kernel
git submodule add --depth 1 -b v2.11.1 https://github.com/littlefs-project/littlefs deps/littlefs
git submodule add --depth 1 https://github.com/Wiznet/ioLibrary_Driver deps/ioLibrary
```

- [ ] **Step 2: 写 `.gitignore`**

```gitignore
build/
build-host/
*.elf
*.hex
*.bin
*.map
*.log
```

- [ ] **Step 3: 写 `VERSION`**

```
VERSION_MAJOR = 0
VERSION_MINOR = 3
PATCHLEVEL = 0
VERSION_TWEAK = 0
EXTRAVERSION = dev
```

（FreeRTOS 版本线从 0.3.0 起，与 Zephyr 版 0.2.x 区分。）

- [ ] **Step 4: 写 `src/fw_version.h.in`**

```c
#ifndef FW_VERSION_H
#define FW_VERSION_H
#define FW_VERSION_MAJOR @FW_VERSION_MAJOR@
#define FW_VERSION_MINOR @FW_VERSION_MINOR@
#define FW_VERSION_PATCH @FW_VERSION_PATCH@
#define FW_GIT_VERSION "@FW_GIT_VERSION@"
#endif
```

- [ ] **Step 5: 写 `CMakeLists.txt`**

```cmake
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
    src/sys/syscalls.c
)
set(APP_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${CMAKE_BINARY_DIR}/generated
)

add_executable(fw.elf ${APP_SOURCES} stm32f4xx_hal_conf.h)
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
    HAL::STM32::F4::PWR
    CMSIS::STM32::F407VET6
    STM32::NoSys
)
stm32_print_size_of_target(fw.elf)
stm32_generate_hex_file(fw.elf)
```

注意：`project()` 放在 `configure_file` 之后没问题（configure_file 不依赖 project）；若 CMake 报错则把版本注入段移到 `project()` 之后。执行时以实际报错为准修正顺序，但内容不变。

- [ ] **Step 6: 写 `FreeRTOSConfig.h`**

```c
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

#define configCPU_CLOCK_HZ              ((uint32_t)168000000)
#define configTICK_RATE_HZ              ((TickType_t)1000)
#define configUSE_PREEMPTION            1
#define configUSE_TIME_SLICING          1
#define configMAX_PRIORITIES            7
#define configMINIMAL_STACK_SIZE        ((uint16_t)128)
#define configTOTAL_HEAP_SIZE           ((size_t)(16 * 1024))
#define configMAX_TASK_NAME_LEN         16
#define configUSE_16_BIT_TICKS          0
#define configIDLE_SHOULD_YIELD         1
#define configUSE_TICKLESS_IDLE         0
#define configUSE_MUTEXES               1
#define configUSE_RECURSIVE_MUTEXES     0
#define configUSE_COUNTING_SEMAPHORES   1
#define configQUEUE_REGISTRY_SIZE       0
#define configSUPPORT_STATIC_ALLOCATION 1
#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configCHECK_FOR_STACK_OVERFLOW  2
#define configUSE_MALLOC_FAILED_HOOK    0
#define configUSE_IDLE_HOOK             0
#define configUSE_TICK_HOOK             0
#define configUSE_TIMERS                1
#define configTIMER_TASK_PRIORITY       4
#define configTIMER_QUEUE_LENGTH        8
#define configTIMER_TASK_STACK_DEPTH    256

#define configPRIO_BITS                         4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY 15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY         (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

#define configASSERT(x) do { if ((x) == 0) { taskDISABLE_INTERRUPTS(); for (;;) {} } } while (0)

#define INCLUDE_vTaskDelay           1
#define INCLUDE_xTaskGetTickCount    1
#define INCLUDE_vTaskSuspend         1
#define INCLUDE_xQueueGetMutexHolder 1

#endif /* FREERTOS_CONFIG_H */
```

- [ ] **Step 7: 写 `stm32f4xx_hal_conf.h`**

```c
#ifndef STM32F4xx_HAL_CONF_H
#define STM32F4xx_HAL_CONF_H

#define HAL_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED
#define HAL_SPI_MODULE_ENABLED
#define HAL_ADC_MODULE_ENABLED
#define HAL_CAN_MODULE_ENABLED
#define HAL_IWDG_MODULE_ENABLED
#define HAL_RTC_MODULE_ENABLED
#define HAL_TIM_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED

#define HSE_VALUE    13000000U  /* 板上 13MHz 晶振 */
#define HSE_STARTUP_TIMEOUT 100U
#define LSE_VALUE    32768U
#define LSE_STARTUP_TIMEOUT 5000U
#define LSI_STARTUP_TIMEOUT 100U  /* 若该宏在 CubeF4 中叫法不同，以 deps/STM32CubeF4 模板为准 */
#define HSI_VALUE    16000000U
#define EXTERNAL_CLOCK_VALUE 12288000U

#define VDD_VALUE                    3300U
#define TICK_INT_PRIORITY            15U
#define USE_RTOS                     0U
#define PREFETCH_ENABLE              1U
#define INSTRUCTION_CACHE_ENABLE     1U
#define DATA_CACHE_ENABLE            1U

#define USE_SPI_CRC                  0U

#include "stm32f4xx_hal_rcc.h"
#include "stm32f4xx_hal_gpio.h"
#include "stm32f4xx_hal_cortex.h"
#include "stm32f4xx_hal_uart.h"
#include "stm32f4xx_hal_spi.h"
#include "stm32f4xx_hal_adc.h"
#include "stm32f4xx_hal_can.h"
#include "stm32f4xx_hal_iwdg.h"
#include "stm32f4xx_hal_rtc.h"
#include "stm32f4xx_hal_tim.h"
#include "stm32f4xx_hal_pwr.h"

#ifdef USE_FULL_ASSERT
#define assert_param(expr) ((expr) ? (void)0U : assert_failed((uint8_t *)__FILE__, __LINE__))
void assert_failed(uint8_t *file, uint32_t line);
#else
#define assert_param(expr) ((void)0U)
#endif

#endif /* STM32F4xx_HAL_CONF_H */
```

（执行时对照 `deps/STM32CubeF4/Projects/...` 的官方模板补齐缺失宏——以能编译为准，上面列出的值不许改。）

- [ ] **Step 8: 写 `src/sys/syscalls.c`（newlib 桩）**

```c
#include <sys/stat.h>
#include <errno.h>
#include "main.h"   /* extern UART 句柄，见 Step 9 main.c 定义 */

/* 最小 syscalls：printf -> USART1 阻塞发送 */
int _write(int fd, const char *buf, int len);
int _read(int fd, char *buf, int len) { (void)fd; (void)buf; (void)len; return -1; }
int _close(int fd) { (void)fd; return -1; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
int _lseek(int fd, int off, int whence) { (void)fd; (void)off; (void)whence; return -1; }
caddr_t _sbrk(int incr) { (void)incr; errno = ENOMEM; return (caddr_t)-1; } /* 全静态分配，无堆 */
```

`_write` 的实现放 `src/sys/log.c`（Task 13）；本任务先在 syscalls.c 里给阻塞版：

```c
extern UART_HandleTypeDef huart1;
int _write(int fd, const char *buf, int len)
{
    (void)fd;
    HAL_UART_Transmit(&huart1, (const uint8_t *)buf, (uint16_t)len, 100);
    return len;
}
```

- [ ] **Step 9: 写最小 `src/main.c` + `src/main.h`**

`src/main.h`：

```c
#ifndef APP_MAIN_H
#define APP_MAIN_H
#include "stm32f4xx_hal.h"
extern UART_HandleTypeDef huart1;
void Error_Handler(void);
#endif
```

`src/main.c`（本任务只验证构建链与调度器，时钟用默认 HSI 16MHz）：

```c
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "fw_version.h"

UART_HandleTypeDef huart1;

static StackType_t idle0_stack[configMINIMAL_STACK_SIZE];
static StaticTask_t idle0_tcb;
void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer,
                                   uint32_t *pulIdleTaskStackSize)
{
    *ppxIdleTaskTCBBuffer = &idle0_tcb;
    *ppxIdleTaskStackBuffer = idle0_stack;
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

static StackType_t tmr_stack[configTIMER_TASK_STACK_DEPTH];
static StaticTask_t tmr_tcb;
void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer,
                                    StackType_t **ppxTimerTaskStackBuffer,
                                    uint32_t *pulTimerTaskStackSize)
{
    *ppxTimerTaskTCBBuffer = &tmr_tcb;
    *ppxTimerTaskStackBuffer = tmr_stack;
    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask; (void)pcTaskName;
    NVIC_SystemReset(); /* 对齐 Zephyr k_sys_fatal_error_handler: 栈溢出 -> 热重启 */
}

static void hello_task(void *arg)
{
    (void)arg;
    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}

static StackType_t hello_stack[256];
static StaticTask_t hello_tcb;

int main(void)
{
    HAL_Init();
    xTaskCreateStatic(hello_task, "hello", 256, NULL, 1, hello_stack, &hello_tcb);
    vTaskStartScheduler();
    for (;;) {}
}

void Error_Handler(void) { taskDISABLE_INTERRUPTS(); for (;;) {} }
```

- [ ] **Step 10: 配置并构建**

Run（Global Constraints 中的命令）。Expected: 配置成功（若 `find_package` 因 `STM32::NoSys`/newlib 变体报链接错误，先检查 `arm-zephyr-eabi/arm-zephyr-eabi/bin` 下是否有 `libnosys.a`、`libc.a`；Zephyr SDK 自带 newlib。如 newlib 不兼容，回退方案：winget 安装 xpack arm-none-eabi 并改 `STM32_TOOLCHAIN_PATH`/`STM32_TARGET_TRIPLET=arm-none-eabi`——这是设计文档 D6 允许的回退）。构建产物 `build/fw.elf` + `build/fw.hex` 生成，`stm32_print_size_of_target` 输出 flash < 40KB（仅内核+HAL 骨架）。

- [ ] **Step 11: 提交**

```bash
git add .gitmodules deps .gitignore .clang-format VERSION CMakeLists.txt FreeRTOSConfig.h stm32f4xx_hal_conf.h src
git commit -m "build: project skeleton with stm32-cmake + FreeRTOS + HAL"
```

---

### Task 2: 时钟 168MHz + HAL TIM 时基 + 心跳灯

**Files:**
- Create: `src/board/board.c`, `src/board/board.h`, `src/board/stm32f4xx_hal_timebase_tim.c`
- Modify: `src/main.c`（调用 `board_init()`，心跳任务替代 hello 任务）

**Interfaces:**
- Produces: `void board_init(void)`（时钟 + 全部 GPIO 时钟使能）；`void heartbeat_start(void)`；168MHz 系统时钟、TIM7 作 HAL 时基（SysTick 归 FreeRTOS）

- [ ] **Step 1: 写 `src/board/board.h`**

```c
#ifndef APP_BOARD_H
#define APP_BOARD_H
#include "main.h"

#define STATUS_LED_PORT  GPIOE
#define STATUS_LED_PIN   GPIO_PIN_7

void board_init(void);
void heartbeat_start(void);
#endif
```

- [ ] **Step 2: 写 `src/board/board.c`**

时钟树来自 Zephyr 板 dts：HSE 13MHz，PLL M=13 N=336 P=2 Q=7 → 168MHz；APB1 /4 = 42MHz，APB2 /2 = 84MHz；FLASH_LATENCY_5。

```c
#include "board.h"

void board_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();

    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState = RCC_HSE_ON;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM = 13;
    osc.PLL.PLLN = 336;
    osc.PLL.PLLP = RCC_PLLP_DIV2;
    osc.PLL.PLLQ = 7;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) { Error_Handler(); }

    RCC_ClkInitTypeDef clk = {0};
    clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                    RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV4;
    clk.APB2CLKDivider = RCC_HCLK_DIV2;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5) != HAL_OK) { Error_Handler(); }
}
```

- [ ] **Step 3: 写 `src/board/stm32f4xx_hal_timebase_tim.c`**

HAL 时基改用 TIM7（SysTick 归 FreeRTOS），标准 CubeMX 模式：

```c
#include "main.h"

TIM_HandleTypeDef htim7;

HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
    __HAL_RCC_TIM7_CLK_ENABLE();
    uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
    uint32_t apb1mul = (RCC->CFGR & RCC_CFGR_PPRE1) ? 2U : 1U; /* APB1>1 分频时定时器时钟 x2 */
    uint32_t timclk = pclk1 * apb1mul;                         /* 84MHz */
    htim7.Instance = TIM7;
    htim7.Init.Prescaler = (timclk / 1000000U) - 1U;            /* 1MHz */
    htim7.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim7.Init.Period = 1000U - 1U;                             /* 1ms */
    htim7.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    if (HAL_TIM_Base_Init(&htim7) != HAL_OK) { return HAL_ERROR; }
    if (HAL_TIM_Base_Start_IT(&htim7) != HAL_OK) { return HAL_ERROR; }
    if (TickPriority < (1UL << __NVIC_PRIO_BITS)) {
        HAL_NVIC_SetPriority(TIM7_IRQn, TickPriority, 0U);
        HAL_NVIC_EnableIRQ(TIM7_IRQn);
    }
    return HAL_OK;
}

void HAL_SuspendTick(void) { __HAL_TIM_DISABLE_IT(&htim7, TIM_IT_UPDATE); }
void HAL_ResumeTick(void)  { __HAL_TIM_ENABLE_IT(&htim7, TIM_IT_UPDATE); }

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM7) { HAL_IncTick(); }
}

void TIM7_IRQHandler(void) { HAL_TIM_IRQHandler(&htim7); }
```

同时 `src/board/board_isr.h`（或在 main.h）声明并加入向量表：CubeF4 的启动文件用弱符号 `TIM7_IRQHandler`，上面强定义即可覆盖，无需改向量表。

- [ ] **Step 4: 改 `src/main.c`——`board_init()` + 心跳任务**

心跳时序来自 Zephyr 版：亮 300ms / 灭 2700ms（PE7，高有效）：

```c
#include "board.h"

static void heartbeat_task(void *arg)
{
    (void)arg;
    GPIO_InitTypeDef io = {0};
    io.Pin = STATUS_LED_PIN; io.Mode = GPIO_MODE_OUTPUT_PP; io.Pull = GPIO_NOPULL; io.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(STATUS_LED_PORT, &io);
    for (;;) {
        HAL_GPIO_WritePin(STATUS_LED_PORT, STATUS_LED_PIN, GPIO_PIN_SET);
        vTaskDelay(pdMS_TO_TICKS(300));
        HAL_GPIO_WritePin(STATUS_LED_PORT, STATUS_LED_PIN, GPIO_PIN_RESET);
        vTaskDelay(pdMS_TO_TICKS(2700));
    }
}

int main(void)
{
    HAL_Init();
    board_init();
    xTaskCreateStatic(heartbeat_task, "hb", 256, NULL, 1, hb_stack, &hb_tcb);
    vTaskStartScheduler();
    for (;;) {}
}
```

（静态栈/TCB 声明模式同 Task 1；从本任务起 `cmake --build build` 必须通过。）

- [ ] **Step 5: 构建 + 提交**

Run: 固件构建命令。Expected: 编译通过。
上机验收项（记入 Task 17 checklist）：PE7 以 3 秒周期心跳闪烁。

```bash
git add src/board src/main.c && git commit -m "board: 168MHz clock, TIM7 HAL tick, heartbeat LED"
```

---

### Task 3: util 模块（bytes + CRC）——主机 TDD

**Files:**
- Create: `src/include/io_bytes.h`, `src/util/io_bytes.c`, `src/include/io_crc.h`, `src/util/io_crc.c`
- Create: `tests/CMakeLists.txt`, `tests/test_util.h`, `tests/test_bytes_crc.c`

**Interfaces:**
- Produces:
  `uint16_t io_get_be16(const uint8_t *)`; `uint32_t io_get_be32(const uint8_t *)`; `uint32_t io_get_le32(const uint8_t *)`;
  `void io_put_be16(uint16_t, uint8_t *)`; `void io_put_be32(uint32_t, uint8_t *)`; `void io_put_le32(uint32_t, uint8_t *)`;
  `uint16_t crc16_modbus(const uint8_t *, size_t)`（poly 0xA001 反射、初值 0xFFFF，即 CRC-16/MODBUS，RTU 帧校验）;
  `uint32_t crc32_ieee(const uint8_t *, size_t)`（反射 0xEDB88320、初值/终异或 0xFFFFFFFF，config_store 用）

- [ ] **Step 1: 写测试框架 `tests/test_util.h`**

```c
#ifndef TEST_UTIL_H
#define TEST_UTIL_H
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
static int t_failures = 0;
#define TEST_ASSERT(cond) do { if (!(cond)) { t_failures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define TEST_EQ_INT(a, b) TEST_ASSERT((long long)(a) == (long long)(b))
#define TEST_EQ_MEM(a, b, n) TEST_ASSERT(memcmp((a), (b), (n)) == 0)
#define TEST_MAIN_END() do { printf("%s: %s (%d failures)\n", __func__, \
    t_failures ? "FAIL" : "PASS", t_failures); return t_failures ? 1 : 0; } while (0)
#endif
```

- [ ] **Step 2: 写 `tests/CMakeLists.txt`**

```cmake
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
```

- [ ] **Step 3: 写失败测试 `tests/test_bytes_crc.c`**

```c
#include "test_util.h"
#include "io_bytes.h"
#include "io_crc.h"

int main(void)
{
    uint8_t b[4] = {0x12, 0x34, 0x56, 0x78};
    TEST_EQ_INT(io_get_be16(b), 0x1234);
    TEST_EQ_INT(io_get_be32(b), 0x12345678U);
    TEST_EQ_INT(io_get_le32(b), 0x78563412U);

    uint8_t o[4];
    io_put_be16(0xAABB, o); TEST_EQ_INT(o[0], 0xAA); TEST_EQ_INT(o[1], 0xBB);
    io_put_be32(0x11223344U, o); TEST_EQ_INT(o[0], 0x11); TEST_EQ_INT(o[3], 0x44);
    io_put_le32(0x11223344U, o); TEST_EQ_INT(o[0], 0x44); TEST_EQ_INT(o[3], 0x11);

    const uint8_t v[] = "123456789";
    TEST_EQ_INT(crc16_modbus(v, 9), 0x4B37);      /* CRC-16/MODBUS 标准校验值 */
    TEST_EQ_INT(crc32_ieee(v, 9), 0xCBF43926U);   /* CRC-32 标准校验值 */
    TEST_MAIN_END();
}
```

- [ ] **Step 4: 跑测试确认失败**

Run: 主机测试命令。Expected: 编译失败（io_bytes.h 不存在）。

- [ ] **Step 5: 实现 `io_bytes.h/.c`、`io_crc.h/.c`**

`io_crc.c`：

```c
#include "io_crc.h"

uint16_t crc16_modbus(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (crc >> 1 ^ 0xEDB88320U) : crc >> 1;
        }
    }
    return crc ^ 0xFFFFFFFFU;
}
```

`io_bytes.c` 按接口逐个实现（移位拼装）。

- [ ] **Step 6: 跑测试通过 + 提交**

Run: 主机测试命令。Expected: `test_bytes_crc PASS`。
把 `src/util/io_bytes.c`、`src/util/io_crc.c` 加入固件 `APP_SOURCES` 并构建通过。

```bash
git add src tests && git commit -m "util: byte order helpers + CRC16-MODBUS/CRC32 (host-tested)"
```

---

### Task 4: flash 后端接口 + config_store——主机 TDD

**Files:**
- Create: `src/include/io_flash.h`, `src/storage/config_store.c`, `src/include/config_store.h`
- Test: `tests/test_config_store.c`（含 RAM flash 后端假件 `tests/fake_flash.c/.h`）

**Interfaces:**
- Produces:
  ```c
  /* io_flash.h —— NOR 后端抽象，target 用 W25Qxx，host 测试用 RAM */
  struct io_flash {
      int (*read)(uint32_t addr, uint8_t *buf, uint32_t len);
      int (*erase)(uint32_t addr, uint32_t len);            /* len 为 4096 倍数 */
      int (*write)(uint32_t addr, const uint8_t *buf, uint32_t len); /* 页编程, len<=256 */
  };
  int config_store_init(const struct io_flash *f);          /* 读取，两槽皆坏用默认 */
  int config_store_save(const struct io_cfg *cfg);          /* 写非活动槽，世代+1 */
  void config_store_erase_all(void);                        /* 恢复出厂：擦 A/B 两槽 */
  void config_store_get(struct io_cfg *out);                /* 当前生效配置(或默认) */
  void config_store_get_defaults(struct io_cfg *out);
  ```
  `struct io_cfg`（10 个持久化键，与 Zephyr settings 键一一对应）：
  ```c
  struct io_cfg {
      uint16_t di_en, ai_en, di_si, ai_si, his;
      uint16_t can_id, can_bps, rs485_bps, slave_id;
      uint16_t ip[4];   /* 4 个八位组 */
  };
  ```
- NOR 布局常量（放 `config_store.h`）：`CFG_SLOT_A=0x100000`、`CFG_SLOT_B=0x108000`、`CFG_SLOT_SIZE=0x8000`、`LFS_OFFSET=0x110000`、`LFS_SIZE=0x1000000-0x110000`
- 存储格式：每槽 `[magic 4B "IOCF"][generation u32 LE][len u16 LE][io_cfg 原生字节][crc32_ieee(magic..len..cfg)]`

- [ ] **Step 1: 写 RAM 假件 `tests/fake_flash.c/.h`**

```c
/* fake_flash.h */
#include "io_flash.h"
void fake_flash_reset(void);                    /* 全 0xFF */
void fake_flash_corrupt(uint32_t addr);         /* 翻转 1 字节 */
const struct io_flash *fake_flash_get(void);
```

实现要点：内部 `static uint8_t mem[0x100000]`（1MB，从 NOR 偏移 `0x100000` 起映射，即 `fake_mem_addr = addr - 0x100000`，覆盖 A/B 两槽 + 缩小版 littlefs 分区）；read 直接 memcpy；erase 置 0xFF；write 只允许 1→0（模拟 NOR），返回 -1 如果违反。

- [ ] **Step 2: 写失败测试 `tests/test_config_store.c`**

覆盖以下用例（每个一个 TEST 段）：
1. 空片（全 0xFF）init → 得到默认值（`di_en=0xFFFF, ai_en=0x000F, di_si=200, ai_si=200, his=0, can_id=0x0111, can_bps=250, rs485_bps=9600, slave_id=1, ip={192,168,12,101}`——与 Zephyr 版 `holding_reg[]` 初始化一致）
2. save 后重新 init → 恢复保存值
3. save 两次（不同值）→ 槽 A/B 交替（世代递增），重新 init 取最新
4. `fake_flash_corrupt` 破坏最新槽一字节 → init 回退到旧槽
5. 两槽都破坏 → init 得默认值
6. `config_store_erase_all()` 后 init → 默认值

- [ ] **Step 3: 跑测试确认失败**

Run: 主机测试命令。Expected: 编译失败（config_store 不存在）。

- [ ] **Step 4: 实现 config_store**

实现要点（完整逻辑）：
- init：分别校验 A/B 槽（magic + len==sizeof(struct io_cfg) + crc32 匹配），取 generation 大者为当前槽；无有效槽 → 内存中置默认值
- save：写非当前槽（先 erase 该槽，再 write 头+体+crc 分段，注意页 256B 对齐分段写），成功后当前槽切换
- erase_all：erase 两个槽，内存置默认值
- 所有 flash 返回 -1 时上抛 -1

- [ ] **Step 5: 跑测试通过、构建固件、提交**

Run: 主机测试 + 固件构建。Expected: PASS + 编译通过。

```bash
git add src tests && git commit -m "storage: config_store A/B slots with CRC32 (host-tested)"
```

---

### Task 5: 寄存器模型（function.c 移植）——主机 TDD

**Files:**
- Create: `src/include/init.h`（从 Zephyr 版同名文件移植：`DI_NUM/DO_NUM/AI_NUM`、`enum input_reg_idx`、`enum holding_reg_idx`、`struct his_data` **逐字保留**；寄存器数量宏直接写死 18/6）
- Create: `src/modbus/regmap.c`, `src/include/io_hooks.h`
- Test: `tests/test_regmap.c`

**Interfaces:**
- Consumes: `config_store_*`（Task 4）
- Produces（函数名与 Zephyr 版一致，后续模块按此调用）:
  ```c
  uint16_t get_holding_reg(uint16_t addr);              /* 越界返回 0 */
  int update_holding_reg(uint16_t addr, uint16_t reg);  /* 无副作用写，-1 越界 */
  uint16_t get_input_reg(uint16_t addr);
  int update_input_reg(uint16_t addr, uint16_t reg);
  uint16_t io_read_holding(uint16_t addr);              /* 0x0E/0x0F 返回实时时间 */
  int io_write_holding(uint16_t addr, uint16_t reg);    /* 带副作用的写（FC06/FC16 语义）*/
  int io_write_do_bit(uint16_t bit, bool state);        /* FC05 语义 */
  void holding_reg_save(void);                          /* 导出 io_cfg 并 config_store_save */
  void holding_reg_load(void);                          /* config_store -> holding_reg（boot 用）*/
  int io_coil_rd(uint16_t addr, bool *st);              /* coil 0-7 = DO1-8，读寄存器影子 */
  int io_discrete_rd(uint16_t addr, bool *st);          /* 0-15 = DI1-16 */
  bool ip_addr_valid(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
  ```
- `io_hooks.h`（regmap 向外部的调用点，host 测试给假件、target 给真实现）:
  ```c
  void mb_set_do(uint16_t val);            /* dio.c */
  void history_enable_write(bool en);      /* history.c */
  void history_sync(void);                 /* history.c */
  bool set_timestamp(time_t t);            /* sys/time.c，范围门 946684800..4102444800 */
  void io_reboot_cold(void);               /* sys/reboot.c */
  uint32_t io_now_epoch(void);             /* sys/time.c */
  void io_lock(void); void io_unlock(void);/* sys/os.c：FreeRTOS 互斥锁；host 为空 */
  ```

- [ ] **Step 1: 写失败测试 `tests/test_regmap.c`**

测试文件内实现全部 hooks 假件（记录调用：`fake_do_val`、`fake_hist_en`、`fake_timestamp`、`fake_reboots`、`fake_time`、锁空实现），并链接 `regmap.c + config_store.c + fake_flash.c`。用例：

1. 默认值：`get_holding_reg(0x01)==0xFFFF`、`get_holding_reg(0x03)==200`、`get_holding_reg(0x06)==0x0111`、`get_input_reg(0)==(0<<12|3<<8|0)`（版本由 fw_version.h 测试值注入）
2. `io_write_holding(0x00, 0x05)` → `fake_do_val==5`（副作用 mb_set_do 被调）
3. **同值写跳过**：`io_write_holding(0x11, 0)`（默认已 0）→ `fake_reboots==0`
4. `io_write_holding(0x11, 1)` → `fake_reboots==1` 且 `holding_reg[0x11]` 回读 0（先 history_sync 后 reboot）
5. `io_write_holding(0x10, 1)` → 触发 save（config_store 内容更新）且回读 0
6. 写 0x0E=0x00000000>>16 的 hi、再写 0x0F → `set_timestamp` 收到 `(hi<<16)|lo`
7. `io_write_holding(0x05, 1)` → `fake_hist_en==true`
8. `io_write_do_bit(3, true)` → `fake_do_val` 的 bit3 置位；`io_write_do_bit(9,..)` 返回 -1
9. `io_read_holding(0x0E/0x0F)` 返回 `fake_time` 的高/低 16 位
10. `holding_reg_load/save` 往返：改 0x09=5、0x0A..0x0D=10.20.30.40，save，重新 load（新建 config_store）→ 恢复；ip 非法（如 0.0.0.1? 注意 `ip_addr_valid` 只拒绝 `d==0||d==0xFF, a==0||a==127||a>=224`）时导出行为：Zephyr 版导出时跳过非法 ip——对齐：save 时 ip 非法则保留旧 cfg.ip
11. `io_coil_rd`：写 DO=0x0A 后 coil1=true, coil3=true；`io_discrete_rd` 从 `input_reg[5]` 取位

- [ ] **Step 2: 跑测试确认失败**

- [ ] **Step 3: 移植实现 `src/modbus/regmap.c`**

逐行对齐 Zephyr `function.c`，仅以下替换：
- `K_MUTEX_DEFINE(reg_lock)` → `io_lock()/io_unlock()`（只在 `io_write_do_bit` 与 save 导出路径加锁——**同值跳过、普通写不加锁**，保持 Zephyr 版的锁粒度）
- `settings_save()` → `holding_reg_save()` 内部组装 `struct io_cfg`（di_en=reg[0x01], ai_en=reg[0x02], di_si=reg[0x03], ai_si=reg[0x04], his=reg[0x05], can_id=reg[0x06], can_bps=reg[0x07], rs485_bps=reg[0x08], slave_id=reg[0x09], ip[i]=reg[0x0A+i]）→ `config_store_save`
- `holding_reg_load()`（新函数，对应 Zephyr settings_load 回填）：cfg → reg 上述 10 键；ip 仅当 `ip_addr_valid(cfg.ip[0..3])` 时回填
- `LOG_*` → `LOG_WRN/LOG_INF` 宏先占位为空（Task 13 替换），写成 `#define LOG_WRN(...) do{}while(0)` 段落于文件头，Task 13 统一改
- `sys_reboot(SYS_REBOOT_COLD)` → `io_reboot_cold()`；`time(NULL)` → `io_now_epoch()`；`k_msleep(100)` 从 reboot 路径移除（ reboot 前延时放到 io_reboot_cold 实现里）
- `set_timestamp` 返回值 bool（Zephyr void；UDP 用返回值判 ok——用 bool）

版本注入：`input_reg[INPUT_VER_IDX] = ((FW_VERSION_MAJOR << 12) | (FW_VERSION_MINOR << 8) | FW_VERSION_PATCH)`。host 测试用 `-DFW_VERSION_MAJOR=0 -DFW_VERSION_MINOR=3 -DFW_VERSION_PATCH=0 -DFW_GIT_VERSION=\"test\"` 编译参数注入（tests/CMakeLists 里给 regmap 源加这些定义；固件构建走 fw_version.h）。

- [ ] **Step 4: 跑测试通过、构建、提交**

```bash
git add src tests && git commit -m "regmap: register model port of function.c (host-tested)"
```

---

### Task 6: mb_server——Modbus PDU 解码器（主机 TDD，覆盖最重）

**Files:**
- Create: `src/modbus/mb_server.c`, `src/include/mb_server.h`
- Test: `tests/test_mb_server.c`（链接 regmap + config_store + fake_flash + hooks 假件）

**Interfaces:**
- Consumes: Task 5 的 `io_read_holding/io_write_holding/io_write_do_bit/io_coil_rd/io_discrete_rd/get_input_reg`
- Produces:
  ```c
  /* 处理一条 PDU（fc + data，不含 MBAP/CRC）。
   * 返回 true=有响应（out/out_len 有效）；false=静默丢弃。
   * 语义逐条对齐 zephyr/subsys/modbus/modbus_server.c（提取事实见下）。 */
  bool mb_server_process(const uint8_t *in, uint16_t in_len,
                         uint8_t *out, uint16_t *out_len);
  void mb_server_diag_count(enum mb_diag_counter c);  /* 传输层上报: MB_DIAG_CRC_ERR, MB_DIAG_NO_RESP */
  ```
  对齐事实（已在提取阶段核实）：支持 FC 01/02/03/04/05/06/08/15/16；FC01-06、08 要求 PDU data 长度==4，FC15/16 >=6，违反→**静默丢弃**（无异常响应）；异常码：未知 FC/FC08 未知子功能/FP 扩展区(地址≥5000 的 FC03/04/06/16)→0x01，回调失败→0x02，数量==0 或超限（FC01/02:2000；FC03/04/16:125；FC15/16 字节数不匹配规则）→0x03；FC05 值 0x0000=OFF 其他=ON，响应回显 addr+原值；响应 fc|0x80 + 1 字节异常码

- [ ] **Step 1: 写失败测试（每个 FC 至少：正常、异常 0x02、异常 0x03、静默丢弃四类路径）**

关键用例清单（in/out 全部写成字节数组断言）：
1. FC03 读 0x0000..2（holding DO/di_en）→ 正常响应 `03 04 xx xx xx xx`
2. FC03 地址 18 → 异常 0x02；数量 0 → 0x03；数量 126 → 0x03；地址 5000 → 0x01（FP 扩展）
3. FC03 读 0x0E/0x0F → 实时时间拼装
4. FC04 读 input；FC01/02 读 coil/discrete（含越界 0x02）
5. FC05：`05 00 03 FF 00` → do_bit(3,true)，响应回显原值；`05 00 03 00 00` → off；bit≥8 → 0x02
6. FC06：正常回显；写 0x11=1 触发 reboot hook；同值写跳过（写 0x11=0 无 reboot）
7. FC15：`0F 00 00 00 03 01 05`（qty=3, nbytes=1, bits=101b）→ do=0x05，响应回显 addr+qty；qty=0 / nbytes 不匹配 `((qty-1)/8)+1 != n` / `dlen != n+5` → 0x03
8. FC16：`10 00 01 00 02 04 00 05 00 06` → reg[1]=5, reg[2]=6；`(dlen-5)!=nbytes` 或 `nbytes != 2*qty` 或 qty>125 → 0x03；越界地址 → 0x02（注意：Zephyr 逐寄存器调 holding_reg_wr_cb，先写的已生效——保持逐个写，遇错返回异常但已写的保留）
9. FC08 子功能 0x0000 回显；0x0A 清零计数；0x0B..0x0F 返回计数器值；其他子功能 → 0x01
10. FC07/FC11（未知）→ 0x01；长度违例（如 FC03 data 5 字节）→ false 静默
11. 广播写入路径由传输层测（Task 10/11）

- [ ] **Step 2: 跑测试确认失败**

- [ ] **Step 3: 实现 `mb_server.c`**

骨架（按上述事实实现全部分支；诊断计数器 struct 保存 bus_msg/exc/srv_msg/no_resp/crc_err）：

```c
bool mb_server_process(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t *out_len)
{
    uint8_t fc = in[0];
    const uint8_t *d = in + 1;
    uint16_t dlen = in_len - 1;
    uint16_t addr = (dlen >= 2) ? io_get_be16(d) : 0;
    uint16_t qty = (dlen >= 4) ? io_get_be16(d + 2) : 0;

    switch (fc) {
    case 0x01: case 0x02: case 0x03: case 0x04:
        if (dlen != 4) { diag.bus_msg++; return false; }
        ...
    case 0x05: if (dlen != 4) return false; ...
    case 0x06: if (dlen != 4) return false; ...
    case 0x08: if (dlen != 4) return false; ...
    case 0x0F: if (dlen < 6) return false; ...
    case 0x10: if (dlen < 6) return false; ...
    default: return exc(out, out_len, fc, 0x01);
    }
}
```

- [ ] **Step 4: 跑测试通过、构建、提交**

```bash
git add src tests && git commit -m "modbus: mb_server PDU decoder for FC01-08,15,16 (host-tested)"
```

---

### Task 7: W25Qxx 驱动 + littlefs 移植层

**Files:**
- Create: `src/storage/w25qxx.c`, `src/include/w25qxx.h`, `src/storage/lfs_port.c`, `src/include/lfs_port.h`
- Test: `tests/test_lfs_port.c`（RAM flash 假件上挂载/读写/格式化恢复）

**Interfaces:**
- Produces:
  ```c
  /* w25qxx.h —— SPI1 + CS PA4，42MHz */
  int w25qxx_init(void);   /* JEDEC 0xEF4018 校验 */
  const struct io_flash *w25qxx_flash(void);  /* 实现 io_flash 后端 */
  /* 本任务即创建 src/include/io_watchdog.h（仅一个原型 void watchdog_feed(void);，实现在 Task 13），
     lfs_port 的 erase 回调与 w25qxx 擦除轮询都要喂狗 */
  /* lfs_port.h */
  int lfs_port_mount(lfs_t *lfs);   /* 挂载 LFS_OFFSET/LFS_SIZE 分区；失败则格式化重挂（喂狗由回调内完成） */
  ```
- littlefs 配置对齐 Zephyr 版：`read_size=16, prog_size=16, cache_size=1024, lookahead_size=32, block_cycles=512, block_size=4096`；erase 回调支持 4K(0x20)/32K(0x52)/64K(0xD8) 命令映射（按 len/对齐选），**每次 erase 回调内调用 `watchdog_feed()`**（对齐 Zephyr 版 mkfs 前后喂狗—— NOR 全片擦除可达分钟级）

- [ ] **Step 1: 写 `tests/test_lfs_port.c`**

RAM 假件 flash（复用 fake_flash，但容量需 ≥ LFS 测试分区——测试用缩小版：tests 里 `#define LFS_PORT_TEST_SMALL` 把 LFS_SIZE 缩到 256KB，或者 fake_flash 内存扩为 1MB 并用真实 offset）：
1. 全新片 mount → 自动格式化成功 → 建文件写读回
2. 掉电模拟：写文件、`fake_flash` 转 RAM 快照、卸载、把部分元数据字节破坏成"半擦除"态 → mount 失败 → 自动格式化 → 可用
3. `w25qxx` 驱动本身 target-only（HAL SPI），host 不测

- [ ] **Step 2: 跑失败 → 实现 → 跑通过**

`lfs_port.c` 核心结构：

```c
static int lf_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off,
                   void *buffer, lfs_size_t size)
{ return flash->read(LFS_OFFSET + block * 4096 + off, buffer, size) == 0 ? 0 : LFS_ERR_IO; }
/* prog/erase/sync 同理；erase 里 watchdog_feed()；sync 返回 0 */
```

`w25qxx.c`：命令 `0x9F`(JEDEC)/`0x06`(WREN)/`0x04`(WRDI)/`0x02`(页编程, ≤256B 且不跨页由调用方保证——io_flash 接口已约束)/`0x03`(快读)/`0x20`/`0x52`/`0xD8`(擦)；每操作 CS 拉低-命令-数据-CS 拉高；写/擦前 WREN，擦后轮询状态寄存器 busy（`0x05`，带超时 + 喂狗）；HAL_SPI_TransmitReceive 轮询模式。

- [ ] **Step 3: 构建（target）+ 测试（host）+ 提交**

```bash
git add src tests && git commit -m "storage: W25Q128 driver + littlefs port with watchdog-fed erase"
```

---

### Task 8: 历史记录（纯文件核心 + OS 壳）——主机 TDD

**Files:**
- Create: `src/history/history_file.c`, `src/include/history_file.h`（纯核心，可 host 测）
- Create: `src/history/history.c`, `src/include/history.h`（FreeRTOS 队列 + 任务壳）
- Test: `tests/test_history.c`

**Interfaces:**
- Produces（history.h，函数名对齐 Zephyr 版）:
  ```c
  void history_init(lfs_t *lfs);        /* 启动任务（main 调） */
  bool send_history_data(const struct his_data *d);  /* 生产者投递：关/未挂载静默丢弃；队满丢+计数 */
  void history_enable_write(bool en);
  void history_sync(void);              /* lfs_file_sync，带 hist_lock */
  ```
  history_file.h（纯核心）:
  ```c
  int hist_file_init(lfs_t *lfs);       /* boot 扫描续写逻辑 */
  int hist_file_write(const struct his_data *d);  /* 追加+轮转+清理 */
  int hist_file_sync(void);
  void hist_file_close(void);
  ```
- 对齐事实：DI 记录 10B、AI 记录 16B（packed struct 写盘长度按 type 截断）；文件名 `data_MMDD_HHMMSS.raw`（epoch+8h 后 gmtime，字段截断保护，NULL 回退 `data_0101_000000.raw`）；单文件 1MB（`#ifndef HIST_FILE_MAX` 可被测试覆盖）；保留 10 份（新建文件时删字典序最小直至 ≤10）；启动续写：恢复 `his_cur_name` → 否则扫目录字典序最大且 <1MB 追加 → 否则新建；禁用→再启用续写同一文件；`HIST_DIR` 对应 lfs 根 `/`

- [ ] **Step 1: 写失败测试 `tests/test_history.c`**

链接 `history_file.c + lfs_port.c + littlefs + fake_flash`，`-DHIST_FILE_MAX=4096`、时间注入用 history_file.c 提供的专用接口 `void hist_file_set_clock(time_t (*fn)(void))`（target 不调用，host 测试注入固定时间）。用例：
1. 写 1 条 DI 记录 → 文件出现，尺寸 10B，内容逐字节比对（type=1 LE、timestamp LE、en、value）
2. 写到超 4096B → 轮转出新文件（文件名含注入时间）；旧文件保留
3. 连续建 12 个文件后触发清理 → 目录里只剩 10 个字典序最大的
4. `hist_file_close` 后再 write → 续写同一文件（不新建）
5. 重新 `hist_file_init`（模拟重启）→ 续写字典序最大的未满文件
6. AI 记录 16B 布局

- [ ] **Step 2: 跑失败 → 实现（移植 Zephyr history.c 逻辑到 lfs 原生 API）→ 跑通过**

移植映射：`fs_open(FS_O_APPEND|FS_O_CREATE|FS_O_WRONLY)`→`lfs_file_open(..., LFS_O_APPEND|LFS_O_CREAT|LFS_O_WRONLY)`；`fs_tell/fs_tell`→`lfs_file_size`（打开句柄上 `lfs_file_tell`）；`fs_unlink`→`lfs_remove`；`fs_opendir/readdir`→`lfs_dir_open/read`；`fs_sync`→`lfs_file_sync`；轮转/清理/命名逻辑**逐行保留**。

`history.c` OS 壳：`xQueueCreateStatic(16, sizeof(struct his_data), ...)`；任务 prio 2 / 栈 1024 字（4096B，对齐 Zephyr hist workq 栈）；`history_enable_write(false)` 不再走 workq——直接置 volatile 标志，关闭动作在任务内下次迭代处理（保持"disable 异步关闭"语义）；`send_history_data` 队满时 `drop_cnt++`（对齐 Zephyr atomic 计数，每 4 次告警日志）；`history_sync` 持 `hist_lock`（静态递归？普通静态 Mutex）保护 lfs 并发（write 与 sync 互斥）。

- [ ] **Step 3: 构建 + 提交**

```bash
git add src tests && git commit -m "history: littlefs-backed recorder port (host-tested)"
```

---

### Task 9: W5500 网络层（ioLibrary 集成 + socket 池 + 链路监控）

**Files:**
- Create: `src/net/w5500.c`, `src/include/w5500.h`, `src/board/spi.c`（SPI1/SPI2 初始化，供 w25qxx/w5500 共用）
- Modify: `CMakeLists.txt`（编译 `deps/ioLibrary/Ethernet/*.c` + `deps/ioLibrary/Internet/*.c` 仅 W5500 目录、链接 littlefs）

**Interfaces:**
- Produces:
  ```c
  int w5500_net_init(const uint8_t mac[6], const uint8_t ip[4],
                     const uint8_t mask[4], const uint8_t gw[4]);
  bool w5500_link_up(void);          /* PHYCFGR.LNK 轮询缓存 */
  /* socket 池（集中分配，二期 web/FTP 从这里拿） */
  #define SN_UDP_CFG   0
  #define SN_MB_BASE   1             /* 1,2,3 归 Modbus TCP */
  int  sn_alloc(uint8_t *sn);        /* 从空闲池取 */
  void sn_free(uint8_t sn);
  ```
- 实现要点：SPI2（PB13/14/15，CS PB12 软控，21MHz 全双工轮询）实现 ioLibrary 回调（`wizchip_cs_control`/`wizchip_spi_readbyte`...—— 用 `wizchip_conf.h` 的 `wizchip_init` 注册，具体函数集以 `deps/ioLibrary/Ethernet/wizchip_conf.h` 为准）；RST PD0 复位时序（低 50ms→高 50ms）；`wizchip_init` 后 `wizchip_setnetinfo`（mac/ip/sn=255.255.255.0/gw=ip 末字节改 1/dns=0.0.0.0）；socket 缓冲 2KB×8；`WIZCHIP_READ(PHYCFGR)` bit0 轮询（500ms 周期，在 net 监控任务里）链接状态，下降沿 → `update_holding_reg(HOLDING_DO_IDX, 0); mb_set_do(0);`（对齐 Zephyr `NET_EVENT_IF_DOWN` 行为）；INT 脚 PD1 第一期不接
- net 监控任务（prio 4，栈 256 字）：轮询链路 + 上升沿 give `net_link_sem`（boot 等待，对齐 main.c 5s 等待语义）

- [ ] **Step 1: 实现（本任务 target-only，host 不测）**

- [ ] **Step 2: 构建通过**

- [ ] **Step 3: 提交**

```bash
git add src CMakeLists.txt && git commit -m "net: W5500 ioLibrary integration, socket pool, link monitor"
```

上机验收项：ping 192.168.12.101 通；拔网线后 DO 全灭、插回后恢复可配置。

---

### Task 10: Modbus TCP 服务器（MBAP 逻辑主机测试 + W5500 传输层）

**Files:**
- Create: `src/modbus/mbtcp_adu.c`, `src/include/mbtcp_adu.h`（纯逻辑）
- Create: `src/modbus/tcp.c`（W5500 socket 状态机任务）
- Test: `tests/test_mbtcp_adu.c`

**Interfaces:**
- Consumes: `mb_server_process`（Task 6）
- Produces:
  ```c
  /* 输入完整 MBAP+PDU 帧给 srv_unit（从站号），输出响应帧。
   * 返回 1=发送响应；0=静默（广播/畸形长度）。 */
  int mbtcp_adu_process(const uint8_t *in, uint16_t in_len,
                        uint8_t *out, uint16_t out_cap, uint16_t *out_len,
                        uint8_t srv_unit);
  /* tcp.c */
  void mb_tcp_start(void);   /* main 调 */
  ```
- 对齐事实（提取自 tcp.c + modbus_raw.c）：proto_id≠0 → 构造 server-failure ADU 响应（`fc|0x80, data[0]=0x04, len=1`，trans 回显、unit 回显原始值）；MBAP length 字段 `MIN(len,256)-2` 钳制；unit≠0 → 改写为 srv_unit 后处理（**mb_server 接受任意 unit**，改写仅影响内部；响应 unit 恒为请求原始 unit）；unit==0 → 副作用执行、无任何响应（`mb_server_diag_count(MB_DIAG_NO_RESP)`）；PDU 长度违例 → 静默；单次 `send()` 合并 MBAP+PDU（W5500 send 天然一次）；事务 ID 回显

- [ ] **Step 1: 写失败测试 `tests/test_mbtcp_adu.c`**

用例：
1. 正常 FC03 帧（trans=0xABCD, unit=1）→ 响应 trans=0xABCD、unit=1、功能码 03
2. unit=0x05（非广播非从站号）→ **照常响应且响应 unit=0x05**（现版行为：改写后处理）
3. unit=0x00 广播 FC06 写 → 寄存器已写、返回 0（无响应）
4. proto_id=0x0001 → server-failure 响应（`fc|0x80 04`）
5. FC03 data 长度违例 → 返回 0

- [ ] **Step 2: 跑失败 → 实现 `mbtcp_adu.c` → 跑通过**

- [ ] **Step 3: 实现 `tcp.c`（W5500 socket 状态机）**

设计（W5500 accept 语义：listen socket 自身变 ESTABLISHED，需另开 socket 继续监听）：
- 池：Sn1/Sn2/Sn3。不变量：**至多 1 个 LISTEN、至多 2 个 ESTABLISHED**
- 每轮（100ms 周期）：对 LISTEN socket 调 `accept()`，返回本 socket 号即有连接 → 该 socket 转数据态，立即在空闲 socket 上 `socket()+bind(502)+listen()`；对 ESTABLISHED socket：`getsockopt(sn, SO_STATUS, &sr)`，`SOCK_CLOSED` → close+回收（回收后若无 LISTEN 且有空闲 → 重新 listen）；`recv()` 有数据 → 状态机收满 `6+2+length` 字节（MBAP length 已知）或 500ms 超时断开 → `mbtcp_adu_process` → 有响应则 `send()`；收中途对端关闭 → 回收
- accept 的连接写 `Sn_KPALVTR=6`（30s keepalive）
- `net_link_up()==false` 时不 accept 新连接（对齐 tcp.c）
- 线程：prio 3、栈 1024 字（对齐 Zephyr 2048B）
- `SO_REUSEADDR`：W5500 无此选项，绑定时若 TIME_WAIT 由 close 后重开处理——记录为已知实现差异（无行为影响）

- [ ] **Step 4: 构建 + 上机项（pytest modbus TCP 用例）+ 提交**

```bash
git add src tests && git commit -m "modbus: TCP server on W5500 with MBAP logic (host-tested)"
```

---

### Task 11: Modbus RTU 从站（帧状态机主机测试 + USART2/DE 传输层）

**Files:**
- Create: `src/modbus/rtu_frame.c`, `src/include/rtu_frame.h`（纯逻辑）
- Create: `src/modbus/rtu.c`（USART2 + DE + 定时器传输层）
- Test: `tests/test_rtu_frame.c`

**Interfaces:**
- Consumes: `mb_server_process`、`crc16_modbus`
- Produces:
  ```c
  /* rtu_frame.h —— 纯状态机，传输层喂数据/超时，输出响应帧 */
  void rtu_reset(void);
  void rtu_rx_feed(const uint8_t *bytes, uint16_t len);  /* 喂收到的字节，内部重启 t35 定时钩子 */
  /* t35 到期钩子：由传输层定时器调用；需要发响应时经 tx 回调输出 */
  void rtu_t35_expired(void);
  /* 注入回调（host 测试与 target 各自注册） */
  void rtu_frame_bind(uint8_t srv_unit,
                      uint32_t baud,
                      void (*tx)(const uint8_t *frame, uint16_t len));
  /* rtu.c */
  void mb_rtu_start(void);
  ```
- 对齐事实：帧 `[unit][pdu...][crc16 LE16]`；长度 <4 或 >256 → 静默丢弃；CRC 错 → 静默 + `mb_server_diag_count(MB_DIAG_CRC_ERR)`；unit 非本站且非 0 → 静默；unit 0 → 执行副作用无响应（`MB_DIAG_NO_RESP`）；从站号/波特率**启动时固定**（读 reg 0x09/0x08），运行期写只存不生效（对齐现版）；t3.5 = 3.5×11bit/baud，>19200bps 固定 2ms

- [ ] **Step 1: 写失败测试 `tests/test_rtu_frame.c`**

用例（构造帧时用 `crc16_modbus` 现算）：
1. 合法 FC03 请求（unit=从站号）→ tx 收到正确响应帧（含 CRC）
2. CRC 破坏 1 字节 → 无 tx
3. unit=其他值 → 无 tx；unit=0 广播 FC06 → 寄存器已写、无 tx
4. 长度 3 字节 → 无 tx
5. 长度 257 → 溢出保护、无 tx、状态复位

- [ ] **Step 2: 跑失败 → 实现 → 跑通过**

- [ ] **Step 3: 实现 `rtu.c` 传输层**

- USART2 PA2/PA3，8N1，波特率=reg 0x08（默认 9600），DE=PA1 推挽输出
- 接收：`HAL_UARTEx_ReceiveToIdle_IT(&huart2, buf, 256)` + `HAL_UARTEx_RxEventCallback`（USART2）→ `rtu_rx_feed(buf, size)` 后重启接收；t3.5 用 FreeRTOS 软件定时器（单次，周期按波特率算）；到期回调 `xTaskNotifyGive(rtu_task)`，RTU 任务（prio 5、栈 512 字）里调 `rtu_t35_expired()`
- 发送：DE 置高 → `HAL_UART_Transmit_IT` → `HAL_UART_TxCpltCallback` 里 DE 拉低
- USART2 IRQ 优先级 6（≥5，FromISR 通知合法）
- `mb_rtu_start()`：绑 srv_unit/reg 0x09、baud/reg 0x08、tx 回调

- [ ] **Step 4: 构建 + 上机项（Modbus Poll 9600 读写）+ 提交**

```bash
git add src tests && git commit -m "modbus: RTU slave with IDLE framing + RS485 DE control (host-tested)"
```

---

### Task 12: UDP 配置服务（命令处理主机测试 + W5500 UDP 层）

**Files:**
- Create: `src/net/udp_cfg.c`, `src/include/udp_cfg.h`（纯命令处理）
- Create: `src/net/udp_task.c`（W5500 UDP socket 任务 + 路由）
- Test: `tests/test_udp_cfg.c`

**Interfaces:**
- Consumes: regmap、`set_timestamp`、`config_store_erase_all`、`history_sync`、`io_reboot_cold`
- Produces:
  ```c
  /* 处理一条命令；返回应答长度（0=静默）。reply[0]=cmd。 */
  uint16_t udp_app_cmd(uint8_t cmd, const uint8_t *data, uint16_t len,
                       uint8_t *reply, uint16_t cap);
  bool udp_cmd_bcast_allowed(uint8_t cmd);  /* 跨网段白名单：仅 0x11 */
  void udp_cfg_start(void);
  ```
- 命令格式逐字节对齐（提取事实）：无 magic/长度/CRC；应答缓冲 64B（数据 ≤63B）：
  - 0x10 SET_IP `[a][b][c][d]` → 校验 `ip_addr_valid`（拒 d==0/0xFF、a==0/127/≥224）→ 写 reg 0x0A-0x0D + save；应答 `[0x10][ok]`（非法输入也**总是应答** ok=0）
  - 0x11 GET_IP → 应答 `[0x11][a][b][c][d]`
  - 0x12 SET_MODBUS `[slave_id 1B][rs485_baud BE16]` → 写 reg 0x09/0x08 + save（重启生效）；应答 `[0x12][ok]`（len<3 → ok=0）
  - 0x13 GET_MODBUS → `[0x13][slave_id][rs485_baud BE16]`
  - 0x14 SET_TIME `[unix BE32]` → `set_timestamp` 成功与否 → `[0x14][ok]`
  - 0x19 FACTORY_RESET 两步确认：首次（距上次 >5000ms）→ `[0x19][00]`；5000ms 内第二次 → `config_store_erase_all()` → `[0x19][01]` → `history_sync()` → 100ms → 冷重启。**保留现版怪癖**：计数起点 0，开机 5 秒内的第一条即视为确认步（单命令直接执行）
  - 未知命令（含 0x01–0x06 固件升级命令，第一期无 MCUboot）→ 返回 0（静默，对齐 lib 未处理行为）
- 路由（udp_task.c）：同网段（`src_ip & /24 == local_ip & /24`）→ 单播回源 IP:源端口；跨网段 → 命令在白名单则回 `255.255.255.255:8601`，否则**静默丢弃不执行**；本地 IP/掩码取自 holding regs + 固定 /24

- [ ] **Step 1: 写失败测试 `tests/test_udp_cfg.c`**（hooks 假件记录 reboot/save/erase；时间用注入）

用例：SET_IP 合法/非法、GET_IP、SET_MODBUS、SET_TIME 范围门（<2000 年拒绝）、FACTORY_RESET 两步（含 5 秒内/外、开机 5 秒内单命令立即执行怪癖）、未知命令静默。

- [ ] **Step 2: 跑失败 → 实现 → 跑通过**

- [ ] **Step 3: 实现 `udp_task.c`**：Sn=SN_UDP_CFG，`socket(SOCK_MR_UDP, 8600)`，循环 `recvfrom` → 路由判定 → `udp_app_cmd` → `sendto`。任务 prio 4、栈 512 字。

- [ ] **Step 4: 构建 + 上机项（pytest UDP 用例）+ 提交**

```bash
git add src tests && git commit -m "net: UDP config protocol 0x10-0x19 (host-tested)"
```

---

### Task 13: time / watchdog / reboot / log / os 胶水

**Files:**
- Create: `src/sys/time.c`, `src/include/io_time.h`, `src/sys/watchdog.c`, `src/include/io_watchdog.h`, `src/sys/reboot.c`, `src/sys/log.c`, `src/include/log.h`, `src/sys/os.c`

**Interfaces:**
- Produces:
  ```c
  /* io_time.h */
  bool set_timestamp(time_t t);      /* 范围门 946684800(2000)..4102444800(2100)，写 RTC+缓存 epoch */
  uint32_t io_now_epoch(void);       /* 缓存 epoch（boot 从 RTC 载入，1Hz 软件定时器 +1）*/
  void io_time_init(void);           /* RTC/LSE 初始化（HAL_RTC_GetTime 后必须再 GetDate）*/
  /* io_watchdog.h */
  void watchdog_init(void);          /* IWDG 30s: PSC=256, LSI 32kHz -> 125Hz, reload=3750; __HAL_DBGMCU_FREEZE_IWDG() */
  void watchdog_feed(void);
  /* reboot.c */
  void io_reboot_cold(void);         /* history 已由调用方 sync；100ms 延时 + NVIC_SystemReset */
  void set_reboot_status(bool en); bool get_reboot_status(void);  /* RAM volatile bool，对齐现版 */
  /* log.h */
  #define LOG_ERR(...) / LOG_WRN(...) / LOG_INF(...)  /* USART1 115200 + epoch+8h 时间戳；LOG_ENABLE=0 时空宏 */
  /* os.c: io_lock/io_unlock = 静态 FreeRTOS 互斥锁 */
  ```
- regmap.c 中 Task 5 的占位 LOG 宏改为 `#include "log.h"`；syscalls.c 的 `_write` 移交 log.c（USART1 句柄在 board 初始化）

- [ ] **Step 1: 实现（time 范围门逻辑补 3 个主机用例进 `tests/test_udp_cfg.c` 或独立小测试）**

```c
TEST_ASSERT(!set_timestamp(0));                     /* < 2000 年 */
TEST_ASSERT(set_timestamp(1600000000));
TEST_ASSERT(!set_timestamp(5000000000LL));          /* > 2100 年 */
```
（host 编译 time.c 时 HAL 部分用 `#ifdef HOST_TEST` 分离——范围门提取成纯函数 `ts_in_range(time_t)`。）

- [ ] **Step 2: 构建通过（含 regmap LOG 替换）+ 主机测试通过 + 提交**

```bash
git add src tests && git commit -m "sys: RTC time, IWDG, reboot, logging"
```

---

### Task 14: DI/DO/ADC 任务

**Files:**
- Create: `src/io/dio.c`, `src/io/adc.c`, `src/include/io.h`

**Interfaces:**
- Produces: `int mb_set_do(uint16_t val)`（DO+LED 镜像，对齐 Zephyr 版）；`void dio_start(void)`、`void adc_start(void)`；AI 换算纯函数 `uint16_t ai_convert(uint8_t ch, int32_t raw)`
- 引脚表（来自 overlay，**顺序即通道号**）：
  - DI1-16：PD3 PD4 PD5 PD6 PB5 PB6 PB7 PB8 PB9 PB10 PB11 PD2 PB0 PB1 PB3 PB4（输入下拉、高有效）
  - DO1-8：PD7..PD14；LED1-8：PE8..PE15（推挽输出，初始低）
  - ADC：ADC1 IN10-13 = PC0-PC3，12bit，系数 {7414,7414,3704,3704}
- DI 线程：周期 = reg 0x03 钳 [10,5000]ms；单次瞬时读、无消抖；禁用通道读 0；`update_input_reg(INPUT_DI_IDX, val)`；`en!=0` 时产 his_data 投递。prio 6、栈 256 字
- ADC 线程：周期 = reg 0x04 同钳法；逐通道单次转换（HAL_ADC 轮询，采样时间 144 周期，ADCCLK=PCLK2/4=21MHz——记录偏差：Zephyr overlay prescaler=2 疑似 42MHz 超芯片规格，取 /4 更安全）；换算 `raw*3300/4096` → `coeff*mv/10000`（64 位中间量）；`en&0xF` 时产 AI 记录。prio 6、栈 256 字
- `ai_convert` 进主机测试：`ai_convert(0, 4095)` = 7414*3300*4095/4096/10000 = 2449（4-20mA 通道满量程≈24.49mA... 断言按公式手算值）；`ai_convert(2, 4095)` = 3704*3300/10000 ≈ 1222

- [ ] **Step 1: `tests/test_adc_math.c`**（ai_convert）→ 失败 → 实现 → 通过
- [ ] **Step 2: 实现 dio.c/adc.c（HAL 部分）→ 构建通过**
- [ ] **Step 3: 提交**

```bash
git add src tests && git commit -m "io: DI/DO/ADC tasks ported"
```

---

### Task 15: main 初始化顺序 + 版本注入接线

**Files:**
- Modify: `src/main.c`（完整初始化序列）
- Modify: `src/sys/syscalls.c`（若 _write 已移 log.c 则清理）

**Interfaces:**
- Consumes: 前面全部模块的 start/init 函数
- 初始化顺序（对齐 SYS_INIT 级别序，设计文档 §6.2）：
  ```
  HAL_Init → board_init(时钟/GPIO)
  → io_time_init(RTC→epoch) → log 可用
  → w25qxx_init → config_store_init(挂在 w25qxx flash) → holding_reg_load
  → timestamp 影子寄存器刷新(update_holding_reg(0x0E/0x0F, now))
  → lfs_port_mount → history_init → history_enable_write(reg 0x05!=0)
  → dio_start → adc_start
  → can_start(T16 若已实现，顺序放这)
  → w5500_net_init(MAC=UID 派生, IP=reg) → 链路等待 ≤5s
  → mb_tcp_start → mb_rtu_start → udp_cfg_start
  → watchdog_init(IWDG 30s)
  → 心跳循环：喂狗 + LED 300/2700ms + get_reboot_status() 检查（delayed reboot: log + history_sync + 1s + io_reboot_cold）
  ```
- MAC 派生（对齐 main.c）：OUI {0x00,0x08,0xDC} + 12B UID（`UID_BASE` 内存序）XOR 折叠：`mac[3]=uid[0]^uid[3]^uid[6]^uid[9]`、`mac[4]=uid[1]^uid[4]^uid[7]^uid[10]`、`mac[5]=uid[2]^uid[5]^uid[8]^uid[11]`；读取失败回退 01:02:03
- 开机 banner 对齐：`__DATE__/__TIME__`、板名、flash/RAM、`v%d.%d.%d_%s`

- [ ] **Step 1: 实现 → 构建通过**
- [ ] **Step 2: 提交**

```bash
git add src && git commit -m "main: full init sequence, MAC from UID, heartbeat+watchdog loop"
```

---

### Task 16: CAN 初始化（对齐现版：无周期推送）

**Files:**
- Create: `src/net/can.c`, `src/include/io_can.h`

**Interfaces:**
- Produces:
  ```c
  void can_start(void);
  int mod_can_send(uint32_t id, const uint8_t *data, uint8_t len);  /* len>8 → -1；100ms 邮箱等待 */
  ```
- 对齐事实：波特率 = reg 0x07 ∈ {50,100,125,250,500,800,1000}×1000，非法回退 250k；RX 过滤业务 ID（reg 0x06，默认 0x0111）帧静默消费；无周期发送。PA11/PA12。
- **800k 偏差（记录）**：42MHz APB1 无法整除出 800k（42e6/8e5=52.5），选 {50,100,125,250,500,1000} 精确表 + 800k 警告回退 250k（Zephyr 版接受了 800k 但硬件精度存疑，记录为已知差异）
- 位时序表（APB1 42MHz，同步段 1 tq）：50k:PSC=105,BS1=6,BS2=1（87.5%）；100k:PSC=42,BS1=8,BS2=1（90%）；125k:PSC=42,BS1=6,BS2=1（87.5%）；250k:PSC=21,BS1=6,BS2=1；500k:PSC=12,BS1=5,BS2=1（85.7%）；1000k:PSC=6,BS1=5,BS2=1
- 实现：HAL_CAN 标准帧、FIFO0 精确 ID 硬件过滤、RX 中断（优先级 6）释放 FIFO 丢弃（计数）、TX 用 `HAL_CAN_AddTxMessage` + 邮箱轮询超时 100ms；CAN IRQ 中只 HAL 操作（无需 FromISR）
- Modify: `src/main.c`——在 `adc_start()` 之后、网络初始化之前插入 `can_start();`

- [ ] **Step 1: 实现 → 构建通过**
- [ ] **Step 2: 提交**

```bash
git add src && git commit -m "net: CAN init with bitrate table, silent consume, mod_can_send"
```

---

### Task 17: release 构建 + 文档 + 上机验收清单

**Files:**
- Modify: `CMakeLists.txt`（`option(APP_RELEASE "..." OFF)` → `target_compile_definitions(fw.elf PRIVATE LOG_ENABLE=$<NOT:$<BOOL:${APP_RELEASE}>))`）
- Create: `README.md`, `docs/ACCEPTANCE.md`, `.gitmodules` 校验
- Create: `docs/KNOWN_DEVIATIONS.md`（汇总已知偏差）

**内容：**

- `README.md`：仓库结构、submodule 初始化（`git submodule update --init --recursive`）、两条构建命令（固件/主机测试）、工具链来源（Zephyr SDK 复用 + xpack 回退）、烧录（SWD，0x08000000）
- `docs/ACCEPTANCE.md`（上机清单，逐条勾选）：
  1. pytest 套件：Zephyr 版 `applications/io-edge-hub/tests/` 中 Modbus TCP 与 UDP 用例直接复用（协议不变）
  2. Modbus RTU：Modbus Poll 9600 8N1，FC01/03/05/06/15/16 通过、广播写生效不应答
  3. 心跳/看门狗：PE7 3s 周期；程序挂死 >30s 自动复位；调试器挂起不死机
  4. 历史记录：使能 reg 0x05，断电重启后文件续写、10 份轮转、FTP（二期）前用 pytest/读回验证
  5. DO 安全：拔网线 DO 全灭；插回不自动恢复
  6. UDP：SET_IP/GET_IP/SET_TIME/SET_MODBUS/FACTORY_RESET 两步；跨网段仅 GET_IP 应答（8601）
  7. Modbus TCP：keepalive 30s（拔线 45-60s 内连接断开）、2 并发客户端、unit 0 广播
  8. RTC：SET_TIME 断电保持
  9. 版本：input_reg[0] = (0<<12|3<<8|0)；UDP 0x04 静默（一期无升级）
- `docs/KNOWN_DEVIATIONS.md`：Modbus TCP 并发上限 2；keepalive 单参数（30s 定时探测）；800k CAN 回退 250k；ADC 时钟 /4（21MHz，Zephyr 疑似 42MHz 超规格）；WSL 主机测试路径；SO_REUSEADDR 不适用
- size 预算校验：`stm32_print_size_of_target` 输出 flash < 300KB、SRAM < 128KB（超了要分析并报告，不是硬失败）

- [ ] **Step 1: 实现 release 选项 + 全量构建（Debug 与 Release 两个 build 目录）+ 主机测试全绿**
- [ ] **Step 2: 写三份文档**
- [ ] **Step 3: 最终提交**

```bash
git add CMakeLists.txt README.md docs && git commit -m "docs: acceptance checklist, known deviations, release build option"
```

---

## 计划自检记录

- **Spec 覆盖**：设计文档 §2 功能清单逐项落到 T5(regmap)/T6+T10+T11(Modbus)/T12(UDP)/T8(历史)/T14(IO)/T16(CAN)/T13(系统服务)/T15(初始化+MAC)；§4 依赖 → T1；§6.1 内核 → T1 配置；§6.3 网络 → T9/T10；§6.4 → T6/T10/T11；§6.5 → T4/T7/T8；§9 验证 → 各任务 host 测试 + T17 checklist。无遗漏。
- **占位符**：无 TBD/TODO；HAL conf 与 ioLibrary 回调两处标注"以 deps 内模板/头文件为准核对"——这是对第三方源的核对指令，非占位。
- **类型一致性**：`io_flash`/`io_cfg`/hooks/`mb_server_process`/`mbtcp_adu_process`/`rtu_frame_bind`/`udp_app_cmd` 在生产/消费任务间签名已核对一致。
