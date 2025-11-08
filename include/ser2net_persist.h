/*
 * ser2net MCU - Persistence hooks
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

#ifndef SER2NET_PERSIST_H
#define SER2NET_PERSIST_H

#include <stddef.h>
#include <stdint.h>

#include "ser2net_os.h"

struct ser2net_esp32_serial_port_cfg;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Optional persistence callbacks used by the runtime to store topology.
 */
struct ser2net_esp32_serial_port_cfg;

struct ser2net_persist_ops {
    void *ctx;
    /**
     * @brief Load previously stored serial port table.
     *
     * @param ctx User context.
     * @param ports Destination buffer.
     * @param inout_count On entry contains buffer capacity, on exit contains
     *                    number of valid entries.
     * @return `pdPASS` when data was loaded successfully.
     */
    BaseType_t (*load_ports)(void *ctx,
                             struct ser2net_esp32_serial_port_cfg *ports,
                             size_t *inout_count);
    /**
     * @brief Persist the current serial port table.
     */
    BaseType_t (*save_ports)(void *ctx,
                             const struct ser2net_esp32_serial_port_cfg *ports,
                             size_t count);
    /**
     * @brief Load control-port configuration (tcp port/backlog).
     */
    BaseType_t (*load_control)(void *ctx,
                               uint16_t *tcp_port,
                               int *backlog);
    /**
     * @brief Persist control-port configuration.
     */
    BaseType_t (*save_control)(void *ctx,
                               uint16_t tcp_port,
                               int backlog);
};

#ifdef __cplusplus
}
#endif

#endif /* SER2NET_PERSIST_H */
