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

/**
 * @file json_config.h
 * @brief JSON configuration parsing helpers for the MCU runtime.
 *
 * Translates a JSON configuration string into runtime and adapter structures.
 * Currently only ESP32 specific schemas are supported.
 */

#ifndef SER2NET_FREERTOS_JSON_CONFIG_H
#define SER2NET_FREERTOS_JSON_CONFIG_H

#include "config.h"

/** @brief Parse an embedded JSON configuration string (ESP32 specific). */
BaseType_t ser2net_load_config_json_esp32(const char *json,
        struct ser2net_app_config *app_cfg,
        struct ser2net_esp32_network_cfg *net_cfg,
        struct ser2net_esp32_serial_cfg *serial_cfg,
        struct ser2net_esp32_serial_port_cfg *ports,
        size_t max_ports);

/** @brief Retrieve the last human readable parser error (if any). */
const char *ser2net_json_last_error(void);

#endif /* SER2NET_FREERTOS_JSON_CONFIG_H */
