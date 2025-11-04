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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"

#include "control_port.h"

#ifndef SER2NET_MAX_PORTS
#define SER2NET_MAX_PORTS 8
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Represents an opaque handle for a network listener (server socket). */
typedef void *ser2net_listener_handle_t;

/* Represents an opaque handle for an accepted client connection. */
typedef void *ser2net_client_handle_t;

/* Represents an opaque handle for a serial device. */
typedef void *ser2net_serial_handle_t;

/**
 * @brief Call-backs required for a network backend.
 */
struct ser2net_network_if {
    void *ctx;
    /** Create and start a listener. */
    BaseType_t (*open_listener)(void *ctx, ser2net_listener_handle_t *out_listener);
    /** Accept a pending client connection. */
    BaseType_t (*accept_client)(void *ctx, ser2net_listener_handle_t listener,
                                ser2net_client_handle_t *out_client, TickType_t timeout_ticks);
    /** Close a client connection immediately. */
    void (*close_client)(void *ctx, ser2net_client_handle_t client);
    /** Receive data from a client. */
    int (*client_recv)(void *ctx, ser2net_client_handle_t client,
                       void *buf, size_t len, TickType_t timeout_ticks);
    /** Send data to a client. */
    int (*client_send)(void *ctx, ser2net_client_handle_t client,
                       const void *buf, size_t len, TickType_t timeout_ticks);
    /** Shutdown the client connection gracefully. */
    void (*client_shutdown)(void *ctx, ser2net_client_handle_t client);
};

/**
 * @brief Call-backs required for a serial backend.
 */
struct ser2net_serial_if {
    void *ctx;
    BaseType_t (*open_serial)(void *ctx, int port_id, ser2net_serial_handle_t *out_serial);
    void (*close_serial)(void *ctx, ser2net_serial_handle_t serial);
    int (*serial_read)(void *ctx, ser2net_serial_handle_t serial,
                       void *buf, size_t len, TickType_t timeout_ticks);
    int (*serial_write)(void *ctx, ser2net_serial_handle_t serial,
                        const void *buf, size_t len);
    BaseType_t (*serial_configure)(void *ctx, ser2net_serial_handle_t serial,
                                   int baud, int data_bits, int parity,
                                   int stop_bits, int flow_control);
};

struct ser2net_session_ops {
    void *ctx;
    BaseType_t (*initialise)(void *ctx, ser2net_client_handle_t client,
                             ser2net_serial_handle_t serial);
    BaseType_t (*process_io)(void *ctx, ser2net_client_handle_t client,
                             ser2net_serial_handle_t serial, TickType_t block_ticks);
    void (*handle_disconnect)(void *ctx, ser2net_client_handle_t client,
                              ser2net_serial_handle_t serial);
};

enum ser2net_port_mode;

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
    TickType_t accept_poll_ticks;
    TickType_t session_block_ticks;

    const struct ser2net_network_if *network;
    const struct ser2net_serial_if *serial;
    const struct ser2net_session_ops *session_ops;
    size_t listener_count;
    struct ser2net_runtime_listener_cfg listeners[SER2NET_MAX_PORTS];
    bool control_enabled;
    struct ser2net_control_context control_ctx;
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
