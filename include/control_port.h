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

#ifndef SER2NET_CONTROL_PORT_H
#define SER2NET_CONTROL_PORT_H
/**
 * @file control_port.h
 * @brief Telnet style management interface for the MCU runtime.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ser2net_opts.h"
#include "monitor_bus.h"
#include "ser2net_os.h"

struct ser2net_esp32_serial_port_cfg;
struct ser2net_serial_params;
struct ser2net_active_session;
enum ser2net_port_mode;
struct ser2net_pin_config;

/** @brief Minimal information about an active session. */
struct ser2net_active_session {
    uint16_t tcp_port;
    int port_id;
};

/**
 * @brief Configuration for the text based control port.
 *
 * All callbacks are optional; unavailable features will be hidden from the
 * command set automatically.
 */
struct ser2net_control_context {
    uint16_t tcp_port;
    int backlog;
    const struct ser2net_esp32_serial_port_cfg *ports;
    size_t port_count;
    const char *version;
    bool (*disconnect_cb)(uint16_t tcp_port);
    size_t (*list_sessions_cb)(struct ser2net_active_session *out, size_t max_entries);
    ser2net_os_status_t (*set_serial_config_cb)(uint16_t tcp_port,
                                                const struct ser2net_serial_params *params,
                                                uint32_t idle_timeout_ms,
                                                bool apply_active,
                                                const struct ser2net_pin_config *pins);
    ser2net_os_status_t (*set_port_mode_cb)(uint16_t tcp_port,
                                            enum ser2net_port_mode mode,
                                            bool enable);
    ser2net_os_status_t (*add_port_cb)(const struct ser2net_esp32_serial_port_cfg *cfg);
};

/** @brief Start the control port task. */
bool ser2net_control_start(const struct ser2net_control_context *ctx);
/** @brief Stop the control port task. */
void ser2net_control_stop(void);

static inline void ser2net_control_monitor_feed(uint16_t tcp_port,
                                                enum ser2net_monitor_stream stream,
                                                const uint8_t *data,
                                                size_t len)
{
    ser2net_monitor_feed(tcp_port, stream, data, len);
}

#if !ENABLE_CONTROL_PORT
static inline bool ser2net_control_start(const struct ser2net_control_context *ctx)
{
    (void) ctx;
    return false;
}

static inline void ser2net_control_stop(void) {}

#endif

#endif /* SER2NET_CONTROL_PORT_H */
