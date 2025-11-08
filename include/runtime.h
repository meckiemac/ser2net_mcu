/*
 * ser2net MCU - Embedded RFC2217 runtime
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

/**
 * @file runtime.h
 * @brief FreeRTOS based runtime glue for the ser2net MCU library.
 *
 * This layer replaces the legacy `select()` driven main loop with a task based
 * execution model that is suitable for MCUs such as the ESP32 or STM32 when
 * running FreeRTOS.  It configures the listener task, session workers, job
 * queues, and timer integration while remaining agnostic to the actual
 * RFC2217 protocol handlers implemented by the session layer.
 */

#ifndef SER2NET_FREERTOS_RUNTIME_H
#define SER2NET_FREERTOS_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>

#include "ser2net_opts.h"
#include "ser2net_os.h"

#include "control_port.h"
#include "ser2net_persist.h"
#include "ser2net_if.h"

#ifndef SER2NET_MAX_PORTS
#define SER2NET_MAX_PORTS 8
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum ser2net_port_mode;

struct ser2net_session_ops;

struct ser2net_runtime_listener_cfg {
    int port_id;
    uint16_t tcp_port;
    const struct ser2net_network_if *network;
};

/**
 * @brief Complete runtime configuration passed to ::ser2net_runtime_start().
 */
struct ser2net_runtime_config {
    const char *listener_task_name;
    UBaseType_t listener_task_priority;
    uint16_t listener_task_stack_words;

    const char *session_task_name;
    UBaseType_t session_task_priority;
    uint16_t session_task_stack_words;

    size_t max_sessions;
    ser2net_tick_t accept_poll_ticks;
    ser2net_tick_t session_block_ticks;

    const struct ser2net_network_if *network;
    const struct ser2net_serial_if *serial;
    const struct ser2net_session_ops *session_ops;
    size_t listener_count;
    struct ser2net_runtime_listener_cfg listeners[SER2NET_MAX_PORTS];
    bool control_enabled;
    struct ser2net_control_context control_ctx;
    const struct ser2net_persist_ops *persist_ops;
    void (*config_changed_cb)(void *user_ctx);
    void *config_changed_ctx;
};

/** @brief Start the FreeRTOS runtime. */
BaseType_t ser2net_runtime_start(const struct ser2net_runtime_config *cfg);
/** @brief Stop the runtime and release resources. */
void ser2net_runtime_stop(void);
/** @brief Enumerate currently active sessions. */
size_t ser2net_runtime_list_sessions(struct ser2net_active_session *out, size_t max_entries);
/** @brief Disconnect the next active session associated with a TCP port. */
bool ser2net_runtime_disconnect_tcp_port(uint16_t tcp_port);
/**
 * @brief Update default serial parameters and optionally live sessions.
 *
 * @param tcp_port Listener TCP port.
 * @param params New default parameters.
 * @param idle_timeout_ms New idle timeout (ms).
 * @param apply_active Apply settings to active sessions.
 * @param pins Optional pin reconfiguration.
 */
BaseType_t ser2net_runtime_update_serial_config(uint16_t tcp_port,
                                               const struct ser2net_serial_params *params,
                                               uint32_t idle_timeout_ms,
                                               bool apply_active,
                                               const struct ser2net_pin_config *pins);
/** @brief Change mode (raw/rawlp/telnet) and enabled state of a listener. */
BaseType_t ser2net_runtime_set_port_mode(uint16_t tcp_port,
                                         enum ser2net_port_mode mode,
                                         bool enable);
/** @brief Register a new listener + UART mapping at runtime. */
BaseType_t ser2net_runtime_add_port(const struct ser2net_esp32_serial_port_cfg *cfg);
/** @brief Remove a dynamically added listener + UART mapping. */
BaseType_t ser2net_runtime_remove_port(uint16_t tcp_port);
/** @brief Copy the current port table into the supplied buffer. */
size_t ser2net_runtime_copy_ports(struct ser2net_esp32_serial_port_cfg *out, size_t max_ports);

#ifdef __cplusplus
}
#endif

#endif /* SER2NET_FREERTOS_RUNTIME_H */
