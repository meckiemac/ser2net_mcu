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

#ifndef SER2NET_MONITOR_BUS_H
#define SER2NET_MONITOR_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ser2net_opts.h"

enum ser2net_monitor_stream {
    SER2NET_MONITOR_STREAM_TCP = 0,
    SER2NET_MONITOR_STREAM_TERM = 1
};

typedef void (*ser2net_monitor_sink_t)(void *ctx,
                                       uint16_t tcp_port,
                                       enum ser2net_monitor_stream stream,
                                       const uint8_t *data,
                                       size_t len);

#ifdef __cplusplus
extern "C" {
#endif

bool ser2net_monitor_register_sink(ser2net_monitor_sink_t sink, void *ctx);
void ser2net_monitor_unregister_sink(ser2net_monitor_sink_t sink, void *ctx);
void ser2net_monitor_feed(uint16_t tcp_port,
                          enum ser2net_monitor_stream stream,
                          const uint8_t *data,
                          size_t len);

#ifdef __cplusplus
}
#endif

#if !ENABLE_MONITORING
static inline bool ser2net_monitor_register_sink(ser2net_monitor_sink_t sink, void *ctx)
{
    (void) sink;
    (void) ctx;
    return false;
}

static inline void ser2net_monitor_unregister_sink(ser2net_monitor_sink_t sink, void *ctx)
{
    (void) sink;
    (void) ctx;
}

static inline void ser2net_monitor_feed(uint16_t tcp_port,
                                        enum ser2net_monitor_stream stream,
                                        const uint8_t *data,
                                        size_t len)
{
    (void) tcp_port;
    (void) stream;
    (void) data;
    (void) len;
}
#endif

#endif /* SER2NET_MONITOR_BUS_H */

