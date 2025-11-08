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

/*
 *  ser2net - Platform adapters for FreeRTOS runtime
 *
 *  This file provides thin wrappers that translate the abstract
 *  ser2net interface into concrete SDK calls for ESP-IDF and STM32
 *  targets.  The focus is on keeping the logic high-level and easy to
 *  replace on other platforms.
 */

#include "adapters.h"

#if SER2NET_TARGET == SER2NET_PLATFORM_ESP32

#include <string.h>
#include <stdbool.h>

#include "ser2net_log.h"

/* ---------------------------------------------------------------------- */

#endif /* SER2NET_PLATFORM_ESP32 */
/* ESP32 / ESP-IDF networking                                             */
/* ---------------------------------------------------------------------- */

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#ifndef SER2NET_MAX_PORTS
#define SER2NET_MAX_PORTS 8
#endif

struct esp32_network_ctx {
    struct ser2net_esp32_network_cfg cfg;
    int listen_fd;
};

struct esp32_client_handle {
    int fd;
};



static BaseType_t esp32_open_listener(void *ctx, ser2net_listener_handle_t *out_listener);
static BaseType_t esp32_accept_client(void *ctx, ser2net_listener_handle_t listener,
                                      ser2net_client_handle_t *out_client, ser2net_tick_t timeout_ticks);
static void esp32_close_client(void *ctx, ser2net_client_handle_t client_handle);
static int esp32_client_recv(void *ctx, ser2net_client_handle_t client_handle,
                             void *buf, size_t len, ser2net_tick_t timeout_ticks);
static int esp32_client_send(void *ctx, ser2net_client_handle_t client_handle,
                             const void *buf, size_t len, ser2net_tick_t timeout_ticks);
static void esp32_client_shutdown(void *ctx, ser2net_client_handle_t client_handle);

struct esp32_network_if_entry {
    struct ser2net_network_if iface;
    struct esp32_network_ctx ctx;
    bool in_use;
};

static struct esp32_network_if_entry g_esp32_network_entries[SER2NET_MAX_PORTS];

static const struct ser2net_network_if g_esp32_network_if_template = {
    .ctx = NULL,
    .open_listener = esp32_open_listener,
    .accept_client = esp32_accept_client,
    .close_client = esp32_close_client,
    .client_recv = esp32_client_recv,
    .client_send = esp32_client_send,
    .client_shutdown = esp32_client_shutdown
};

/**
 * @brief Open a TCP listener bound to the configured IP/port.
 *
 * @param ctx Pointer to ::esp32_network_ctx describing the socket.
 * @param out_listener Receives the opaque listener handle on success.
 * @return `pdPASS` when the socket is ready, otherwise `pdFAIL`.
 */
static BaseType_t
esp32_open_listener(void *ctx, ser2net_listener_handle_t *out_listener)
{
    struct esp32_network_ctx *c = ctx;
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    int yes = 1;

    if (fd < 0)
        return pdFAIL;

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(c->cfg.listen_port);

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(fd);
        return pdFAIL;
    }

    if (listen(fd, c->cfg.backlog <= 0 ? 4 : c->cfg.backlog) < 0) {
        close(fd);
        return pdFAIL;
    }

    c->listen_fd = fd;
    *out_listener = (ser2net_listener_handle_t) c;
    return pdPASS;
}

/**
 * @brief Wait for incoming connections (with optional timeout).
 *
 * @param ctx Unused (kept for API symmetry).
 * @param listener Handle returned by ::esp32_open_listener().
 * @param out_client Receives the accepted client.
 * @param timeout_ticks FreeRTOS ticks to wait (or `SER2NET_OS_WAIT_FOREVER`).
 * @return `pdPASS` on success, otherwise `pdFAIL`.
 */
static BaseType_t
esp32_accept_client(void *ctx, ser2net_listener_handle_t listener,
                    ser2net_client_handle_t *out_client, ser2net_tick_t timeout_ticks)
{
    struct esp32_network_ctx *c = listener;
    fd_set readfds;
    struct timeval tv;
    int rv;

    (void) ctx;

    FD_ZERO(&readfds);
    FD_SET(c->listen_fd, &readfds);

    struct timeval *timeout_ptr = NULL;
    if (timeout_ticks != SER2NET_OS_WAIT_FOREVER) {
        uint32_t timeout_ms = timeout_ticks * portTICK_PERIOD_MS;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        timeout_ptr = &tv;
    }

    rv = select(c->listen_fd + 1, &readfds, NULL, NULL, timeout_ptr);
    if (rv <= 0)
        return (rv == 0) ? pdFAIL : pdFAIL;

    struct esp32_client_handle *client = ser2net_os_malloc(sizeof(*client));
    if (!client)
        return pdFAIL;

    client->fd = accept(c->listen_fd, NULL, NULL);
    if (client->fd < 0) {
        ser2net_os_free(client);
        return pdFAIL;
    }

    int flags = fcntl(client->fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(client->fd, F_SETFL, flags | O_NONBLOCK);

    *out_client = client;
    return pdPASS;
}

/**
 * @brief Gracefully close a client socket and reclaim memory.
 *
 * @param ctx Unused.
 * @param client_handle Handle returned by ::esp32_accept_client().
 */
static void
esp32_close_client(void *ctx, ser2net_client_handle_t client_handle)
{
    (void) ctx;

    if (!client_handle)
        return;

    struct esp32_client_handle *client = client_handle;
    close(client->fd);
    ser2net_os_free(client);
}

/**
 * @brief Poll a socket for data while honouring runtime timeouts.
 *
 * @param ctx Unused.
 * @param client_handle Accepted client handle.
 * @param buf Destination buffer.
 * @param len Maximum number of bytes to read.
 * @param timeout_ticks Maximum wait in FreeRTOS ticks.
 * @return Bytes read, 0 on timeout, or -1/-2 on error/disconnect.
 */
static int
esp32_client_recv(void *ctx, ser2net_client_handle_t client_handle,
                  void *buf, size_t len, ser2net_tick_t timeout_ticks)
{
    (void) ctx;
    struct esp32_client_handle *client = client_handle;
    fd_set readfds;
    struct timeval tv;
    struct timeval *tv_ptr = NULL;

    if (!client || client->fd < 0 || !buf || len == 0)
        return -1;

    FD_ZERO(&readfds);
    FD_SET(client->fd, &readfds);

    if (timeout_ticks != SER2NET_OS_WAIT_FOREVER) {
        uint32_t timeout_ms = timeout_ticks * portTICK_PERIOD_MS;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tv_ptr = &tv;
    }

    int rv = select(client->fd + 1, &readfds, NULL, NULL, tv_ptr);
    if (rv <= 0) {
        if (rv == 0)
            return 0;
        return -1;
    }

    rv = recv(client->fd, buf, len, 0);
    if (rv < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    if (rv == 0) {
        return -2;
    }

    return rv;
}

/**
 * @brief Push bytes to the network (non-blocking).
 *
 * @param ctx Unused.
 * @param client_handle Accepted client handle.
 * @param buf Payload to send.
 * @param len Payload size.
 * @param timeout_ticks Unused (API symmetry).
 * @return Bytes written, 0 on EAGAIN, -1 on fatal error.
 */
static int
esp32_client_send(void *ctx, ser2net_client_handle_t client_handle,
                  const void *buf, size_t len, ser2net_tick_t timeout_ticks)
{
    (void) ctx;
    (void) timeout_ticks;
    struct esp32_client_handle *client = client_handle;

    if (!client || client->fd < 0 || !buf || len == 0)
        return -1;

    int rv = send(client->fd, buf, len, 0);
    if (rv < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }

    return rv;
}

/**
 * @brief Trigger a shutdown on the peer socket to force disconnect.
 *
 * @param ctx Unused.
 * @param client_handle Accepted client handle.
 */
static void
esp32_client_shutdown(void *ctx, ser2net_client_handle_t client_handle)
{
    (void) ctx;
    struct esp32_client_handle *client = client_handle;

    if (!client || client->fd < 0)
        return;

    shutdown(client->fd, SHUT_RDWR);
}

/**
 * @brief Obtain a network adapter instance for the requested TCP listener.
 *
 * @param cfg TCP port/backlog description.
 * @return Pointer to a dedicated ::ser2net_network_if or NULL when exhausted.
 */
const struct ser2net_network_if *
ser2net_esp32_get_network_if(const struct ser2net_esp32_network_cfg *cfg)
{
    /* Provide a unique network_if wrapper per listener (TCP port). */
    if (!cfg || cfg->listen_port <= 0)
        return NULL;

    for (size_t i = 0; i < SER2NET_MAX_PORTS; ++i) {
        if (g_esp32_network_entries[i].in_use)
            continue;

        g_esp32_network_entries[i].iface = g_esp32_network_if_template;
        g_esp32_network_entries[i].ctx.cfg = *cfg;
        g_esp32_network_entries[i].ctx.listen_fd = -1;
        g_esp32_network_entries[i].iface.ctx = &g_esp32_network_entries[i].ctx;
        g_esp32_network_entries[i].in_use = true;
        return &g_esp32_network_entries[i].iface;
    }

    return NULL;
}

/**
 * @brief Release a network_if slot (and close any lingering socket).
 *
 * @param iface Network interface previously returned by
 *              ::ser2net_esp32_get_network_if().
 */
void
ser2net_esp32_release_network_if(const struct ser2net_network_if *iface)
{
    /* Return network_if slot back to pool (close listener if still open). */
    if (!iface)
        return;

    for (size_t i = 0; i < SER2NET_MAX_PORTS; ++i) {
        if (!g_esp32_network_entries[i].in_use)
            continue;
        if (&g_esp32_network_entries[i].iface != iface)
            continue;

        if (g_esp32_network_entries[i].ctx.listen_fd >= 0) {
            close(g_esp32_network_entries[i].ctx.listen_fd);
            g_esp32_network_entries[i].ctx.listen_fd = -1;
        }
        g_esp32_network_entries[i].in_use = false;
        g_esp32_network_entries[i].iface.ctx = NULL;
        return;
    }
}


/* ---------------------------------------------------------------------- */
