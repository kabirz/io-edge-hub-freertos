#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "board.h"
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

static StackType_t hb_stack[256];
static StaticTask_t hb_tcb;

int main(void)
{
    HAL_Init();
    board_init();
    xTaskCreateStatic(heartbeat_task, "hb", 256, NULL, 1, hb_stack, &hb_tcb);
    vTaskStartScheduler();
    for (;;) {}
}

void Error_Handler(void) { taskDISABLE_INTERRUPTS(); for (;;) {} }
