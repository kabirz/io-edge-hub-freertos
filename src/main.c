#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "board.h"
#include "fw_version.h"
#include "io_hooks.h"    /* os_init / get_reboot_status / history_sync ... */
#include "io_time.h"     /* io_time_init */
#include "io_watchdog.h" /* watchdog_init/feed */
#include "log.h"         /* log_init / LOG_INF */

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

/* 心跳 = Zephyr 版 main 主循环的对应物: 喂狗 + 状态 LED + 延迟重启轮询 */
static void heartbeat_task(void *arg)
{
    (void)arg;
    GPIO_InitTypeDef io = {0};
    io.Pin = STATUS_LED_PIN; io.Mode = GPIO_MODE_OUTPUT_PP; io.Pull = GPIO_NOPULL; io.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(STATUS_LED_PORT, &io);
    for (;;) {
        watchdog_feed(); /* 3s 周期 << 30s IWDG 窗口 */

        HAL_GPIO_WritePin(STATUS_LED_PORT, STATUS_LED_PIN, GPIO_PIN_SET);
        vTaskDelay(pdMS_TO_TICKS(300));
        HAL_GPIO_WritePin(STATUS_LED_PORT, STATUS_LED_PIN, GPIO_PIN_RESET);
        vTaskDelay(pdMS_TO_TICKS(2700));

        if (get_reboot_status()) { /* 对齐 Zephyr 主循环: sync 后延时重启 */
            LOG_INF("delayed reboot");
            history_sync();
            io_reboot_cold(); /* 内含 100ms 延时 */
        }
    }
}

/* 512 字 = 2048B: 心跳现承担喂狗 + 延迟重启 (history_sync -> littlefs
 * 落盘 + LOG 160B 格式化缓冲), 256 字对 FPU 上下文(132B)+vfprintf 深度
 * 余量不足 */
static StackType_t hb_stack[512];
static StaticTask_t hb_tcb;

int main(void)
{
    HAL_Init();
    board_init();
    log_init();      /* USART1 日志最先就绪, 后续 init 的 LOG 可见 */
    os_init();       /* io_lock 互斥锁, 先于任何任务/持锁路径 */
    io_time_init();  /* LSE+RTC (起振秒级) -> epoch 缓存 + 1Hz 定时器 */
    watchdog_init(); /* IWDG 30s; 心跳任务 + fs 长擦除事件型喂狗 */

    LOG_INF("io-edge-hub v%d.%d.%d_%s", FW_VERSION_MAJOR, FW_VERSION_MINOR,
            FW_VERSION_PATCH, FW_GIT_VERSION);

    xTaskCreateStatic(heartbeat_task, "hb", 512, NULL, 1, hb_stack, &hb_tcb);
    vTaskStartScheduler();
    for (;;) {}
}

void Error_Handler(void) { taskDISABLE_INTERRUPTS(); for (;;) {} }
