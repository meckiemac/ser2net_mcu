/*
 * ser2net MCU - Embedded RFC2217 runtime
 *
 * Copyright (C) 2025 Andreas Merk
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
 * @file session_ops.h
 * @brief Session handler glue between the runtime and RFC2217 implementation.
 */

#ifndef SER2NET_FREERTOS_SESSION_OPS_H
#define SER2NET_FREERTOS_SESSION_OPS_H

#include <stddef.h>
#include <stdint.h>

#include "ser2net_opts.h"

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SER2NET_MAX_PORTS 8

enum ser2net_port_mode {
    SER2NET_PORT_MODE_TELNET = 0,
    SER2NET_PORT_MODE_RAW,
    SER2NET_PORT_MODE_RAWLP
};

/** @brief Canonical serial parameters used across the runtime. */
struct ser2net_serial_params {
    int baud;
    int data_bits;
    int parity;
    int stop_bits;
    int flow_control;
};

/** @brief GPIO assignment used when reconfiguring an UART. */
struct ser2net_pin_config {
    int uart_num;
    int tx_pin;
    int rx_pin;
    int rts_pin;
    int cts_pin;
};

/** @brief Per-port defaults consumed by ::ser2net_basic_session_ops_init(). */
struct ser2net_basic_session_cfg {
    size_t net_buf_size;
    size_t serial_buf_size;
    size_t port_count;
    int port_ids[SER2NET_MAX_PORTS];
    uint16_t tcp_ports[SER2NET_MAX_PORTS];
    enum ser2net_port_mode port_modes[SER2NET_MAX_PORTS];
    uint32_t idle_timeout_ms[SER2NET_MAX_PORTS];
    struct ser2net_serial_params port_params[SER2NET_MAX_PORTS];
};

const struct ser2net_session_ops *
ser2net_basic_session_ops_init(const struct ser2net_network_if *network,
                               const struct ser2net_serial_if *serial,
                               const struct ser2net_basic_session_cfg *cfg);

/** @brief Round-robin helper returning the next configured port_id. */
int ser2net_basic_next_port(void);
/** @brief Report per-port active session counters. */
size_t ser2net_get_port_stats(int *port_ids, int *active_sessions, size_t max_entries);
/** @brief Update stored defaults for a port. */
BaseType_t ser2net_session_update_defaults(int port_id,
                                           const struct ser2net_serial_params *params,
                                           uint32_t idle_timeout_ms);
/** @brief Apply new settings to a live session. */
BaseType_t ser2net_session_apply_config(void *session_ctx,
                                        const struct ser2net_serial_params *params);
/** @brief Change the mode of a registered port. */
BaseType_t ser2net_session_set_mode(int port_id, enum ser2net_port_mode mode);
/** @brief Register a port with the session layer. */
BaseType_t ser2net_session_register_port(int port_id,
                                         uint16_t tcp_port,
                                         enum ser2net_port_mode mode,
                                         const struct ser2net_serial_params *defaults,
                                         uint32_t idle_timeout_ms);
/** @brief Remove a registered port. */
BaseType_t ser2net_session_unregister_port(int port_id);

#ifdef __cplusplus
}
#endif

#endif /* SER2NET_FREERTOS_SESSION_OPS_H */
