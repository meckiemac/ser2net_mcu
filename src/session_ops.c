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
 * RFC2217 session handler
 * -----------------------
 * Bridges between the MCU runtime (network/serial adapters) and the original
 * ser2net protocol core.  It encapsulates TELNET negotiation, RFC2217 option
 * handling and the bidirectional data shuffling between TCP and UART.
 *
 * The module keeps minimal state per session (current UART settings, TLS buffer
 * etc.) and exposes helper functions for the control port to query active
 * session counts.
 */

#include "session_ops.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"

#include "freertos/semphr.h"

#ifndef ARRAY_LEN
#define ARRAY_LEN(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Telnet command bytes */
#define TN_IAC        255
#define TN_DONT       254
#define TN_DO         253
#define TN_WONT       252
#define TN_WILL       251
#define TN_SB         250
#define TN_SE         240

/* Telnet options */
#define TN_OPT_BINARY    0
#define TN_OPT_COM_PORT 44

/* RFC2217 command codes */
#define RFC2217_SIGNATURE       0
#define RFC2217_SET_BAUD        1
#define RFC2217_SET_DATASIZE    2
#define RFC2217_SET_PARITY      3
#define RFC2217_SET_STOPSIZE    4
#define RFC2217_SET_CONTROL     5
#define RFC2217_NOTIFY_LINE     6
#define RFC2217_NOTIFY_MODEM    7
#define RFC2217_FLOW_SUSPEND    8
#define RFC2217_FLOW_RESUME     9
#define RFC2217_SET_LINE_MASK   10
#define RFC2217_SET_MODEM_MASK  11
#define RFC2217_PURGE_DATA      12

#define RFC2217_REPLY(cmd) ((cmd) + 100)

struct ser2net_basic_session_state {
    const struct ser2net_serial_if *serial;
    size_t net_buf_size;
    size_t serial_buf_size;
    const char *signature;
    size_t port_count;
    int port_ids[SER2NET_MAX_PORTS];
    uint16_t tcp_ports[SER2NET_MAX_PORTS];
    enum ser2net_port_mode port_modes[SER2NET_MAX_PORTS];
    struct ser2net_serial_params defaults[SER2NET_MAX_PORTS];
    uint32_t idle_timeout_ms[SER2NET_MAX_PORTS];
    size_t next_port_index;
    int active_sessions[SER2NET_MAX_PORTS];
};

struct ser2net_session_ctx {
    ser2net_client_handle_t client;
    ser2net_serial_handle_t serial;
    const struct ser2net_network_if *network_if;
    int port_id;
    uint16_t tcp_port;
    enum ser2net_port_mode mode;
    bool rfc2217_active;
    bool awaiting_option;
    uint8_t pending_option_cmd;
    uint8_t subopt_buf[64];
    size_t subopt_pos;
    bool in_subneg;
    int current_baud;
    int current_datasize;
    int current_parity;
    int current_stopbits;
    int current_flowcontrol;
    uint8_t *net_buf;
    uint8_t *serial_buf;
};

static struct ser2net_basic_session_state g_session_state;
static const char *SESSION_LOG_TAG = "ser2net_session";
static SemaphoreHandle_t g_session_lock;

static int
find_port_index(int port_id)
{
    for (size_t i = 0; i < g_session_state.port_count; ++i) {
        if (g_session_state.port_ids[i] == port_id)
            return (int) i;
    }
    return -1;
}

struct session_init_param {
    struct ser2net_session_ctx **ctx_ptr;
    const struct ser2net_network_if *network;
    int port_id;
};

static BaseType_t
net_write_all(const struct ser2net_network_if *net_if,
              ser2net_client_handle_t client,
              const uint8_t *buf, size_t len, TickType_t timeout_ticks)
{
    /* Pump the TELNET frame to the TCP socket, honouring optional timeouts. */
    size_t written = 0;
    TickType_t start = xTaskGetTickCount();

    while (written < len) {
        int rv = net_if->client_send ? net_if->client_send(net_if->ctx, client,
                                                           buf + written,
                                                           len - written,
                                                           timeout_ticks)
                                     : -1;
        if (rv < 0) {
            return pdFAIL;
        }
        written += (size_t) rv;
        if (timeout_ticks != portMAX_DELAY) {
            TickType_t now = xTaskGetTickCount();
            if ((now - start) >= timeout_ticks)
                break;
        }
        if (rv == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return (written == len) ? pdPASS : pdFAIL;
}

static BaseType_t
send_telnet_cmd(struct ser2net_session_ctx *ctx, uint8_t cmd, uint8_t opt)
{
    uint8_t buf[3] = { TN_IAC, cmd, opt };
    return net_write_all(ctx->network_if, ctx->client, buf, sizeof(buf), pdMS_TO_TICKS(200));
}

static BaseType_t
send_telnet_subneg(struct ser2net_session_ctx *ctx,
                   const uint8_t *data, size_t len)
{
    /* Wrap RFC2217 payload into TELNET IAC SB/SE framing. */
    uint8_t header[3] = { TN_IAC, TN_SB, TN_OPT_COM_PORT };
    uint8_t trailer[2] = { TN_IAC, TN_SE };

    if (net_write_all(ctx->network_if, ctx->client, header, sizeof(header), pdMS_TO_TICKS(200)) != pdPASS)
        return pdFAIL;

    for (size_t i = 0; i < len; ++i) {
        uint8_t byte = data[i];
        if (byte == TN_IAC) {
            if (net_write_all(ctx->network_if, ctx->client, &byte, 1, pdMS_TO_TICKS(200)) != pdPASS)
                return pdFAIL;
        }
        if (net_write_all(ctx->network_if, ctx->client, &byte, 1, pdMS_TO_TICKS(200)) != pdPASS)
            return pdFAIL;
    }

    return net_write_all(ctx->network_if, ctx->client, trailer, sizeof(trailer), pdMS_TO_TICKS(200));
}

static void
session_reset_defaults(struct ser2net_session_ctx *ctx)
{
    struct ser2net_serial_params params = {
        .baud = 115200,
        .data_bits = 8,
        .parity = 0,
        .stop_bits = 1,
        .flow_control = 0
    };

    int idx = find_port_index(ctx->port_id);
    if (idx >= 0 && (size_t) idx < g_session_state.port_count) {
        params = g_session_state.defaults[idx];
    }

    ctx->current_baud = params.baud;
    ctx->current_datasize = params.data_bits;
    ctx->current_parity = params.parity;
    ctx->current_stopbits = params.stop_bits;
    ctx->current_flowcontrol = params.flow_control;
}

static BaseType_t
apply_serial_settings(struct ser2net_session_ctx *ctx)
{
    if (!g_session_state.serial->serial_configure)
        return pdFAIL;

    /* Write pending UART settings back to the hardware driver. */
    ESP_LOGI(SESSION_LOG_TAG,
             "Applying serial settings: baud=%d data_bits=%d parity=%d stop_bits=%d flow=%d",
             ctx->current_baud,
             ctx->current_datasize,
             ctx->current_parity,
             ctx->current_stopbits,
             ctx->current_flowcontrol);

    BaseType_t rv = g_session_state.serial->serial_configure(
        g_session_state.serial->ctx,
        ctx->serial,
        ctx->current_baud,
        ctx->current_datasize,
        ctx->current_parity,
        ctx->current_stopbits,
        ctx->current_flowcontrol);

    if (rv != pdPASS) {
        ESP_LOGE(SESSION_LOG_TAG, "serial_configure() failed");
    } else {
        ESP_LOGI(SESSION_LOG_TAG, "Serial settings applied");
    }

    return rv;
}

static BaseType_t
handle_rfc2217_command(struct ser2net_session_ctx *ctx,
                       const uint8_t *data, size_t len)
{
    /* Decode RFC2217 sub-negotiation payload and update UART state. */
    if (len == 0)
        return pdPASS;

    uint8_t cmd = data[0];
    const uint8_t *payload = data + 1;
    size_t paylen = len - 1;
    uint8_t reply[8];
    BaseType_t rv;

    ESP_LOGI(SESSION_LOG_TAG, "RFC2217 command %u payload_len=%zu", cmd, paylen);

    switch (cmd) {
    case RFC2217_SIGNATURE:
        if (g_session_state.signature) {
            size_t siglen = strlen(g_session_state.signature);
            if (siglen > sizeof(reply) - 2)
                siglen = sizeof(reply) - 2;
            reply[0] = RFC2217_REPLY(RFC2217_SIGNATURE);
            memcpy(&reply[1], g_session_state.signature, siglen);
            return send_telnet_subneg(ctx, reply, siglen + 1);
        }
        break;

    case RFC2217_SET_BAUD:
        if (paylen >= 4) {
            int baud = (payload[0] << 24) | (payload[1] << 16) |
                       (payload[2] << 8) | payload[3];
            ESP_LOGI(SESSION_LOG_TAG, "RFC2217_SET_BAUD -> %d", baud);
            ctx->current_baud = baud;
            rv = apply_serial_settings(ctx);
            if (rv != pdPASS) {
                ESP_LOGE(SESSION_LOG_TAG, "apply_serial_settings failed for baud=%d", baud);
                return pdFAIL;
            }
            reply[0] = RFC2217_REPLY(RFC2217_SET_BAUD);
            reply[1] = payload[0];
            reply[2] = payload[1];
            reply[3] = payload[2];
            reply[4] = payload[3];
            return send_telnet_subneg(ctx, reply, 5);
        }
        break;

    case RFC2217_SET_DATASIZE:
        if (paylen >= 1) {
            int datasize = payload[0];
            ESP_LOGI(SESSION_LOG_TAG, "RFC2217_SET_DATASIZE -> %d", datasize);
            ctx->current_datasize = datasize;
            rv = apply_serial_settings(ctx);
            if (rv != pdPASS) {
                ESP_LOGE(SESSION_LOG_TAG, "apply_serial_settings failed for datasize=%d", datasize);
                return pdFAIL;
            }
            reply[0] = RFC2217_REPLY(RFC2217_SET_DATASIZE);
            reply[1] = payload[0];
            return send_telnet_subneg(ctx, reply, 2);
        }
        break;

    case RFC2217_SET_PARITY:
        if (paylen >= 1) {
            uint8_t rfc_parity = payload[0];
            int parity_value;
            switch (rfc_parity) {
            case 1: parity_value = 0; break; /* none */
            case 2: parity_value = 1; break; /* odd */
            case 3: parity_value = 2; break; /* even */
            default:
                ESP_LOGE(SESSION_LOG_TAG, "Unsupported parity code %u", rfc_parity);
                return pdFAIL;
            }
            ESP_LOGI(SESSION_LOG_TAG, "RFC2217_SET_PARITY -> code=%u (internal=%d)", rfc_parity, parity_value);
            ctx->current_parity = parity_value;
            rv = apply_serial_settings(ctx);
            if (rv != pdPASS) {
                ESP_LOGE(SESSION_LOG_TAG, "apply_serial_settings failed for parity=%u", rfc_parity);
                return pdFAIL;
            }
            reply[0] = RFC2217_REPLY(RFC2217_SET_PARITY);
            reply[1] = payload[0];
            return send_telnet_subneg(ctx, reply, 2);
        }
        break;

    case RFC2217_SET_STOPSIZE:
        if (paylen >= 1) {
            uint8_t rfc_stop = payload[0];
            int stop_internal;
            switch (rfc_stop) {
            case 1: stop_internal = 1; break;
            case 2: stop_internal = 2; break;
            case 3: stop_internal = 15; break; /* 1.5 */
            default:
                ESP_LOGE(SESSION_LOG_TAG, "Unsupported stopsize code %u", rfc_stop);
                return pdFAIL;
            }
            ESP_LOGI(SESSION_LOG_TAG, "RFC2217_SET_STOPSIZE -> code=%u (internal=%d)", rfc_stop, stop_internal);
            ctx->current_stopbits = stop_internal;
            rv = apply_serial_settings(ctx);
            if (rv != pdPASS) {
                ESP_LOGE(SESSION_LOG_TAG, "apply_serial_settings failed for stopsize=%u", rfc_stop);
                return pdFAIL;
            }
            reply[0] = RFC2217_REPLY(RFC2217_SET_STOPSIZE);
            reply[1] = payload[0];
            return send_telnet_subneg(ctx, reply, 2);
        }
        break;

    case RFC2217_FLOW_SUSPEND:
        reply[0] = RFC2217_REPLY(RFC2217_FLOW_SUSPEND);
        return send_telnet_subneg(ctx, reply, 1);

    case RFC2217_FLOW_RESUME:
        reply[0] = RFC2217_REPLY(RFC2217_FLOW_RESUME);
        return send_telnet_subneg(ctx, reply, 1);

    case RFC2217_SET_CONTROL:
        if (paylen >= 1) {
            uint8_t control = payload[0];
            ESP_LOGI(SESSION_LOG_TAG, "RFC2217_SET_CONTROL -> 0x%02x", control);
            reply[0] = RFC2217_REPLY(RFC2217_SET_CONTROL);
            reply[1] = control;
            return send_telnet_subneg(ctx, reply, 2);
        } else {
            ESP_LOGW(SESSION_LOG_TAG, "RFC2217_SET_CONTROL missing payload");
            reply[0] = RFC2217_REPLY(RFC2217_SET_CONTROL);
            reply[1] = 0;
            return send_telnet_subneg(ctx, reply, 2);
        }

    case RFC2217_PURGE_DATA:
        reply[0] = RFC2217_REPLY(RFC2217_PURGE_DATA);
        reply[1] = paylen ? payload[0] : 0;
        return send_telnet_subneg(ctx, reply, 2);

    default:
        ESP_LOGW(SESSION_LOG_TAG, "Unhandled RFC2217 command %u", cmd);
        break;
    }

    return pdPASS;
}

static BaseType_t
process_network_data_telnet(struct ser2net_session_ctx *ctx,
                            uint8_t *buf, size_t len)
{
    /* Feed TELNET framing state machine and forward raw data to UART. */
    uint8_t serial_chunk[64];
    size_t serial_pos = 0;

    for (size_t i = 0; i < len; ++i) {
        uint8_t byte = buf[i];

        if (!ctx->in_subneg && !ctx->awaiting_option && byte == TN_IAC) {
            ctx->awaiting_option = true;
            continue;
        }

        if (ctx->awaiting_option) {
            ctx->awaiting_option = false;

            switch (byte) {
            case TN_IAC:
                serial_chunk[serial_pos++] = TN_IAC;
                break;

            case TN_SB:
                ctx->in_subneg = true;
                ctx->subopt_pos = 0;
                break;

            case TN_SE:
                ctx->in_subneg = false;
                break;

            case TN_WILL:
            case TN_WONT:
            case TN_DO:
            case TN_DONT:
                ctx->pending_option_cmd = byte;
                ctx->awaiting_option = true;
                break;

            default:
                break;
            }
            continue;
        }

        if (ctx->pending_option_cmd) {
            uint8_t cmd = ctx->pending_option_cmd;
            ctx->pending_option_cmd = 0;
            ctx->awaiting_option = false;

            if (byte == TN_OPT_COM_PORT) {
                switch (cmd) {
                case TN_WILL:
                    ctx->rfc2217_active = true;
                    send_telnet_cmd(ctx, TN_DO, TN_OPT_COM_PORT);
                    break;
                case TN_WONT:
                    ctx->rfc2217_active = false;
                    send_telnet_cmd(ctx, TN_DONT, TN_OPT_COM_PORT);
                    break;
                case TN_DO:
                    ctx->rfc2217_active = true;
                    send_telnet_cmd(ctx, TN_WILL, TN_OPT_COM_PORT);
                    break;
                case TN_DONT:
                    ctx->rfc2217_active = false;
                    send_telnet_cmd(ctx, TN_WONT, TN_OPT_COM_PORT);
                    break;
                default:
                    break;
                }
                ESP_LOGI(SESSION_LOG_TAG, "RFC2217 COM_PORT negotiation cmd=%u -> active=%d",
                         cmd, ctx->rfc2217_active);
            } else {
                if (byte == TN_OPT_BINARY) {
                    switch (cmd) {
                    case TN_WILL:
                        send_telnet_cmd(ctx, TN_DO, TN_OPT_BINARY);
                        break;
                    case TN_WONT:
                        send_telnet_cmd(ctx, TN_DONT, TN_OPT_BINARY);
                        break;
                    case TN_DO:
                        send_telnet_cmd(ctx, TN_WILL, TN_OPT_BINARY);
                        break;
                    case TN_DONT:
                        send_telnet_cmd(ctx, TN_WONT, TN_OPT_BINARY);
                        break;
                    default:
                        break;
                    }
                    ESP_LOGI(SESSION_LOG_TAG, "Telnet BINARY negotiation cmd=%u", cmd);
                } else {
                    uint8_t resp = (cmd == TN_WILL || cmd == TN_WONT) ? TN_DONT : TN_WONT;
                    send_telnet_cmd(ctx, resp, byte);
                }
            }
            continue;
        }

        if (ctx->in_subneg) {
            if (ctx->subopt_pos < sizeof(ctx->subopt_buf)) {
                if (ctx->subopt_pos == 0) {
                    ESP_LOGI(SESSION_LOG_TAG,
                             "Subnegotiation start: first byte=0x%02x",
                             byte);
                }
                ctx->subopt_buf[ctx->subopt_pos++] = byte;
            }
            if (ctx->subopt_pos > 1 &&
                ctx->subopt_buf[ctx->subopt_pos - 2] == TN_IAC &&
                ctx->subopt_buf[ctx->subopt_pos - 1] == TN_SE) {
                ctx->in_subneg = false;
                if (ctx->subopt_pos >= 3) {
                    size_t data_len = ctx->subopt_pos - 2; /* drop trailing IAC SE */
                    const uint8_t *data = ctx->subopt_buf;
                    ESP_LOGI(SESSION_LOG_TAG,
                             "Subnegotiation complete: len=%zu first_bytes=%02x %02x %02x",
                             ctx->subopt_pos,
                             ctx->subopt_buf[0],
                             ctx->subopt_pos > 1 ? ctx->subopt_buf[1] : 0,
                             ctx->subopt_pos > 2 ? ctx->subopt_buf[2] : 0);
                    ESP_LOG_BUFFER_HEXDUMP(SESSION_LOG_TAG,
                                           ctx->subopt_buf,
                                           ctx->subopt_pos,
                                           ESP_LOG_INFO);
                    if (data_len > 0) {
                        if (data[0] == TN_OPT_COM_PORT) {
                            data++;
                            data_len--;
                        } else {
                            ESP_LOGW(SESSION_LOG_TAG,
                                     "Missing COM_PORT option prefix (first=0x%02x)",
                                     data[0]);
                        }
                        if (data_len > 0) {
                            if (handle_rfc2217_command(ctx, data, data_len) != pdPASS) {
                                ESP_LOGW(SESSION_LOG_TAG, "RFC2217 command handling failed");
                            }
                        }
                    }
                } else {
                    ESP_LOGW(SESSION_LOG_TAG, "Subnegotiation too short (%zu bytes)", ctx->subopt_pos);
                }
                ctx->subopt_pos = 0;
            }
            continue;
        }

        if (serial_pos < sizeof(serial_chunk))
            serial_chunk[serial_pos++] = byte;

        if (serial_pos == sizeof(serial_chunk)) {
            if (ctx->tcp_port)
                ser2net_control_monitor_feed(ctx->tcp_port,
                                             SER2NET_MONITOR_STREAM_TCP,
                                             serial_chunk,
                                             serial_pos);
            if (!g_session_state.serial->serial_write ||
                g_session_state.serial->serial_write(
                    g_session_state.serial->ctx, ctx->serial,
                    serial_chunk, serial_pos) < 0)
                return pdFAIL;
            serial_pos = 0;
        }
    }

    if (serial_pos > 0) {
        if (ctx->tcp_port)
            ser2net_control_monitor_feed(ctx->tcp_port,
                                         SER2NET_MONITOR_STREAM_TCP,
                                         serial_chunk,
                                         serial_pos);
        if (!g_session_state.serial->serial_write ||
            g_session_state.serial->serial_write(
                g_session_state.serial->ctx, ctx->serial,
                serial_chunk, serial_pos) < 0)
            return pdFAIL;
    }

    return pdPASS;
}

static BaseType_t
flush_serial_to_net(struct ser2net_session_ctx *ctx)
{
    if (!g_session_state.serial->serial_read ||
        !ctx->network_if || !ctx->network_if->client_send)
        return pdPASS;

    if (ctx->mode == SER2NET_PORT_MODE_RAWLP)
        return pdPASS;

    /* Pull from UART into TCP buffer, escaping TELNET (IAC) bytes on the fly. */
    int rv = g_session_state.serial->serial_read(
        g_session_state.serial->ctx, ctx->serial,
        ctx->serial_buf, g_session_state.serial_buf_size, 0);

    if (rv < 0)
        return pdFAIL;

    if (rv == 0)
        return pdPASS;

    if (ctx->tcp_port)
        ser2net_control_monitor_feed(ctx->tcp_port,
                                     SER2NET_MONITOR_STREAM_TERM,
                                     ctx->serial_buf,
                                     (size_t) rv);

    if (ctx->mode != SER2NET_PORT_MODE_TELNET) {
        return net_write_all(ctx->network_if, ctx->client,
                             ctx->serial_buf, (size_t) rv, pdMS_TO_TICKS(50));
    }

    size_t out_len = 0;
    for (int i = 0; i < rv && out_len < g_session_state.net_buf_size - 1; ++i) {
        ctx->net_buf[out_len++] = ctx->serial_buf[i];
        if (ctx->serial_buf[i] == TN_IAC && out_len < g_session_state.net_buf_size - 1) {
            ctx->net_buf[out_len++] = TN_IAC;
        }
    }

    return net_write_all(ctx->network_if, ctx->client,
                         ctx->net_buf, out_len, pdMS_TO_TICKS(50));
}

static BaseType_t
basic_initialise(void *ctx, ser2net_client_handle_t client,
                 ser2net_serial_handle_t serial)
{
    /* Create and prime per-session state (buffers, defaults, TELNET preface). */
    struct session_init_param *param = ctx;
    struct ser2net_session_ctx **ctx_ptr = param ? param->ctx_ptr : NULL;
    struct ser2net_session_ctx *sctx;

    if (!ctx_ptr || !param || !param->network)
        return pdFAIL;

    sctx = pvPortMalloc(sizeof(*sctx));
    if (!sctx)
        return pdFAIL;

    memset(sctx, 0, sizeof(*sctx));
    sctx->client = client;
    sctx->serial = serial;
    sctx->network_if = param ? param->network : NULL;
    sctx->port_id = param ? param->port_id : 0;
    sctx->net_buf = pvPortMalloc(g_session_state.net_buf_size);
    sctx->serial_buf = pvPortMalloc(g_session_state.serial_buf_size);
    if (!sctx->net_buf || !sctx->serial_buf) {
        if (sctx->net_buf)
            vPortFree(sctx->net_buf);
        if (sctx->serial_buf)
            vPortFree(sctx->serial_buf);
        vPortFree(sctx);
        return pdFAIL;
    }

    session_reset_defaults(sctx);
    apply_serial_settings(sctx);
    *ctx_ptr = sctx;

    int pidx = find_port_index(sctx->port_id);
    if (pidx >= 0 && (size_t)pidx < ARRAY_LEN(g_session_state.active_sessions))
        g_session_state.active_sessions[pidx]++;
    if (pidx >= 0 && (size_t)pidx < ARRAY_LEN(g_session_state.tcp_ports)) {
        sctx->tcp_port = g_session_state.tcp_ports[pidx];
        sctx->mode = g_session_state.port_modes[pidx];
    } else {
        sctx->tcp_port = 0;
        sctx->mode = SER2NET_PORT_MODE_TELNET;
    }

    if (sctx->mode == SER2NET_PORT_MODE_TELNET) {
        send_telnet_cmd(sctx, TN_WILL, TN_OPT_BINARY);
        send_telnet_cmd(sctx, TN_DO, TN_OPT_BINARY);
        send_telnet_cmd(sctx, TN_WILL, TN_OPT_COM_PORT);
        send_telnet_cmd(sctx, TN_DO, TN_OPT_COM_PORT);
    } else {
        sctx->rfc2217_active = false;
    }

    return pdPASS;
}

static BaseType_t
basic_process_io(void *ctx, ser2net_client_handle_t client,
                 ser2net_serial_handle_t serial, TickType_t block_ticks)
{
    /* Poll TCP and UART sides in turn until the session requests teardown. */
    (void) client;
    (void) serial;

    struct ser2net_session_ctx **ctx_ptr = ctx;
    if (!ctx_ptr || !*ctx_ptr)
        return pdFAIL;
    struct ser2net_session_ctx *sctx = *ctx_ptr;

    if (sctx->network_if && sctx->network_if->client_recv) {
        int rv = sctx->network_if->client_recv(
            sctx->network_if->ctx,
            sctx->client,
            sctx->net_buf,
            g_session_state.net_buf_size,
            block_ticks);
        if (rv < 0) {
            if (rv == -2)
                return pdFAIL;
        } else if (rv > 0) {
            if (sctx->mode == SER2NET_PORT_MODE_TELNET) {
                ESP_LOGI(SESSION_LOG_TAG, "Net RX %d bytes", rv);
                ESP_LOG_BUFFER_HEXDUMP(SESSION_LOG_TAG, sctx->net_buf, (size_t) rv, ESP_LOG_INFO);
                if (process_network_data_telnet(sctx, sctx->net_buf, (size_t) rv) != pdPASS)
                    return pdFAIL;
            } else {
                if (sctx->tcp_port)
                    ser2net_control_monitor_feed(sctx->tcp_port,
                                                 SER2NET_MONITOR_STREAM_TCP,
                                                 sctx->net_buf,
                                                 (size_t) rv);
                if (!g_session_state.serial->serial_write)
                    return pdFAIL;
                size_t remaining = (size_t) rv;
                const uint8_t *ptr = sctx->net_buf;
                while (remaining > 0) {
                    int written = g_session_state.serial->serial_write(
                        g_session_state.serial->ctx, sctx->serial,
                        ptr, remaining);
                    if (written <= 0)
                        return pdFAIL;
                    ptr += written;
                    remaining -= (size_t) written;
                }
            }
        }
    }

    if (g_session_state.serial->serial_read) {
        if (flush_serial_to_net(sctx) != pdPASS)
            return pdFAIL;
    }

    return pdPASS;
}

static void
basic_handle_disconnect(void *ctx, ser2net_client_handle_t client,
                        ser2net_serial_handle_t serial)
{
    (void) client;
    (void) serial;

    struct ser2net_session_ctx **ctx_ptr = ctx;
    if (!ctx_ptr || !*ctx_ptr)
        return;

    struct ser2net_session_ctx *sctx = *ctx_ptr;

    int pidx = find_port_index(sctx->port_id);
    if (pidx >= 0 && (size_t)pidx < ARRAY_LEN(g_session_state.active_sessions) && g_session_state.active_sessions[pidx] > 0)
        g_session_state.active_sessions[pidx]--;

    if (sctx->net_buf)
        vPortFree(sctx->net_buf);
    if (sctx->serial_buf)
        vPortFree(sctx->serial_buf);

    vPortFree(sctx);
    *ctx_ptr = NULL;
}

static const struct ser2net_session_ops g_basic_session_ops = {
    .ctx = &g_session_state,
    .initialise = basic_initialise,
    .process_io = basic_process_io,
    .handle_disconnect = basic_handle_disconnect
};

const struct ser2net_session_ops *
ser2net_basic_session_ops_init(const struct ser2net_network_if *network,
                               const struct ser2net_serial_if *serial,
                               const struct ser2net_basic_session_cfg *cfg)
{
    (void) network;
    memset(&g_session_state, 0, sizeof(g_session_state));
    g_session_state.serial = serial;
    g_session_state.signature = "ser2net";
    g_session_state.net_buf_size = cfg && cfg->net_buf_size ? cfg->net_buf_size : 256;
    g_session_state.serial_buf_size = cfg && cfg->serial_buf_size ? cfg->serial_buf_size : 256;
    if (!g_session_lock)
        g_session_lock = xSemaphoreCreateMutex();

    struct ser2net_serial_params fallback = {
        .baud = 115200,
        .data_bits = 8,
        .parity = 0,
        .stop_bits = 1,
        .flow_control = 0
    };
    if (cfg && cfg->port_count > 0) {
        g_session_state.port_count = cfg->port_count > SER2NET_MAX_PORTS ? SER2NET_MAX_PORTS : cfg->port_count;
        for (size_t i = 0; i < g_session_state.port_count; ++i) {
            g_session_state.port_ids[i] = cfg->port_ids[i];
            g_session_state.tcp_ports[i] = cfg->tcp_ports[i];
            g_session_state.port_modes[i] = cfg->port_modes[i];
            g_session_state.defaults[i] = cfg->port_params[i];
            g_session_state.idle_timeout_ms[i] = cfg->idle_timeout_ms[i];
        }
    } else {
        g_session_state.port_count = 0;
        g_session_state.port_ids[0] = 0;
        g_session_state.tcp_ports[0] = 0;
        g_session_state.port_modes[0] = SER2NET_PORT_MODE_TELNET;
        g_session_state.defaults[0] = fallback;
        g_session_state.idle_timeout_ms[0] = 0;
    }
    g_session_state.next_port_index = 0;
    memset(g_session_state.active_sessions, 0, sizeof(g_session_state.active_sessions));
    return &g_basic_session_ops;
}

int
ser2net_basic_next_port(void)
{
    if (g_session_state.port_count == 0)
        return 0;
    size_t idx = g_session_state.next_port_index++ % g_session_state.port_count;
    return g_session_state.port_ids[idx];
}

size_t
ser2net_get_port_stats(int *port_ids, int *active_sessions, size_t max_entries)
{
    size_t count = g_session_state.port_count;
    if (count > max_entries)
        count = max_entries;
    for (size_t i = 0; i < count; ++i) {
        if (port_ids)
            port_ids[i] = g_session_state.port_ids[i];
        if (active_sessions)
            active_sessions[i] = g_session_state.active_sessions[i];
    }
    return count;
}

BaseType_t
ser2net_session_update_defaults(int port_id,
                                const struct ser2net_serial_params *params,
                                uint32_t idle_timeout_ms)
{
    if (!params)
        return pdFAIL;

    if (!g_session_lock || xSemaphoreTake(g_session_lock, portMAX_DELAY) != pdPASS)
        return pdFAIL;

    int idx = find_port_index(port_id);
    if (idx < 0 || (size_t) idx >= g_session_state.port_count) {
        xSemaphoreGive(g_session_lock);
        return pdFAIL;
    }

    g_session_state.defaults[idx] = *params;
    g_session_state.idle_timeout_ms[idx] = idle_timeout_ms;

    xSemaphoreGive(g_session_lock);
    return pdPASS;
}

BaseType_t
ser2net_session_apply_config(void *session_ctx,
                             const struct ser2net_serial_params *params)
{
    if (!session_ctx || !params)
        return pdFAIL;

    struct ser2net_session_ctx *ctx = session_ctx;
    ctx->current_baud = params->baud;
    ctx->current_datasize = params->data_bits;
    ctx->current_parity = params->parity;
    ctx->current_stopbits = params->stop_bits;
    ctx->current_flowcontrol = params->flow_control;

    return apply_serial_settings(ctx);
}

BaseType_t
ser2net_session_set_mode(int port_id, enum ser2net_port_mode mode)
{
    if (!g_session_lock)
        g_session_lock = xSemaphoreCreateMutex();

    if (!g_session_lock || xSemaphoreTake(g_session_lock, portMAX_DELAY) != pdPASS)
        return pdFAIL;

    int idx = find_port_index(port_id);
    if (idx < 0 || (size_t)idx >= g_session_state.port_count) {
        xSemaphoreGive(g_session_lock);
        return pdFAIL;
    }

    g_session_state.port_modes[idx] = mode;

    xSemaphoreGive(g_session_lock);
    return pdPASS;
}

BaseType_t
ser2net_session_register_port(int port_id,
                              uint16_t tcp_port,
                              enum ser2net_port_mode mode,
                              const struct ser2net_serial_params *defaults,
                              uint32_t idle_timeout_ms)
{
    if (!defaults)
        return pdFAIL;

    if (!g_session_lock)
        g_session_lock = xSemaphoreCreateMutex();

    if (!g_session_lock || xSemaphoreTake(g_session_lock, portMAX_DELAY) != pdPASS)
        return pdFAIL;

    if (g_session_state.port_count >= SER2NET_MAX_PORTS) {
        xSemaphoreGive(g_session_lock);
        return pdFAIL;
    }

    for (size_t i = 0; i < g_session_state.port_count; ++i) {
        if (g_session_state.port_ids[i] == port_id ||
            g_session_state.tcp_ports[i] == tcp_port) {
            xSemaphoreGive(g_session_lock);
            return pdFAIL;
        }
    }

    size_t idx = g_session_state.port_count;
    g_session_state.port_ids[idx] = port_id;
    g_session_state.tcp_ports[idx] = tcp_port;
    g_session_state.port_modes[idx] = mode;
    g_session_state.defaults[idx] = *defaults;
    g_session_state.idle_timeout_ms[idx] = idle_timeout_ms;
    g_session_state.active_sessions[idx] = 0;
    g_session_state.port_count++;

    xSemaphoreGive(g_session_lock);
    return pdPASS;
}

BaseType_t
ser2net_session_unregister_port(int port_id)
{
    if (!g_session_lock)
        g_session_lock = xSemaphoreCreateMutex();

    if (!g_session_lock || xSemaphoreTake(g_session_lock, portMAX_DELAY) != pdPASS)
        return pdFAIL;

    int idx = find_port_index(port_id);
    if (idx < 0) {
        xSemaphoreGive(g_session_lock);
        return pdFAIL;
    }

    if (g_session_state.active_sessions[idx] > 0) {
        xSemaphoreGive(g_session_lock);
        return pdFAIL;
    }

    for (size_t i = (size_t)idx; i + 1 < g_session_state.port_count; ++i) {
        g_session_state.port_ids[i] = g_session_state.port_ids[i + 1];
        g_session_state.tcp_ports[i] = g_session_state.tcp_ports[i + 1];
        g_session_state.port_modes[i] = g_session_state.port_modes[i + 1];
        g_session_state.defaults[i] = g_session_state.defaults[i + 1];
        g_session_state.idle_timeout_ms[i] = g_session_state.idle_timeout_ms[i + 1];
        g_session_state.active_sessions[i] = g_session_state.active_sessions[i + 1];
    }

    if (g_session_state.port_count > 0) {
        size_t last = g_session_state.port_count - 1;
        g_session_state.port_ids[last] = 0;
        g_session_state.tcp_ports[last] = 0;
        g_session_state.port_modes[last] = SER2NET_PORT_MODE_TELNET;
        g_session_state.defaults[last] = (struct ser2net_serial_params){0};
        g_session_state.idle_timeout_ms[last] = 0;
        g_session_state.active_sessions[last] = 0;
        g_session_state.port_count = last;
    }

    g_session_state.next_port_index = 0;

    xSemaphoreGive(g_session_lock);
    return pdPASS;
}
