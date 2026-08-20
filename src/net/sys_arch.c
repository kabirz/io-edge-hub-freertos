/*
 * LwIP sys_arch port for FreeRTOS (STM32F407 + W5500 MACRAW).
 * Implements the OS-dependent functions required by LwIP's NO_SYS=0 mode.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <lwip/sys.h>
#include <lwip/err.h>

/* ---- mbox (FreeRTOS queue-based) ---- */

err_t sys_mbox_new(sys_mbox_t *mb, int size)
{
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
    /* Simple PRNG based on SysTick; good enough for TCP ISN */
    static uint32_t seed = 0xDEADBEEF;
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}
