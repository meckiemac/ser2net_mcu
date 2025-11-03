/*
 * ser2net MCU - Embedded RFC2217 runtime
 *
 * Copyright (C) 2024  Your Name / Your Organisation
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

#include "runtime.h"
#include "session_ops.h"
#include "adapters.h"
#include "esp_log.h"

#include <string.h>
#include <limits.h>

#include "freertos/semphr.h"
#include "driver/uart.h"
#include <sys/types.h>

/*
 * MCU runtime scheduler
 * ---------------------
 * Each accepted TCP (RFC2217) connection is wrapped in a session_job and
 * processed by a dedicated FreeRTOS task.  The listener task round-robins
 * across all configured network listeners (one per UART).  This file also
 * takes care of starting / stopping the optional control port.
 */

struct ser2net_session_ctx;

struct listener_entry {
    const struct ser2net_network_if *net;
    ser2net_listener_handle_t handle;
    int port_id;
    uint16_t tcp_port;
    enum ser2net_port_mode mode;
    bool enabled;
};

struct active_session_entry {
    bool in_use;
    uint16_t tcp_port;
    int port_id;
    const struct ser2net_network_if *network;
    ser2net_client_handle_t client;
    ser2net_serial_handle_t serial;
    void *session_ctx;
};

struct ser2net_runtime_state {
    struct ser2net_runtime_config cfg;
    QueueHandle_t job_queue;
    TaskHandle_t listener_task;
    struct listener_entry listeners[SER2NET_MAX_PORTS];
    size_t listener_count;
    struct ser2net_esp32_serial_port_cfg port_table[SER2NET_MAX_PORTS];
    size_t port_count;
    bool control_started;
    struct active_session_entry *sessions;
    size_t session_capacity;
    SemaphoreHandle_t active_lock;
    SemaphoreHandle_t port_lock;
};

static struct ser2net_runtime_state runtime_state;

static int
runtime_allocate_port_id(void)
{
    for (int candidate = 0; candidate < 256; ++candidate) {
        bool used = false;
        for (size_t i = 0; i < runtime_state.port_count; ++i) {
            if (runtime_state.port_table[i].port_id == candidate) {
                used = true;
                break;
            }
        }
        if (!used)
            return candidate;
    }
    return -1;
}

static void
runtime_notify_config_changed(void)
{
    if (runtime_state.cfg.config_changed_cb)
        runtime_state.cfg.config_changed_cb(runtime_state.cfg.config_changed_ctx);
}

struct session_job {
    int port_id;
    ser2net_client_handle_t client;
    const struct ser2net_network_if *network;
    uint16_t tcp_port;
};

static void
active_sessions_clear(void)
{
    if (!runtime_state.sessions)
        return;
    for (size_t i = 0; i < runtime_state.session_capacity; ++i)
        runtime_state.sessions[i].in_use = false;
}

static bool
active_sessions_register(uint16_t tcp_port,
                         int port_id,
                         const struct ser2net_network_if *network,
                         ser2net_client_handle_t client,
                         ser2net_serial_handle_t serial,
                         void *session_ctx)
{
    if (!runtime_state.sessions || !runtime_state.active_lock)
        return false;

    if (xSemaphoreTake(runtime_state.active_lock, portMAX_DELAY) != pdPASS)
        return false;

    bool stored = false;
    for (size_t i = 0; i < runtime_state.session_capacity; ++i) {
        if (runtime_state.sessions[i].in_use)
            continue;
        runtime_state.sessions[i].in_use = true;
        runtime_state.sessions[i].tcp_port = tcp_port;
        runtime_state.sessions[i].port_id = port_id;
        runtime_state.sessions[i].network = network;
        runtime_state.sessions[i].client = client;
        runtime_state.sessions[i].serial = serial;
        runtime_state.sessions[i].session_ctx = session_ctx;
        stored = true;
        break;
    }

    xSemaphoreGive(runtime_state.active_lock);
    return stored;
}

static void
active_sessions_unregister(uint16_t tcp_port, ser2net_client_handle_t client)
{
    if (!runtime_state.sessions || !runtime_state.active_lock)
        return;

    if (xSemaphoreTake(runtime_state.active_lock, portMAX_DELAY) != pdPASS)
        return;

    for (size_t i = 0; i < runtime_state.session_capacity; ++i) {
        if (!runtime_state.sessions[i].in_use)
            continue;
        if (runtime_state.sessions[i].tcp_port == tcp_port &&
            runtime_state.sessions[i].client == client) {
            runtime_state.sessions[i].in_use = false;
            runtime_state.sessions[i].serial = NULL;
            runtime_state.sessions[i].session_ctx = NULL;
            break;
        }
    }

    xSemaphoreGive(runtime_state.active_lock);
}

struct session_init_param {
    struct ser2net_session_ctx **ctx_ptr;
    const struct ser2net_network_if *network;
    int port_id;
};

/**
 * session_task()
 *
 * Worker loop executed by every session task.  Waits for jobs on the queue,
 * opens the requested UART, initialises RFC2217 handling and pumps data until
 * either side disconnects.
 */
static void session_task(void *param)
{
    struct ser2net_runtime_state *state = (struct ser2net_runtime_state *) param;
    const struct ser2net_runtime_config *cfg = &state->cfg;
    struct session_job job;

    for (;;) {
        if (xQueueReceive(state->job_queue, &job, portMAX_DELAY) != pdPASS)
            continue;

        ser2net_serial_handle_t serial = NULL;
        struct ser2net_session_ctx *session_ctx = NULL;

        if (cfg->serial->open_serial(cfg->serial->ctx, job.port_id, &serial) != pdPASS) {
            job.network->close_client(job.network->ctx, job.client);
            continue;
        }

        struct session_init_param init_param = {
            .ctx_ptr = &session_ctx,
            .network = job.network,
            .port_id = job.port_id
        };

        if (cfg->session_ops->initialise(&init_param, job.client, serial) != pdPASS) {
            cfg->session_ops->handle_disconnect(&session_ctx, job.client, serial);
            cfg->serial->close_serial(cfg->serial->ctx, serial);
            job.network->close_client(job.network->ctx, job.client);
            continue;
        }

        active_sessions_register(job.tcp_port, job.port_id, job.network, job.client, serial, session_ctx);

        for (;;) {
            if (cfg->session_ops->process_io(&session_ctx, job.client,
                                             serial, cfg->session_block_ticks) != pdPASS) {
                break;
            }
        }

        active_sessions_unregister(job.tcp_port, job.client);

        cfg->session_ops->handle_disconnect(&session_ctx, job.client, serial);
        cfg->serial->close_serial(cfg->serial->ctx, serial);
        job.network->close_client(job.network->ctx, job.client);
    }
}

/**
 * listener_task()
 *
 * Iterates over all configured TCP listeners.  Whenever a listener has a
 * pending connection, enqueue a job for the worker pool.  The short retry
 * delay avoids busy-spinning when the queue is full.
 */
static void listener_task(void *param)
{
    struct ser2net_runtime_state *state = (struct ser2net_runtime_state *) param;
    const struct ser2net_runtime_config *cfg = &state->cfg;
    struct session_job job;

    memset(&job, 0, sizeof(job));

    for (;;) {
        for (size_t i = 0; i < state->listener_count; ++i) {
            struct listener_entry *entry = &state->listeners[i];
            if (!entry->enabled)
                continue;
            BaseType_t rv = entry->net->accept_client(entry->net->ctx,
                                                      entry->handle,
                                                      &job.client,
                                                      cfg->accept_poll_ticks);
            if (rv != pdPASS)
                continue;

            job.port_id = entry->port_id;
            job.network = entry->net;
            job.tcp_port = entry->tcp_port;

            if (xQueueSend(state->job_queue, &job, 0) != pdPASS) {
                entry->net->close_client(entry->net->ctx, job.client);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }
}

/**
 * ser2net_runtime_start()
 *
 * Entrypoint used by the application to bootstrap networking and session
 * workers.  Creates one network listener per UART, allocates the worker pool
 * and optionally starts the control port.
 */
BaseType_t
ser2net_runtime_start(const struct ser2net_runtime_config *cfg)
{
    memset(&runtime_state, 0, sizeof(runtime_state));
    runtime_state.cfg = *cfg;

    runtime_state.port_count = 0;
    if (cfg->control_ctx.ports && cfg->control_ctx.port_count > 0) {
        size_t limit = cfg->control_ctx.port_count;
        if (limit > SER2NET_MAX_PORTS)
            limit = SER2NET_MAX_PORTS;
        for (size_t i = 0; i < limit; ++i)
            runtime_state.port_table[i] = cfg->control_ctx.ports[i];
        runtime_state.port_count = limit;
    }
    runtime_state.cfg.control_ctx.ports = runtime_state.port_table;
    runtime_state.cfg.control_ctx.port_count = runtime_state.port_count;
    runtime_state.cfg.control_ctx.add_port_cb = ser2net_runtime_add_port;

    ESP_LOGI("ser2net_runtime", "Starting runtime with %zu listener(s)",
             (size_t)cfg->listener_count);

    runtime_state.sessions = pvPortMalloc(cfg->max_sessions * sizeof(struct active_session_entry));
    if (!runtime_state.sessions)
        return pdFAIL;
    runtime_state.session_capacity = cfg->max_sessions;
    runtime_state.active_lock = xSemaphoreCreateMutex();
    if (!runtime_state.active_lock) {
        vPortFree(runtime_state.sessions);
        runtime_state.sessions = NULL;
        runtime_state.session_capacity = 0;
        return pdFAIL;
    }
    active_sessions_clear();

    runtime_state.port_lock = xSemaphoreCreateMutex();
    if (!runtime_state.port_lock) {
        vSemaphoreDelete(runtime_state.active_lock);
        runtime_state.active_lock = NULL;
        vPortFree(runtime_state.sessions);
        runtime_state.sessions = NULL;
        runtime_state.session_capacity = 0;
        return pdFAIL;
    }

    runtime_state.listener_count = 0;
    for (size_t i = 0; i < cfg->listener_count; ++i) {
        const struct ser2net_network_if *net = cfg->listeners[i].network;
        if (!net)
            return pdFAIL;
        struct listener_entry *entry = &runtime_state.listeners[i];
        entry->net = net;
        entry->port_id = cfg->listeners[i].port_id;
        entry->tcp_port = cfg->listeners[i].tcp_port;
        entry->enabled = true;
        entry->mode = SER2NET_PORT_MODE_TELNET;
        if (net->open_listener(net->ctx, &entry->handle) != pdPASS)
            return pdFAIL;
        if (runtime_state.cfg.control_ctx.ports) {
            for (size_t j = 0; j < runtime_state.cfg.control_ctx.port_count; ++j) {
                const struct ser2net_esp32_serial_port_cfg *p = &runtime_state.cfg.control_ctx.ports[j];
                if (p->tcp_port == entry->tcp_port) {
                    entry->mode = p->mode;
                    entry->enabled = p->enabled;
                    break;
                }
            }
        }
        ESP_LOGI("ser2net_runtime", "Listener ready: tcp=%u -> serial port_id=%d (enabled=%d)",
                 entry->tcp_port, entry->port_id, entry->enabled);
        runtime_state.listener_count++;
    }

    runtime_state.cfg.control_ctx.disconnect_cb = ser2net_runtime_disconnect_tcp_port;
    runtime_state.cfg.control_ctx.list_sessions_cb = ser2net_runtime_list_sessions;
    runtime_state.cfg.control_ctx.set_serial_config_cb = ser2net_runtime_update_serial_config;
    runtime_state.cfg.control_ctx.set_port_mode_cb = ser2net_runtime_set_port_mode;

    if (runtime_state.cfg.control_enabled && runtime_state.cfg.control_ctx.tcp_port > 0) {
        if (ser2net_control_start(&runtime_state.cfg.control_ctx)) {
            runtime_state.control_started = true;
            ESP_LOGI("ser2net_runtime", "Control port listening on tcp=%u", runtime_state.cfg.control_ctx.tcp_port);
        } else {
            ESP_LOGW("ser2net_runtime", "Failed to start control port on tcp=%u", runtime_state.cfg.control_ctx.tcp_port);
        }
    } else {
        runtime_state.control_started = false;
    }

    runtime_state.job_queue = xQueueCreate(cfg->max_sessions, sizeof(struct session_job));
    if (!runtime_state.job_queue) {
        vSemaphoreDelete(runtime_state.active_lock);
        runtime_state.active_lock = NULL;
        vPortFree(runtime_state.sessions);
        runtime_state.sessions = NULL;
        runtime_state.session_capacity = 0;
        return pdFAIL;
    }

    if (xTaskCreate(listener_task,
                    cfg->listener_task_name ? cfg->listener_task_name : "ser2net_listen",
                    cfg->listener_task_stack_words,
                    &runtime_state,
                    cfg->listener_task_priority,
                    &runtime_state.listener_task) != pdPASS) {
        return pdFAIL;
    }

    for (size_t i = 0; i < cfg->max_sessions; ++i) {
        TaskHandle_t task_handle = NULL;
        if (xTaskCreate(session_task,
                        cfg->session_task_name ? cfg->session_task_name : "ser2net_sess",
                        cfg->session_task_stack_words,
                        &runtime_state,
                        cfg->session_task_priority,
                        &task_handle) != pdPASS) {
            return pdFAIL;
        }
    }

    runtime_notify_config_changed();
    runtime_notify_config_changed();
    runtime_notify_config_changed();
    return pdPASS;
}

/**
 * ser2net_runtime_stop()
 *
 * Reverse the setup performed in ser2net_runtime_start(): stop the control
 * port, shut down listener / worker tasks and release allocated listeners.
 */
void
ser2net_runtime_stop(void)
{
    if (runtime_state.control_started) {
        ser2net_control_stop();
        runtime_state.control_started = false;
    }

    if (runtime_state.listener_task) {
        vTaskDelete(runtime_state.listener_task);
        runtime_state.listener_task = NULL;
    }

    if (runtime_state.job_queue) {
        vQueueDelete(runtime_state.job_queue);
        runtime_state.job_queue = NULL;
    }

    if (runtime_state.active_lock) {
        vSemaphoreDelete(runtime_state.active_lock);
        runtime_state.active_lock = NULL;
    }
    if (runtime_state.sessions) {
        vPortFree(runtime_state.sessions);
        runtime_state.sessions = NULL;
        runtime_state.session_capacity = 0;
    }

    if (runtime_state.port_lock) {
        vSemaphoreDelete(runtime_state.port_lock);
        runtime_state.port_lock = NULL;
    }

    runtime_state.cfg.control_ctx.add_port_cb = NULL;
    runtime_state.cfg.control_ctx.ports = NULL;
    runtime_state.cfg.control_ctx.port_count = 0;

    for (size_t i = 0; i < runtime_state.listener_count; ++i) {
        if (runtime_state.listeners[i].net) {
            ser2net_esp32_release_network_if(runtime_state.listeners[i].net);
            runtime_state.listeners[i].net = NULL;
            runtime_state.listeners[i].handle = NULL;
        }
    }
    runtime_state.listener_count = 0;
}

size_t
ser2net_runtime_list_sessions(struct ser2net_active_session *out, size_t max_entries)
{
    size_t reported = 0;

    if (!runtime_state.sessions)
        return 0;

    if (runtime_state.active_lock && xSemaphoreTake(runtime_state.active_lock, portMAX_DELAY) != pdPASS)
        return 0;

    for (size_t i = 0; i < runtime_state.session_capacity; ++i) {
        if (!runtime_state.sessions[i].in_use)
            continue;
        if (out && reported < max_entries) {
            out[reported].tcp_port = runtime_state.sessions[i].tcp_port;
            out[reported].port_id = runtime_state.sessions[i].port_id;
        }
        reported++;
    }

    if (runtime_state.active_lock)
        xSemaphoreGive(runtime_state.active_lock);

    return reported;
}

bool
ser2net_runtime_disconnect_tcp_port(uint16_t tcp_port)
{
    if (!runtime_state.sessions)
        return false;

    const struct ser2net_network_if *network = NULL;
    ser2net_client_handle_t client = NULL;

    if (runtime_state.active_lock && xSemaphoreTake(runtime_state.active_lock, portMAX_DELAY) == pdPASS) {
        for (size_t i = 0; i < runtime_state.session_capacity; ++i) {
            if (!runtime_state.sessions[i].in_use)
                continue;
            if (runtime_state.sessions[i].tcp_port == tcp_port) {
                network = runtime_state.sessions[i].network;
                client = runtime_state.sessions[i].client;
                break;
            }
        }
        xSemaphoreGive(runtime_state.active_lock);
    }

    if (!network || !client || !network->client_shutdown)
        return false;

    network->client_shutdown(network->ctx, client);
    return true;
}

BaseType_t
ser2net_runtime_update_serial_config(uint16_t tcp_port,
                                     const struct ser2net_serial_params *params,
                                     uint32_t idle_timeout_ms,
                                     bool apply_active,
                                     const struct ser2net_pin_config *pins)
{
    if (!params)
        return pdFAIL;

    int port_id = -1;
    for (size_t i = 0; i < runtime_state.listener_count; ++i) {
        if (runtime_state.listeners[i].tcp_port == tcp_port) {
            port_id = runtime_state.listeners[i].port_id;
            break;
        }
    }

    if (port_id < 0)
        return pdFAIL;

    if (ser2net_session_update_defaults(port_id, params, idle_timeout_ms) != pdPASS)
        return pdFAIL;

    uart_port_t uart_num = UART_NUM_MAX;
    int new_tx = UART_PIN_NO_CHANGE;
    int new_rx = UART_PIN_NO_CHANGE;
    int new_rts = UART_PIN_NO_CHANGE;
    int new_cts = UART_PIN_NO_CHANGE;
    bool update_uart = pins && pins->uart_num >= 0;
    bool update_pins = pins && (update_uart ||
                                pins->tx_pin >= 0 || pins->tx_pin == INT_MIN ||
                                pins->rx_pin >= 0 || pins->rx_pin == INT_MIN ||
                                pins->rts_pin >= 0 || pins->rts_pin == INT_MIN ||
                                pins->cts_pin >= 0 || pins->cts_pin == INT_MIN);

    if (runtime_state.cfg.control_ctx.ports) {
        for (size_t i = 0; i < runtime_state.cfg.control_ctx.port_count; ++i) {
            struct ser2net_esp32_serial_port_cfg *p = (struct ser2net_esp32_serial_port_cfg *) &runtime_state.cfg.control_ctx.ports[i];
            if (p->tcp_port != tcp_port)
                continue;
            p->baud_rate = params->baud;
            switch (params->data_bits) {
            case 5: p->data_bits = UART_DATA_5_BITS; break;
            case 6: p->data_bits = UART_DATA_6_BITS; break;
            case 7: p->data_bits = UART_DATA_7_BITS; break;
            default: p->data_bits = UART_DATA_8_BITS; break;
            }
            p->parity = params->parity == 1 ? UART_PARITY_ODD :
                        (params->parity == 2 ? UART_PARITY_EVEN : UART_PARITY_DISABLE);
            switch (params->stop_bits) {
            case 2: p->stop_bits = UART_STOP_BITS_2; break;
#ifdef UART_STOP_BITS_1_5
            case 15: p->stop_bits = UART_STOP_BITS_1_5; break;
#endif
            default: p->stop_bits = UART_STOP_BITS_1; break;
            }
            p->flow_ctrl = params->flow_control ? UART_HW_FLOWCTRL_CTS_RTS : UART_HW_FLOWCTRL_DISABLE;
            p->idle_timeout_ms = idle_timeout_ms;
            if (update_pins) {
                if (update_uart)
                    p->uart_num = pins->uart_num;
                if (pins->tx_pin >= 0 || pins->tx_pin == INT_MIN)
                    p->tx_pin = (pins->tx_pin == INT_MIN) ? UART_PIN_NO_CHANGE : pins->tx_pin;
                if (pins->rx_pin >= 0 || pins->rx_pin == INT_MIN)
                    p->rx_pin = (pins->rx_pin == INT_MIN) ? UART_PIN_NO_CHANGE : pins->rx_pin;
                if (pins->rts_pin >= 0 || pins->rts_pin == INT_MIN)
                    p->rts_pin = (pins->rts_pin == INT_MIN) ? UART_PIN_NO_CHANGE : pins->rts_pin;
                if (pins->cts_pin >= 0 || pins->cts_pin == INT_MIN)
                    p->cts_pin = (pins->cts_pin == INT_MIN) ? UART_PIN_NO_CHANGE : pins->cts_pin;
                uart_num = p->uart_num;
                new_tx = p->tx_pin >= 0 ? p->tx_pin : UART_PIN_NO_CHANGE;
                new_rx = p->rx_pin >= 0 ? p->rx_pin : UART_PIN_NO_CHANGE;
                new_rts = p->rts_pin >= 0 ? p->rts_pin : UART_PIN_NO_CHANGE;
                new_cts = p->cts_pin >= 0 ? p->cts_pin : UART_PIN_NO_CHANGE;
            }
        }
    }

    if (update_pins && uart_num != UART_NUM_MAX)
        uart_set_pin(uart_num, new_tx, new_rx, new_rts, new_cts);

    if (apply_active && runtime_state.sessions && runtime_state.active_lock) {
        struct {
            void *session_ctx;
            ser2net_serial_handle_t serial;
        } matches[SER2NET_MAX_PORTS];
        size_t match_count = 0;

        if (xSemaphoreTake(runtime_state.active_lock, portMAX_DELAY) == pdPASS) {
            for (size_t i = 0; i < runtime_state.session_capacity; ++i) {
                if (!runtime_state.sessions[i].in_use)
                    continue;
                if (runtime_state.sessions[i].tcp_port != tcp_port)
                    continue;
                if (match_count < SER2NET_MAX_PORTS) {
                    matches[match_count].session_ctx = runtime_state.sessions[i].session_ctx;
                    matches[match_count].serial = runtime_state.sessions[i].serial;
                    match_count++;
                }
            }
            xSemaphoreGive(runtime_state.active_lock);
        }

        for (size_t i = 0; i < match_count; ++i) {
            if (matches[i].session_ctx)
                ser2net_session_apply_config(matches[i].session_ctx, params);
        }
    }

    runtime_notify_config_changed();
    return pdPASS;
}

BaseType_t
ser2net_runtime_set_port_mode(uint16_t tcp_port,
                              enum ser2net_port_mode mode,
                              bool enable)
{
    int listener_index = -1;
    for (size_t i = 0; i < runtime_state.listener_count; ++i) {
        if (runtime_state.listeners[i].tcp_port == tcp_port) {
            listener_index = (int)i;
            break;
        }
    }

    if (listener_index < 0)
        return pdFAIL;

    struct listener_entry *entry = &runtime_state.listeners[listener_index];

    if (ser2net_session_set_mode(entry->port_id, mode) != pdPASS)
        return pdFAIL;

    entry->mode = mode;
    entry->enabled = enable;

    if (runtime_state.cfg.control_ctx.ports) {
        for (size_t i = 0; i < runtime_state.cfg.control_ctx.port_count; ++i) {
            struct ser2net_esp32_serial_port_cfg *p = (struct ser2net_esp32_serial_port_cfg *) &runtime_state.cfg.control_ctx.ports[i];
            if (p->tcp_port == tcp_port) {
                p->mode = mode;
                p->enabled = enable;
            }
        }
    }

    if (!enable) {
        bool disconnected = true;
        while (disconnected) {
            disconnected = ser2net_runtime_disconnect_tcp_port(tcp_port);
        }
    }

    runtime_notify_config_changed();
    return pdPASS;
}

size_t
ser2net_runtime_copy_ports(struct ser2net_esp32_serial_port_cfg *out, size_t max_ports)
{
    if (!out || max_ports == 0)
        return 0;

    size_t copied = 0;
    if (runtime_state.port_lock && xSemaphoreTake(runtime_state.port_lock, portMAX_DELAY) == pdPASS) {
        size_t count = runtime_state.port_count;
        if (count > max_ports)
            count = max_ports;
        memcpy(out, runtime_state.port_table, count * sizeof(struct ser2net_esp32_serial_port_cfg));
        copied = count;
        xSemaphoreGive(runtime_state.port_lock);
    }
    return copied;
}

BaseType_t
ser2net_runtime_add_port(const struct ser2net_esp32_serial_port_cfg *cfg)
{
    if (!cfg || !runtime_state.port_lock)
        return pdFAIL;

    if (xSemaphoreTake(runtime_state.port_lock, portMAX_DELAY) != pdPASS)
        return pdFAIL;

    BaseType_t result = pdFAIL;
    bool serial_added = false;
    bool session_added = false;
    const struct ser2net_network_if *net_if = NULL;
    ser2net_listener_handle_t handle = NULL;

    size_t idx = runtime_state.port_count;

    if (runtime_state.port_count >= SER2NET_MAX_PORTS ||
        runtime_state.listener_count >= SER2NET_MAX_PORTS)
        goto out;

    struct ser2net_esp32_serial_port_cfg local = *cfg;

    if (local.port_id < 0) {
        int next_id = runtime_allocate_port_id();
        if (next_id < 0)
            goto out;
        local.port_id = next_id;
    }

    for (size_t i = 0; i < runtime_state.port_count; ++i) {
        if (runtime_state.port_table[i].port_id == local.port_id ||
            runtime_state.port_table[i].tcp_port == local.tcp_port)
            goto out;
    }

    runtime_state.port_table[idx] = local;
    const struct ser2net_esp32_serial_port_cfg *stored = &runtime_state.port_table[idx];

    if (ser2net_esp32_register_port(stored) != pdPASS)
        goto out;
    serial_added = true;

    struct ser2net_serial_params defaults = {
        .baud = stored->baud_rate,
        .data_bits = 8,
        .parity = 0,
        .stop_bits = 1,
        .flow_control = 0
    };

    switch (stored->data_bits) {
    case UART_DATA_5_BITS: defaults.data_bits = 5; break;
    case UART_DATA_6_BITS: defaults.data_bits = 6; break;
    case UART_DATA_7_BITS: defaults.data_bits = 7; break;
    default: defaults.data_bits = 8; break;
    }

    if (stored->parity == UART_PARITY_ODD)
        defaults.parity = 1;
    else if (stored->parity == UART_PARITY_EVEN)
        defaults.parity = 2;
    else
        defaults.parity = 0;

    switch (stored->stop_bits) {
    case UART_STOP_BITS_2: defaults.stop_bits = 2; break;
#ifdef UART_STOP_BITS_1_5
    case UART_STOP_BITS_1_5: defaults.stop_bits = 15; break;
#endif
    default: defaults.stop_bits = 1; break;
    }

    defaults.flow_control = (stored->flow_ctrl == UART_HW_FLOWCTRL_CTS_RTS) ? 1 : 0;

    if (ser2net_session_register_port(stored->port_id,
                                      stored->tcp_port,
                                      stored->mode,
                                      &defaults,
                                      stored->idle_timeout_ms) != pdPASS)
        goto out;
    session_added = true;

    struct ser2net_esp32_network_cfg listener_cfg = {
        .listen_port = stored->tcp_port,
        .backlog = stored->tcp_backlog <= 0 ? 4 : stored->tcp_backlog
    };

    net_if = ser2net_esp32_get_network_if(&listener_cfg);
    if (!net_if)
        goto out;

    if (net_if->open_listener(net_if->ctx, &handle) != pdPASS) {
        ser2net_esp32_release_network_if(net_if);
        net_if = NULL;
        goto out;
    }

    struct listener_entry *entry = &runtime_state.listeners[runtime_state.listener_count];
    entry->net = net_if;
    entry->handle = handle;
    entry->port_id = stored->port_id;
    entry->tcp_port = stored->tcp_port;
    entry->mode = stored->mode;
    entry->enabled = stored->enabled;

    runtime_state.listener_count++;
    runtime_state.cfg.listener_count = runtime_state.listener_count;
    runtime_state.cfg.listeners[runtime_state.listener_count - 1].port_id = stored->port_id;
    runtime_state.cfg.listeners[runtime_state.listener_count - 1].tcp_port = stored->tcp_port;
    runtime_state.cfg.listeners[runtime_state.listener_count - 1].network = net_if;

    runtime_state.port_count++;
    runtime_state.cfg.control_ctx.port_count = runtime_state.port_count;
    runtime_state.cfg.control_ctx.ports = runtime_state.port_table;

    result = pdPASS;

out:
    if (result != pdPASS) {
        if (net_if && handle)
            ser2net_esp32_release_network_if(net_if);
        if (session_added)
            ser2net_session_unregister_port(cfg->port_id);
        if (serial_added)
            ser2net_esp32_unregister_port(cfg->port_id);
        memset(&runtime_state.port_table[idx], 0,
               sizeof(runtime_state.port_table[idx]));
    } else {
        ESP_LOGI("ser2net_runtime", "Dynamic port added: port_id=%d tcp=%u uart=%d",
                 stored->port_id, stored->tcp_port, stored->uart_num);
    }

    xSemaphoreGive(runtime_state.port_lock);

    if (result == pdPASS)
        runtime_notify_config_changed();

    return result;
}

BaseType_t
ser2net_runtime_remove_port(uint16_t tcp_port)
{
    if (!runtime_state.port_lock)
        return pdFAIL;

    if (xSemaphoreTake(runtime_state.port_lock, portMAX_DELAY) != pdPASS)
        return pdFAIL;

    BaseType_t result = pdFAIL;
    ssize_t port_index = -1;
    ssize_t listener_index = -1;

    for (size_t i = 0; i < runtime_state.port_count; ++i) {
        if (runtime_state.port_table[i].tcp_port == tcp_port) {
            port_index = (ssize_t) i;
            break;
        }
    }

    for (size_t i = 0; i < runtime_state.listener_count; ++i) {
        if (runtime_state.listeners[i].tcp_port == tcp_port) {
            listener_index = (ssize_t) i;
            break;
        }
    }

    if (port_index < 0 || listener_index < 0)
        goto out;

    int port_id = runtime_state.port_table[port_index].port_id;
    const struct ser2net_network_if *net_if = runtime_state.listeners[listener_index].net;

    bool disconnected = true;
    while (disconnected)
        disconnected = ser2net_runtime_disconnect_tcp_port(tcp_port);

    bool has_active = false;
    if (runtime_state.sessions && runtime_state.active_lock &&
        xSemaphoreTake(runtime_state.active_lock, portMAX_DELAY) == pdPASS) {
        for (size_t i = 0; i < runtime_state.session_capacity; ++i) {
            if (!runtime_state.sessions[i].in_use)
                continue;
            if (runtime_state.sessions[i].tcp_port == tcp_port) {
                has_active = true;
                break;
            }
        }
        xSemaphoreGive(runtime_state.active_lock);
    }

    if (has_active)
        goto out;

    if (ser2net_session_unregister_port(port_id) != pdPASS)
        goto out;

    ser2net_esp32_unregister_port(port_id);

    if (net_if) {
        runtime_state.listeners[listener_index].net = NULL;
        ser2net_esp32_release_network_if(net_if);
    }

    for (size_t i = (size_t) listener_index; i + 1 < runtime_state.listener_count; ++i)
        runtime_state.listeners[i] = runtime_state.listeners[i + 1];

    if (runtime_state.listener_count > 0) {
        size_t last = runtime_state.listener_count - 1;
        memset(&runtime_state.listeners[last], 0, sizeof(runtime_state.listeners[last]));
        runtime_state.listener_count = last;
    }

    for (size_t i = (size_t) port_index; i + 1 < runtime_state.port_count; ++i)
        runtime_state.port_table[i] = runtime_state.port_table[i + 1];

    if (runtime_state.port_count > 0) {
        size_t last = runtime_state.port_count - 1;
        memset(&runtime_state.port_table[last], 0, sizeof(runtime_state.port_table[last]));
        runtime_state.port_count = last;
    }

    runtime_state.cfg.listener_count = runtime_state.listener_count;
    for (size_t i = 0; i < runtime_state.listener_count; ++i) {
        runtime_state.cfg.listeners[i].port_id = runtime_state.listeners[i].port_id;
        runtime_state.cfg.listeners[i].tcp_port = runtime_state.listeners[i].tcp_port;
        runtime_state.cfg.listeners[i].network = runtime_state.listeners[i].net;
    }
    if (runtime_state.listener_count < SER2NET_MAX_PORTS) {
        memset(&runtime_state.cfg.listeners[runtime_state.listener_count], 0,
               sizeof(runtime_state.cfg.listeners[runtime_state.listener_count]));
    }

    runtime_state.cfg.control_ctx.port_count = runtime_state.port_count;
    runtime_state.cfg.control_ctx.ports = runtime_state.port_table;

    result = pdPASS;

out:
    xSemaphoreGive(runtime_state.port_lock);

    if (result == pdPASS)
        runtime_notify_config_changed();

    return result;
}
