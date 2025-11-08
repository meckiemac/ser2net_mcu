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
 * @file adapters.h
 * @brief Platform specific adapters for the ser2net MCU runtime.
 *
 * Provides concrete implementations of the ser2net network and serial
 * interfaces for ESP-IDF environments.  The goal is to keep the high-level
 * runtime code agnostic of the underlying SDK while still offering hooks that
 * work out-of-the-box on common MCU targets.
 */

#ifndef SER2NET_FREERTOS_ADAPTERS_H
#define SER2NET_FREERTOS_ADAPTERS_H

#include <stddef.h>
#include <stdint.h>

#include "ser2net_platform.h"
#include "ser2net_if.h"
#include "session_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

#if SER2NET_TARGET == SER2NET_PLATFORM_ESP32

/* --------- ESP-IDF networking --------- */


/** @brief Configuration for an ESP32 TCP listener. */
struct ser2net_esp32_network_cfg {
    uint16_t listen_port;
    int backlog;
};

/** @brief Obtain a reusable ESP-IDF network interface instance. */
const struct ser2net_network_if *
ser2net_esp32_get_network_if(const struct ser2net_esp32_network_cfg *cfg);

/** @brief Release a previously acquired network interface slot. */
void ser2net_esp32_release_network_if(const struct ser2net_network_if *iface);


/* --------- ESP-IDF serial --------- */

#include "driver/uart.h"

/** @brief UART + TCP listener pairing for an ESP32. */
struct ser2net_esp32_serial_port_cfg {
    int port_id;
    uart_port_t uart_num;
    int tx_pin;
    int rx_pin;
    int rts_pin;
    int cts_pin;
    uint16_t tcp_port;
    int tcp_backlog;
    int baud_rate;
    uart_word_length_t data_bits;
    uart_parity_t parity;
    uart_stop_bits_t stop_bits;
    uart_hw_flowcontrol_t flow_ctrl;
    enum ser2net_port_mode mode;
    uint32_t idle_timeout_ms;
    bool enabled;
};

/** @brief Global ESP32 serial configuration passed to ::ser2net_esp32_get_serial_if(). */
struct ser2net_esp32_serial_cfg {
    const struct ser2net_esp32_serial_port_cfg *ports;
    size_t num_ports;
    size_t rx_buffer_size;
    size_t tx_buffer_size;
};

/** @brief Access the ESP-IDF serial adapter implementation. */
const struct ser2net_serial_if *
ser2net_esp32_get_serial_if(const struct ser2net_esp32_serial_cfg *cfg);
/** @brief Register a port at runtime (called by ::ser2net_runtime_add_port()). */
BaseType_t ser2net_esp32_register_port(const struct ser2net_esp32_serial_port_cfg *cfg);
/** @brief Remove a previously registered port. */
void ser2net_esp32_unregister_port(int port_id);

#endif /* SER2NET_PLATFORM_ESP32 */

#if SER2NET_TARGET == SER2NET_PLATFORM_STM32

struct ser2net_stm32_network_cfg {
    uint16_t listen_port;
    int backlog;
};

const struct ser2net_network_if *
ser2net_stm32_get_network_if(const struct ser2net_stm32_network_cfg *cfg);
void ser2net_stm32_release_network_if(const struct ser2net_network_if *iface);

struct ser2net_stm32_serial_port_cfg {
    int port_id;
    uint8_t instance;
    uint16_t gpio_tx;
    uint16_t gpio_rx;
    uint32_t baud_rate;
    uint16_t tcp_port;
    int tcp_backlog;
    enum ser2net_port_mode mode;
    uint32_t idle_timeout_ms;
    bool enabled;
};

const struct ser2net_serial_if *
ser2net_stm32_get_serial_if(const struct ser2net_stm32_serial_port_cfg *cfg,
                            size_t num_ports);
BaseType_t ser2net_stm32_register_port(const struct ser2net_stm32_serial_port_cfg *cfg);
void ser2net_stm32_unregister_port(int port_id);

#endif /* SER2NET_PLATFORM_STM32 */

#if SER2NET_TARGET == SER2NET_PLATFORM_RP2040

struct ser2net_rp2040_network_cfg {
    uint16_t listen_port;
    int backlog;
};

const struct ser2net_network_if *
ser2net_rp2040_get_network_if(const struct ser2net_rp2040_network_cfg *cfg);
void ser2net_rp2040_release_network_if(const struct ser2net_network_if *iface);

struct ser2net_rp2040_serial_port_cfg {
    int port_id;
    uint8_t uart_index;
    uint8_t tx_pin;
    uint8_t rx_pin;
    uint32_t baud_rate;
    uint16_t tcp_port;
    int tcp_backlog;
    enum ser2net_port_mode mode;
    uint32_t idle_timeout_ms;
    bool enabled;
};

const struct ser2net_serial_if *
ser2net_rp2040_get_serial_if(const struct ser2net_rp2040_serial_port_cfg *cfg,
                             size_t num_ports);
BaseType_t ser2net_rp2040_register_port(const struct ser2net_rp2040_serial_port_cfg *cfg);
void ser2net_rp2040_unregister_port(int port_id);

#endif /* SER2NET_PLATFORM_RP2040 */

#ifdef __cplusplus
}
#endif

#endif /* SER2NET_FREERTOS_ADAPTERS_H */
