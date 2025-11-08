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

#include "monitor_bus.h"

#if ENABLE_MONITORING

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

#ifndef SER2NET_MONITOR_MAX_SINKS
#define SER2NET_MONITOR_MAX_SINKS 4
#endif

struct monitor_sink_entry {
    ser2net_monitor_sink_t sink;
    void *ctx;
};

static struct monitor_sink_entry sink_table[SER2NET_MONITOR_MAX_SINKS];
static portMUX_TYPE monitor_bus_mux = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief Subscribe to monitor frames (see monitor_bus.h for API contract).
 *
 * @param sink Callback invoked for each published chunk.
 * @param ctx   User data passed back to the callback.
 * @return true when the sink was registered successfully.
 */
bool ser2net_monitor_register_sink(ser2net_monitor_sink_t sink, void *ctx)
{
    if (!sink)
        return false;

    bool inserted = false;
    taskENTER_CRITICAL(&monitor_bus_mux);
    for (size_t i = 0; i < SER2NET_MONITOR_MAX_SINKS; ++i) {
        if (sink_table[i].sink == sink && sink_table[i].ctx == ctx) {
            inserted = true;
            break;
        }
        if (!sink_table[i].sink && !inserted) {
            sink_table[i].ctx = ctx;
            sink_table[i].sink = sink;
            inserted = true;
        }
    }
    taskEXIT_CRITICAL(&monitor_bus_mux);
    return inserted;
}

/**
 * @brief Remove a subscriber previously registered via ser2net_monitor_register_sink().
 */
void ser2net_monitor_unregister_sink(ser2net_monitor_sink_t sink, void *ctx)
{
    if (!sink)
        return;

    taskENTER_CRITICAL(&monitor_bus_mux);
    for (size_t i = 0; i < SER2NET_MONITOR_MAX_SINKS; ++i) {
        if (sink_table[i].sink == sink && sink_table[i].ctx == ctx) {
            sink_table[i].sink = NULL;
            sink_table[i].ctx = NULL;
            break;
        }
    }
    taskEXIT_CRITICAL(&monitor_bus_mux);
}

/**
 * @brief Publish monitor data to all registered sinks.
 *
 * @param tcp_port Listener the bytes originated from.
 * @param stream Direction (TCP or terminal).
 * @param data Payload bytes.
 * @param len Length of @p data.
 */
void ser2net_monitor_feed(uint16_t tcp_port,
                          enum ser2net_monitor_stream stream,
                          const uint8_t *data,
                          size_t len)
{
    if (!data || len == 0)
        return;

    struct monitor_sink_entry snapshot[SER2NET_MONITOR_MAX_SINKS];
    size_t count = 0;

    taskENTER_CRITICAL(&monitor_bus_mux);
    for (size_t i = 0; i < SER2NET_MONITOR_MAX_SINKS; ++i) {
        if (sink_table[i].sink) {
            snapshot[count++] = sink_table[i];
        }
    }
    taskEXIT_CRITICAL(&monitor_bus_mux);

    for (size_t i = 0; i < count; ++i) {
        if (snapshot[i].sink)
            snapshot[i].sink(snapshot[i].ctx, tcp_port, stream, data, len);
    }
}

#endif /* ENABLE_MONITORING */
