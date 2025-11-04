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
 * @file config.h
 * @brief High-level configuration helpers for the MCU runtime.
 */

#ifndef SER2NET_FREERTOS_CONFIG_H
#define SER2NET_FREERTOS_CONFIG_H

#include "runtime.h"
#include "session_ops.h"
#include "adapters.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Aggregated application configuration used by ::ser2net_start().
 */
struct ser2net_app_config {
    struct ser2net_runtime_config runtime_cfg;
    struct ser2net_basic_session_cfg session_cfg;
    const struct ser2net_network_if *network_if;
    const struct ser2net_serial_if *serial_if;
};

/** @brief Fill ::ser2net_runtime_config with default values. */
void ser2net_runtime_config_init(struct ser2net_runtime_config *cfg);
/** @brief Fill ::ser2net_basic_session_cfg with default values. */
void ser2net_session_config_init(struct ser2net_basic_session_cfg *cfg);

/** @brief Initialise and start the runtime using the supplied configuration. */
BaseType_t ser2net_start(const struct ser2net_app_config *cfg);
/** @brief Stop the runtime (wrapper around ::ser2net_runtime_stop). */
void ser2net_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* SER2NET_FREERTOS_CONFIG_H */
