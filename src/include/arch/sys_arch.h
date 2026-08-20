/*
 * LwIP arch/sys_arch.h for FreeRTOS.
 * Defines OS-specific types used by LwIP's NO_SYS=0 mode.
 */
#ifndef LWIP_ARCH_SYS_ARCH_H
#define LWIP_ARCH_SYS_ARCH_H

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* mbox is a FreeRTOS queue of void* pointers */
typedef QueueHandle_t sys_mbox_t;

/* semaphore is a FreeRTOS counting semaphore */
typedef SemaphoreHandle_t sys_sem_t;

/* mutex is a FreeRTOS mutex */
typedef SemaphoreHandle_t sys_mutex_t;

/* protection type (unused, critical sections via taskENTER/EXIT_CRITICAL) */
typedef int sys_prot_t;

/* Thread handle (not used — we don't create LwIP threads ourselves) */
typedef void *sys_thread_t;

/* Mailbox empty return value (defined by LwIP sys.h) */

#endif /* LWIP_ARCH_SYS_ARCH_H */
