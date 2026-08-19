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

/* SysTick 归 FreeRTOS：把内核 tick ISR 映射到 CMSIS 向量名，强符号覆盖启动文件的弱定义。
   HAL 时基走 TIM7（见 src/board/stm32f4xx_hal_timebase_tim.c）。 */
#define xPortSysTickHandler SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
