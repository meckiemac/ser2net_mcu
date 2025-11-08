/*
 * ser2net MCU - Interface definitions
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

#ifndef SER2NET_IF_H
#define SER2NET_IF_H

#include <stddef.h>

#include "ser2net_os.h"

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
    ser2net_os_status_t (*open_listener)(void *ctx, ser2net_listener_handle_t *out_listener);
    /** Accept a pending client connection. */
    ser2net_os_status_t (*accept_client)(void *ctx, ser2net_listener_handle_t listener,
                                         ser2net_client_handle_t *out_client, ser2net_tick_t timeout_ticks);
    /** Close a client connection immediately. */
    void (*close_client)(void *ctx, ser2net_client_handle_t client);
    /** Receive data from a client. */
    int (*client_recv)(void *ctx, ser2net_client_handle_t client,
                       void *buf, size_t len, ser2net_tick_t timeout_ticks);
    /** Send data to a client. */
    int (*client_send)(void *ctx, ser2net_client_handle_t client,
                       const void *buf, size_t len, ser2net_tick_t timeout_ticks);
    /** Shutdown the client connection gracefully. */
    void (*client_shutdown)(void *ctx, ser2net_client_handle_t client);
};

/**
 * @brief Call-backs required for a serial backend.
 */
struct ser2net_serial_if {
    void *ctx;
    ser2net_os_status_t (*open_serial)(void *ctx, int port_id, ser2net_serial_handle_t *out_serial);
    void (*close_serial)(void *ctx, ser2net_serial_handle_t serial);
    int (*serial_read)(void *ctx, ser2net_serial_handle_t serial,
                       void *buf, size_t len, ser2net_tick_t timeout_ticks);
    int (*serial_write)(void *ctx, ser2net_serial_handle_t serial,
                        const void *buf, size_t len);
    ser2net_os_status_t (*serial_configure)(void *ctx, ser2net_serial_handle_t serial,
                                            int baud, int data_bits, int parity,
                                            int stop_bits, int flow_control);
};

#ifdef __cplusplus
}
#endif

#endif /* SER2NET_IF_H */
