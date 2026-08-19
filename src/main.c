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
