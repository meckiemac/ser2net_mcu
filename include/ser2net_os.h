/*
 * ser2net MCU - OS abstraction shim
 *
 * Copyright (C) 2025  Andreas Merk
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef SER2NET_OS_H
#define SER2NET_OS_H

#include <stddef.h>
#include <stdint.h>

#ifndef SER2NET_OS_FREERTOS
#define SER2NET_OS_FREERTOS 1
#endif

#if SER2NET_OS_FREERTOS

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

typedef TaskHandle_t ser2net_task_handle_t;
typedef QueueHandle_t ser2net_queue_t;
typedef SemaphoreHandle_t ser2net_mutex_t;
typedef TickType_t ser2net_tick_t;
typedef BaseType_t ser2net_os_status_t;

static inline void *ser2net_os_malloc(size_t bytes)
{
    return pvPortMalloc(bytes);
}

static inline void ser2net_os_free(void *ptr)
{
    if (ptr)
        vPortFree(ptr);
}

static inline ser2net_mutex_t
ser2net_os_mutex_create(void)
{
    return xSemaphoreCreateMutex();
}

static inline void
ser2net_os_mutex_delete(ser2net_mutex_t mutex)
{
    if (mutex)
        vSemaphoreDelete(mutex);
}

static inline ser2net_os_status_t
ser2net_os_mutex_lock(ser2net_mutex_t mutex, ser2net_tick_t ticks)
{
    return xSemaphoreTake(mutex, ticks);
}

static inline void
ser2net_os_mutex_unlock(ser2net_mutex_t mutex)
{
    xSemaphoreGive(mutex);
}

static inline ser2net_queue_t
ser2net_os_queue_create(UBaseType_t length, UBaseType_t item_size)
{
    return xQueueCreate(length, item_size);
}

static inline void
ser2net_os_queue_delete(ser2net_queue_t queue)
{
    if (queue)
        vQueueDelete(queue);
}

static inline ser2net_os_status_t
ser2net_os_queue_receive(ser2net_queue_t queue, void *item, ser2net_tick_t ticks)
{
    return xQueueReceive(queue, item, ticks);
}

static inline ser2net_os_status_t
ser2net_os_queue_send(ser2net_queue_t queue, const void *item, ser2net_tick_t ticks)
{
    return xQueueSend(queue, item, ticks);
}

static inline void
ser2net_os_queue_reset(ser2net_queue_t queue)
{
    xQueueReset(queue);
}

static inline ser2net_os_status_t
ser2net_os_task_create(TaskFunction_t fn,
                       const char *name,
                       uint16_t stack_words,
                       void *param,
                       UBaseType_t priority,
                       ser2net_task_handle_t *out_handle)
{
    return xTaskCreate(fn, name, stack_words, param, priority, out_handle);
}

static inline void
ser2net_os_task_delete(ser2net_task_handle_t handle)
{
    vTaskDelete(handle);
}

static inline ser2net_tick_t
ser2net_os_ticks_now(void)
{
    return xTaskGetTickCount();
}

static inline void
ser2net_os_task_delay_ticks(ser2net_tick_t ticks)
{
    vTaskDelay(ticks);
}

static inline void
ser2net_os_task_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

#if defined(portMUX_TYPE)
typedef portMUX_TYPE ser2net_spinlock_t;
#define SER2NET_SPINLOCK_INITIALIZER portMUX_INITIALIZER_UNLOCKED
static inline void ser2net_os_spinlock_enter(ser2net_spinlock_t *lock)
{
    taskENTER_CRITICAL(lock);
}
static inline void ser2net_os_spinlock_exit(ser2net_spinlock_t *lock)
{
    taskEXIT_CRITICAL(lock);
}
#else
typedef uint32_t ser2net_spinlock_t;
#define SER2NET_SPINLOCK_INITIALIZER 0
static inline void ser2net_os_spinlock_enter(ser2net_spinlock_t *lock)
{
    taskENTER_CRITICAL((void *) lock);
}
static inline void ser2net_os_spinlock_exit(ser2net_spinlock_t *lock)
{
    taskEXIT_CRITICAL((void *) lock);
}
#endif

#define SER2NET_OS_MS_TO_TICKS(ms) pdMS_TO_TICKS(ms)
#define SER2NET_OS_WAIT_FOREVER    portMAX_DELAY
#define SER2NET_OS_STATUS_OK       pdPASS
#define SER2NET_OS_STATUS_FAIL     pdFAIL

#else
#error "No OS shim implementation selected"
#endif /* SER2NET_OS_FREERTOS */

#endif /* SER2NET_OS_H */
