/*
 * LwIP sys_arch port for FreeRTOS (STM32F407 + W5500 MACRAW).
 * Implements the OS-dependent functions required by LwIP's NO_SYS=0 mode.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <lwip/sys.h>
#include <lwip/err.h>

/* ---- sys_init (called by lwip_init, no-op for FreeRTOS) ---- */

void sys_init(void)
{
    /* FreeRTOS manages all OS resources; nothing to do here. */
}

/* ---- mbox ---- */

err_t sys_mbox_new(sys_mbox_t *mb, int size)
{
    /* lwip 传入的 size 可能为 0 (未覆盖的 DEFAULT_*_MBOX_SIZE);
     * 长度 0 的队列会触发 configASSERT, 兜底为 1 */
    if (size < 1) {
        size = 1;
    }
    *mb = xQueueCreate((UBaseType_t)size, sizeof(void *));
    return (*mb == NULL) ? ERR_MEM : ERR_OK;
}

void sys_mbox_free(sys_mbox_t *mb)
{
    vQueueDelete(*mb);
}

void sys_mbox_post(sys_mbox_t *mb, void *msg)
{
    (void)xQueueSend(*mb, &msg, portMAX_DELAY);
}

err_t sys_mbox_trypost(sys_mbox_t *mb, void *msg)
{
    return xQueueSend(*mb, &msg, 0) == pdTRUE ? ERR_OK : ERR_MEM;
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mb, void **msg, u32_t timeout)
{
    TickType_t ticks = (timeout == 0) ? portMAX_DELAY :
                        pdMS_TO_TICKS(timeout);
    if (xQueueReceive(*mb, msg, ticks) == pdTRUE) {
        return 0; /* success */
    }
    *msg = NULL;
    return SYS_ARCH_TIMEOUT;
}

int sys_mbox_valid(sys_mbox_t *mb)
{
    return (*mb != NULL) ? 1 : 0;
}

void sys_mbox_set_invalid(sys_mbox_t *mb)
{
    *mb = NULL;
}

u32_t sys_mbox_tryfetch(sys_mbox_t *mb, void **msg)
{
    if (xQueueReceive(*mb, msg, 0) == pdTRUE) {
        return ERR_OK;
    }
    return SYS_MBOX_EMPTY;
}

/* ---- semaphore ---- */

err_t sys_sem_new(sys_sem_t *sem, u8_t count)
{
    *sem = xSemaphoreCreateCounting(0xFFFF, (UBaseType_t)count);
    return (*sem == NULL) ? ERR_MEM : ERR_OK;
}

void sys_sem_free(sys_sem_t *sem)
{
    vSemaphoreDelete(*sem);
}

void sys_sem_signal(sys_sem_t *sem)
{
    (void)xSemaphoreGive(*sem);
}

err_t sys_sem_trywait(sys_sem_t *sem)
{
    return xSemaphoreTake(*sem, 0) == pdTRUE ? ERR_OK : SYS_ARCH_TIMEOUT;
}

/* 带超时等待: 返回等待毫秒数, 超时 SYS_ARCH_TIMEOUT (netconn/sockets 用) */
u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
    TickType_t ticks = (timeout == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout);
    TickType_t t0 = xTaskGetTickCount();

    if (xSemaphoreTake(*sem, ticks) == pdTRUE) {
        return (u32_t)pdTICKS_TO_MS(xTaskGetTickCount() - t0);
    }
    return SYS_ARCH_TIMEOUT;
}

int sys_sem_valid(sys_sem_t *sem)
{
    return (*sem != NULL) ? 1 : 0;
}

void sys_sem_set_invalid(sys_sem_t *sem)
{
    *sem = NULL;
}

/* ---- mutex ---- */

err_t sys_mutex_new(sys_mutex_t *mutex)
{
    *mutex = xSemaphoreCreateMutex();
    return (*mutex == NULL) ? ERR_MEM : ERR_OK;
}

void sys_mutex_free(sys_mutex_t *mutex)
{
    vSemaphoreDelete(*mutex);
}

void sys_mutex_lock(sys_mutex_t *mutex)
{
    (void)xSemaphoreTake(*mutex, portMAX_DELAY);
}

void sys_mutex_unlock(sys_mutex_t *mutex)
{
    (void)xSemaphoreGive(*mutex);
}

/* ---- protect (semaphore-based critical section for netif) ---- */

sys_prot_t sys_arch_protect(void)
{
    taskENTER_CRITICAL();
    return 0;
}

void sys_arch_unprotect(sys_prot_t val)
{
    (void)val;
    taskEXIT_CRITICAL();
}

/* ---- interrupts from ISR ---- */

void sys_yield(void)
{
    taskYIELD();
}

/* ---- arch random (optional, used by TCP ISN) ---- */

u32_t sys_arch_random(void)
{
    static uint32_t seed = 0xDEADBEEF;
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

/* ---- thread create (for tcpip_init's tcpip thread) ---- */

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread,
                            void *arg, int stacksize, int prio)
{
    (void)name;
    StackType_t *stk = (StackType_t *)pvPortMalloc((size_t)stacksize * sizeof(StackType_t));
    StaticTask_t *tcb = (StaticTask_t *)pvPortMalloc(sizeof(StaticTask_t));
    if (stk == NULL || tcb == NULL) {
        return NULL;
    }
    return xTaskCreateStatic((TaskFunction_t)thread, name, (uint32_t)stacksize,
                             arg, (UBaseType_t)prio, stk, tcb);
}

/* ---- sys_now: system time in milliseconds ---- */

u32_t sys_now(void)
{
    return (u32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}
