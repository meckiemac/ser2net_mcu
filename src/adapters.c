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

/*
 *  ser2net - Platform adapters for FreeRTOS runtime
 *
 *  This file provides thin wrappers that translate the abstract
 *  ser2net interface into concrete SDK calls for ESP-IDF and STM32
 *  targets.  The focus is on keeping the logic high-level and easy to
 *  replace on other platforms.
 */

#include "adapters.h"

#include <string.h>
#include <stdbool.h>

#include "esp_log.h"

/* ---------------------------------------------------------------------- */
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

static const char *ADAPTER_LOG_TAG = "ser2net_adapter";

static BaseType_t esp32_open_listener(void *ctx, ser2net_listener_handle_t *out_listener);
static BaseType_t esp32_accept_client(void *ctx, ser2net_listener_handle_t listener,
                                      ser2net_client_handle_t *out_client, TickType_t timeout_ticks);
static void esp32_close_client(void *ctx, ser2net_client_handle_t client_handle);
static int esp32_client_recv(void *ctx, ser2net_client_handle_t client_handle,
                             void *buf, size_t len, TickType_t timeout_ticks);
static int esp32_client_send(void *ctx, ser2net_client_handle_t client_handle,
                             const void *buf, size_t len, TickType_t timeout_ticks);
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

/* Open a TCP listener bound to the configured IP/port. */
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

/* Wait for incoming connections (with optional timeout). */
static BaseType_t
esp32_accept_client(void *ctx, ser2net_listener_handle_t listener,
                    ser2net_client_handle_t *out_client, TickType_t timeout_ticks)
{
    struct esp32_network_ctx *c = listener;
    fd_set readfds;
    struct timeval tv;
    int rv;

    (void) ctx;

    FD_ZERO(&readfds);
    FD_SET(c->listen_fd, &readfds);

    struct timeval *timeout_ptr = NULL;
    if (timeout_ticks != portMAX_DELAY) {
        uint32_t timeout_ms = timeout_ticks * portTICK_PERIOD_MS;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        timeout_ptr = &tv;
    }

    rv = select(c->listen_fd + 1, &readfds, NULL, NULL, timeout_ptr);
    if (rv <= 0)
        return (rv == 0) ? pdFAIL : pdFAIL;

    struct esp32_client_handle *client = pvPortMalloc(sizeof(*client));
    if (!client)
        return pdFAIL;

    client->fd = accept(c->listen_fd, NULL, NULL);
    if (client->fd < 0) {
        vPortFree(client);
        return pdFAIL;
    }

    int flags = fcntl(client->fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(client->fd, F_SETFL, flags | O_NONBLOCK);

    *out_client = client;
    return pdPASS;
}

/* Gracefully close a client socket. */
static void
esp32_close_client(void *ctx, ser2net_client_handle_t client_handle)
{
    (void) ctx;

    if (!client_handle)
        return;

    struct esp32_client_handle *client = client_handle;
    close(client->fd);
    vPortFree(client);
}

/* Poll socket for data, respecting timeout semantics used by runtime. */
static int
esp32_client_recv(void *ctx, ser2net_client_handle_t client_handle,
                  void *buf, size_t len, TickType_t timeout_ticks)
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

    if (timeout_ticks != portMAX_DELAY) {
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

/* Push data to the network (non-blocking, optional timeout). */
static int
esp32_client_send(void *ctx, ser2net_client_handle_t client_handle,
                  const void *buf, size_t len, TickType_t timeout_ticks)
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

static void
esp32_client_shutdown(void *ctx, ser2net_client_handle_t client_handle)
{
    (void) ctx;
    struct esp32_client_handle *client = client_handle;

    if (!client || client->fd < 0)
        return;

    shutdown(client->fd, SHUT_RDWR);
}

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
/* ESP32 / ESP-IDF serial                                                 */
/* ---------------------------------------------------------------------- */

struct esp32_serial_port_state {
    const struct ser2net_esp32_serial_port_cfg *cfg;
    bool driver_installed;
};

struct esp32_serial_ctx {
    struct ser2net_esp32_serial_cfg cfg;
    struct ser2net_esp32_serial_port_cfg storage[SER2NET_MAX_PORTS];
    struct esp32_serial_port_state ports[SER2NET_MAX_PORTS];
};

struct esp32_serial_handle {
    const struct esp32_serial_port_state *state;
};

static struct esp32_serial_ctx g_esp32_serial_ctx;

/* Locate requested UART, install driver if needed and return handle. */
static BaseType_t
esp32_open_serial(void *ctx, int port_id, ser2net_serial_handle_t *out_serial)
{
    (void) ctx;

    for (size_t i = 0; i < g_esp32_serial_ctx.cfg.num_ports && i < SER2NET_MAX_PORTS; ++i) {
        struct esp32_serial_port_state *state = &g_esp32_serial_ctx.ports[i];

        if (!state->cfg || state->cfg->port_id != port_id)
            continue;

        if (!state->driver_installed) {
            uart_config_t uart_cfg = {
                .baud_rate = state->cfg->baud_rate,
                .data_bits = state->cfg->data_bits,
                .parity = state->cfg->parity,
                .stop_bits = state->cfg->stop_bits,
                .flow_ctrl = state->cfg->flow_ctrl,
                .source_clk = UART_SCLK_APB
            };

            if (uart_param_config(state->cfg->uart_num, &uart_cfg) != ESP_OK)
                return pdFAIL;

            if (uart_set_pin(state->cfg->uart_num,
                             state->cfg->tx_pin,
                             state->cfg->rx_pin,
                             state->cfg->rts_pin,
                             state->cfg->cts_pin) != ESP_OK)
                return pdFAIL;

            if (uart_driver_install(state->cfg->uart_num,
                                    g_esp32_serial_ctx.cfg.rx_buffer_size,
                                    g_esp32_serial_ctx.cfg.tx_buffer_size,
                                    0, NULL, 0) != ESP_OK)
                return pdFAIL;

            state->driver_installed = true;
        }

        uart_flush_input(state->cfg->uart_num);

        struct esp32_serial_handle *handle = pvPortMalloc(sizeof(*handle));
        if (!handle)
            return pdFAIL;

        handle->state = state;
        *out_serial = handle;
        return pdPASS;
    }

    return pdFAIL;
}

static void
esp32_close_serial(void *ctx, ser2net_serial_handle_t serial)
{
    (void) ctx;

    if (!serial)
        return;

    struct esp32_serial_handle *handle = serial;
    uart_flush(handle->state->cfg->uart_num);
    uart_driver_delete(handle->state->cfg->uart_num);
    ((struct esp32_serial_port_state *) handle->state)->driver_installed = false;
    vPortFree(handle);
}

static int
esp32_serial_read(void *ctx, ser2net_serial_handle_t serial,
                  void *buf, size_t len, TickType_t timeout_ticks)
{
    (void) ctx;
    struct esp32_serial_handle *handle = serial;

    if (!handle || !handle->state || !buf || len == 0)
        return -1;

    int rv = uart_read_bytes(handle->state->cfg->uart_num, buf, len, timeout_ticks);
    if (rv < 0)
        return -1;

    return rv;
}

static int
esp32_serial_write(void *ctx, ser2net_serial_handle_t serial,
                   const void *buf, size_t len)
{
    (void) ctx;
    struct esp32_serial_handle *handle = serial;

    if (!handle || !handle->state || !buf || len == 0)
        return -1;

    int rv = uart_write_bytes(handle->state->cfg->uart_num, buf, len);
    if (rv < 0)
        return -1;

    return rv;
}

static BaseType_t
esp32_serial_configure(void *ctx, ser2net_serial_handle_t serial,
                       int baud, int data_bits, int parity,
                       int stop_bits, int flow_control)
{
    (void) ctx;
    struct esp32_serial_handle *handle = serial;
    uart_port_t uart_num;
    uart_word_length_t word_len;
    uart_parity_t parity_mode;
    uart_stop_bits_t stop_mode;
    uart_hw_flowcontrol_t flow_mode;

    if (!handle || !handle->state)
        return pdFAIL;

    uart_num = handle->state->cfg->uart_num;

    ESP_LOGI(ADAPTER_LOG_TAG,
             "serial_configure(uart=%d, baud=%d, data_bits=%d, parity=%d, stop_bits=%d, flow_control=%d)",
             uart_num, baud, data_bits, parity, stop_bits, flow_control);

    if (uart_set_baudrate(uart_num, baud) != ESP_OK)
        return pdFAIL;

    switch (data_bits) {
    case 5: word_len = UART_DATA_5_BITS; break;
    case 6: word_len = UART_DATA_6_BITS; break;
    case 7: word_len = UART_DATA_7_BITS; break;
    case 8: word_len = UART_DATA_8_BITS; break;
#ifdef UART_DATA_9_BITS
    case 9: word_len = UART_DATA_9_BITS; break;
#endif
    default:
        return pdFAIL;
    }
    if (uart_set_word_length(uart_num, word_len) != ESP_OK)
        return pdFAIL;

    switch (parity) {
    case 0: parity_mode = UART_PARITY_DISABLE; break;
    case 1: parity_mode = UART_PARITY_ODD; break;
    case 2: parity_mode = UART_PARITY_EVEN; break;
    default: return pdFAIL;
    }
    if (uart_set_parity(uart_num, parity_mode) != ESP_OK)
        return pdFAIL;

    switch (stop_bits) {
    case 1: stop_mode = UART_STOP_BITS_1; break;
    case 2: stop_mode = UART_STOP_BITS_2; break;
#ifdef UART_STOP_BITS_1_5
    case 15: stop_mode = UART_STOP_BITS_1_5; break;
#endif
    default: return pdFAIL;
    }
    if (uart_set_stop_bits(uart_num, stop_mode) != ESP_OK)
        return pdFAIL;

    switch (flow_control) {
    case 0:
        flow_mode = UART_HW_FLOWCTRL_DISABLE;
        break;
    case 1:
        flow_mode = UART_HW_FLOWCTRL_CTS_RTS;
        break;
    default:
        return pdFAIL;
    }
    if (uart_set_hw_flow_ctrl(uart_num, flow_mode, 120) != ESP_OK)
        return pdFAIL;

    return pdPASS;
}

static const struct ser2net_serial_if g_esp32_serial_if = {
    .ctx = &g_esp32_serial_ctx,
    .open_serial = esp32_open_serial,
    .close_serial = esp32_close_serial,
    .serial_read = esp32_serial_read,
    .serial_write = esp32_serial_write,
    .serial_configure = esp32_serial_configure
};

const struct ser2net_serial_if *
ser2net_esp32_get_serial_if(const struct ser2net_esp32_serial_cfg *cfg)
{
    /* Copy user configuration so runtime can open/close UARTs on demand. */
    if (cfg) {
        g_esp32_serial_ctx.cfg = *cfg;
        size_t limit = cfg->num_ports;
        if (limit > SER2NET_MAX_PORTS)
            limit = SER2NET_MAX_PORTS;
        g_esp32_serial_ctx.cfg.num_ports = limit;
        g_esp32_serial_ctx.cfg.ports = g_esp32_serial_ctx.storage;
        memset(g_esp32_serial_ctx.storage, 0, sizeof(g_esp32_serial_ctx.storage));
        memset(g_esp32_serial_ctx.ports, 0, sizeof(g_esp32_serial_ctx.ports));
        for (size_t i = 0; i < limit; ++i) {
            g_esp32_serial_ctx.storage[i] = cfg->ports[i];
            g_esp32_serial_ctx.ports[i].cfg = &g_esp32_serial_ctx.storage[i];
            g_esp32_serial_ctx.ports[i].driver_installed = false;
        }
    }
    return &g_esp32_serial_if;
}

BaseType_t
ser2net_esp32_register_port(const struct ser2net_esp32_serial_port_cfg *cfg)
{
    if (!cfg)
        return pdFAIL;

    if (g_esp32_serial_ctx.cfg.num_ports >= SER2NET_MAX_PORTS)
        return pdFAIL;

    for (size_t i = 0; i < g_esp32_serial_ctx.cfg.num_ports; ++i) {
        if (g_esp32_serial_ctx.storage[i].port_id == cfg->port_id ||
            g_esp32_serial_ctx.storage[i].tcp_port == cfg->tcp_port)
            return pdFAIL;
    }

    size_t idx = g_esp32_serial_ctx.cfg.num_ports;
    g_esp32_serial_ctx.storage[idx] = *cfg;
    g_esp32_serial_ctx.ports[idx].cfg = &g_esp32_serial_ctx.storage[idx];
    g_esp32_serial_ctx.ports[idx].driver_installed = false;
    g_esp32_serial_ctx.cfg.num_ports++;

    return pdPASS;
}

void
ser2net_esp32_unregister_port(int port_id)
{
    size_t count = g_esp32_serial_ctx.cfg.num_ports;
    size_t index = count;

    for (size_t i = 0; i < count; ++i) {
        if (g_esp32_serial_ctx.storage[i].port_id == port_id) {
            index = i;
            break;
        }
    }

    if (index >= count)
        return;

    if (g_esp32_serial_ctx.ports[index].driver_installed) {
        uart_driver_delete(g_esp32_serial_ctx.storage[index].uart_num);
        g_esp32_serial_ctx.ports[index].driver_installed = false;
    }

    for (size_t i = index; i + 1 < count; ++i) {
        g_esp32_serial_ctx.storage[i] = g_esp32_serial_ctx.storage[i + 1];
        g_esp32_serial_ctx.ports[i].cfg = &g_esp32_serial_ctx.storage[i];
        g_esp32_serial_ctx.ports[i].driver_installed = g_esp32_serial_ctx.ports[i + 1].driver_installed;
    }

    if (count > 0) {
        size_t last = count - 1;
        memset(&g_esp32_serial_ctx.storage[last], 0, sizeof(g_esp32_serial_ctx.storage[last]));
        g_esp32_serial_ctx.ports[last].cfg = NULL;
        g_esp32_serial_ctx.ports[last].driver_installed = false;
        g_esp32_serial_ctx.cfg.num_ports = last;
    }
}


/* ---------------------------------------------------------------------- */
