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
 * JSON configuration loader (cJSON backed)
 * ---------------------------------------
 * Translates the embedded JSON string into runtime / adapter structures.
 * Each serial entry defines a UART as well as its dedicated TCP listener.
 * The optional control block enables the text-based diagnostics port.
 */

#include "json_config.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "cJSON.h"
#include "esp_log.h"
#include "control_port.h"
#include "session_ops.h"

static char json_error_msg[128];

static void
set_json_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(json_error_msg, sizeof(json_error_msg), fmt, ap);
    va_end(ap);
}

const char *
ser2net_json_last_error(void)
{
    return json_error_msg[0] ? json_error_msg : NULL;
}

static BaseType_t
parse_sessions(const cJSON *obj, struct ser2net_runtime_config *cfg)
{
    if (!obj)
        return pdPASS;

    const cJSON *max = cJSON_GetObjectItemCaseSensitive(obj, "max");
    if (max) {
        if (!cJSON_IsNumber(max) || max->valueint <= 0) {
            set_json_error("sessions.max invalid");
            return pdFAIL;
        }
        cfg->max_sessions = (size_t) max->valueint;
    }

    const cJSON *stack = cJSON_GetObjectItemCaseSensitive(obj, "stack_words");
    if (stack) {
        if (!cJSON_IsNumber(stack) || stack->valueint < 512) {
            set_json_error("sessions.stack_words invalid");
            return pdFAIL;
        }
        cfg->session_task_stack_words = (uint16_t) stack->valueint;
    }

    const cJSON *prio = cJSON_GetObjectItemCaseSensitive(obj, "priority");
    if (prio) {
        if (!cJSON_IsNumber(prio) || prio->valueint < 0) {
            set_json_error("sessions.priority invalid");
            return pdFAIL;
        }
        cfg->session_task_priority = (UBaseType_t) prio->valueint;
    }

    return pdPASS;
}

static BaseType_t
parse_buffers(const cJSON *obj, struct ser2net_basic_session_cfg *cfg)
{
    if (!obj)
        return pdPASS;

    const cJSON *net = cJSON_GetObjectItemCaseSensitive(obj, "net");
    if (net) {
        if (!cJSON_IsNumber(net) || net->valueint < 64) {
            set_json_error("buffers.net invalid");
            return pdFAIL;
        }
        cfg->net_buf_size = (size_t) net->valueint;
    }

    const cJSON *serial = cJSON_GetObjectItemCaseSensitive(obj, "serial");
    if (serial) {
        if (!cJSON_IsNumber(serial) || serial->valueint < 64) {
            set_json_error("buffers.serial invalid");
            return pdFAIL;
        }
        cfg->serial_buf_size = (size_t) serial->valueint;
    }

    return pdPASS;
}

static int
parse_uart_string(const char *str)
{
    if (!str)
        return -1;
    char tmp[16];
    size_t len = strlen(str);
    if (len >= sizeof(tmp))
        len = sizeof(tmp) - 1;
    for (size_t i = 0; i < len; ++i)
        tmp[i] = (char) tolower((unsigned char) str[i]);
    tmp[len] = '\0';

    if (strncmp(tmp, "uart", 4) == 0)
        return atoi(tmp + 4);
    return atoi(tmp);
}

static BaseType_t
parse_serial_item(const cJSON *item,
                  struct ser2net_esp32_serial_port_cfg *port,
                  int index)
{
    /* Extract UART + TCP listener information for one serial block. */
    if (!cJSON_IsObject(item)) {
        set_json_error("serial[%d] invalid object", index);
        return pdFAIL;
    }

    memset(port, 0, sizeof(*port));
    port->baud_rate = 115200;
    port->data_bits = UART_DATA_8_BITS;
    port->parity = UART_PARITY_DISABLE;
    port->stop_bits = UART_STOP_BITS_1;
    port->flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    port->tcp_port = 0;
    port->tcp_backlog = 4;
    port->rts_pin = UART_PIN_NO_CHANGE;
    port->cts_pin = UART_PIN_NO_CHANGE;
    port->mode = SER2NET_PORT_MODE_TELNET;
    port->idle_timeout_ms = 0;
    port->enabled = true;

    bool has_uart = false, has_tx = false, has_rx = false;

    port->port_id = index;
    const cJSON *port_id = cJSON_GetObjectItemCaseSensitive(item, "port_id");
    if (cJSON_IsNumber(port_id))
        port->port_id = port_id->valueint;

    const cJSON *uart = cJSON_GetObjectItemCaseSensitive(item, "uart");
    if (uart) {
        if (cJSON_IsNumber(uart)) {
            port->uart_num = (uart_port_t) uart->valueint;
            has_uart = true;
        } else if (cJSON_IsString(uart) && uart->valuestring) {
            int parsed = parse_uart_string(uart->valuestring);
            if (parsed >= 0) {
                port->uart_num = (uart_port_t) parsed;
                has_uart = true;
            }
        }
    }

    const cJSON *tx = cJSON_GetObjectItemCaseSensitive(item, "tx_pin");
    const cJSON *rx = cJSON_GetObjectItemCaseSensitive(item, "rx_pin");
    if (cJSON_IsNumber(tx)) {
        port->tx_pin = tx->valueint;
        has_tx = true;
    }
    if (cJSON_IsNumber(rx)) {
        port->rx_pin = rx->valueint;
        has_rx = true;
    }

    const cJSON *rts = cJSON_GetObjectItemCaseSensitive(item, "rts_pin");
    if (cJSON_IsNumber(rts))
        port->rts_pin = rts->valueint;
    const cJSON *cts = cJSON_GetObjectItemCaseSensitive(item, "cts_pin");
    if (cJSON_IsNumber(cts))
        port->cts_pin = cts->valueint;

    const cJSON *baud = cJSON_GetObjectItemCaseSensitive(item, "baud");
    if (cJSON_IsNumber(baud) && baud->valuedouble > 0)
        port->baud_rate = (int) baud->valuedouble;

    const cJSON *db = cJSON_GetObjectItemCaseSensitive(item, "data_bits");
    if (cJSON_IsNumber(db)) {
        switch (db->valueint) {
        case 5: port->data_bits = UART_DATA_5_BITS; break;
        case 6: port->data_bits = UART_DATA_6_BITS; break;
        case 7: port->data_bits = UART_DATA_7_BITS; break;
        case 8: port->data_bits = UART_DATA_8_BITS; break;
        default:
            set_json_error("serial[%d].data_bits unsupported", index);
            return pdFAIL;
        }
    }

    const cJSON *parity = cJSON_GetObjectItemCaseSensitive(item, "parity");
    if (cJSON_IsString(parity) && parity->valuestring) {
        if (strcasecmp(parity->valuestring, "odd") == 0)
            port->parity = UART_PARITY_ODD;
        else if (strcasecmp(parity->valuestring, "even") == 0)
            port->parity = UART_PARITY_EVEN;
        else
            port->parity = UART_PARITY_DISABLE;
    }

    const cJSON *stop = cJSON_GetObjectItemCaseSensitive(item, "stop_bits");
    if (cJSON_IsNumber(stop)) {
        double val = stop->valuedouble;
        if (val >= 2.0)
            port->stop_bits = UART_STOP_BITS_2;
#ifdef UART_STOP_BITS_1_5
        else if (val > 1.0 && val < 2.0)
            port->stop_bits = UART_STOP_BITS_1_5;
#endif
        else
            port->stop_bits = UART_STOP_BITS_1;
    }

    const cJSON *flow = cJSON_GetObjectItemCaseSensitive(item, "flow_control");
    if (cJSON_IsString(flow) && flow->valuestring) {
        if (strcasecmp(flow->valuestring, "rtscts") == 0)
            port->flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS;
        else
            port->flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    }

    if (!has_uart) {
        set_json_error("serial[%d].uart invalid", index);
        return pdFAIL;
    }

    if (!has_tx || !has_rx) {
        set_json_error("serial[%d].tx/rx pins invalid", index);
        return pdFAIL;
    }

    const cJSON *tcp_port = cJSON_GetObjectItemCaseSensitive(item, "tcp_port");
    if (tcp_port) {
        if (!cJSON_IsNumber(tcp_port) || tcp_port->valueint <= 0 || tcp_port->valueint > 65535) {
            set_json_error("serial[%d].tcp_port invalid", index);
            return pdFAIL;
        }
        port->tcp_port = (uint16_t) tcp_port->valueint;
    }

    const cJSON *tcp_backlog = cJSON_GetObjectItemCaseSensitive(item, "tcp_backlog");
    if (tcp_backlog) {
        if (!cJSON_IsNumber(tcp_backlog) || tcp_backlog->valueint < 0) {
            set_json_error("serial[%d].tcp_backlog invalid", index);
            return pdFAIL;
        }
        port->tcp_backlog = tcp_backlog->valueint;
    }

    if (port->tcp_port == 0) {
        set_json_error("serial[%d].tcp_port missing", index);
        return pdFAIL;
    }

    if (port->tcp_backlog <= 0)
        port->tcp_backlog = 4;

    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(item, "mode");
    if (mode) {
        if (!cJSON_IsString(mode) || !mode->valuestring) {
            set_json_error("serial[%d].mode invalid", index);
            return pdFAIL;
        }
        if (strcasecmp(mode->valuestring, "telnet") == 0) {
            port->mode = SER2NET_PORT_MODE_TELNET;
        } else if (strcasecmp(mode->valuestring, "raw") == 0) {
            port->mode = SER2NET_PORT_MODE_RAW;
        } else if (strcasecmp(mode->valuestring, "rawlp") == 0) {
            port->mode = SER2NET_PORT_MODE_RAWLP;
        } else {
            set_json_error("serial[%d].mode must be telnet, raw or rawlp", index);
            return pdFAIL;
        }
    }

    const cJSON *timeout = cJSON_GetObjectItemCaseSensitive(item, "idle_timeout_ms");
    if (timeout) {
        if (!cJSON_IsNumber(timeout) || timeout->valuedouble < 0) {
            set_json_error("serial[%d].idle_timeout_ms invalid", index);
            return pdFAIL;
        }
        port->idle_timeout_ms = (uint32_t) timeout->valuedouble;
    }

    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(item, "enabled");
    if (enabled) {
        if (!cJSON_IsBool(enabled)) {
            set_json_error("serial[%d].enabled must be boolean", index);
            return pdFAIL;
        }
        port->enabled = cJSON_IsTrue(enabled);
    }
    return pdPASS;
}

BaseType_t
ser2net_load_config_json_esp32(const char *json,
        struct ser2net_app_config *app_cfg,
        struct ser2net_esp32_network_cfg *net_cfg,
        struct ser2net_esp32_serial_cfg *serial_cfg,
        struct ser2net_esp32_serial_port_cfg *ports,
        size_t max_ports)
{
    /* High level entry: parse JSON string and populate runtime config. */
    json_error_msg[0] = '\0';

    if (!json || !app_cfg || !net_cfg || !serial_cfg || !ports || max_ports == 0) {
        set_json_error("invalid arguments");
        return pdFAIL;
    }

    ser2net_runtime_config_init(&app_cfg->runtime_cfg);
    ser2net_session_config_init(&app_cfg->session_cfg);
    serial_cfg->ports = ports;
    serial_cfg->num_ports = 0;
    serial_cfg->rx_buffer_size = 512;
    serial_cfg->tx_buffer_size = 512;
    net_cfg->listen_port = 0;
    net_cfg->backlog = 4;

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        set_json_error("JSON parse error");
        return pdFAIL;
    }

    if (!cJSON_IsObject(root)) {
        set_json_error("Top-level JSON object expected");
        cJSON_Delete(root);
        return pdFAIL;
    }

    if (parse_sessions(cJSON_GetObjectItemCaseSensitive(root, "sessions"), &app_cfg->runtime_cfg) != pdPASS) {
        cJSON_Delete(root);
        return pdFAIL;
    }

    if (parse_buffers(cJSON_GetObjectItemCaseSensitive(root, "buffers"), &app_cfg->session_cfg) != pdPASS) {
        cJSON_Delete(root);
        return pdFAIL;
    }

    const cJSON *serial = cJSON_GetObjectItemCaseSensitive(root, "serial");
    if (serial && !cJSON_IsArray(serial)) {
        set_json_error("serial field must be an array when present");
        cJSON_Delete(root);
        return pdFAIL;
    }

    size_t idx = 0;
    if (serial) {
        const cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, serial) {
            if (idx >= max_ports)
                break;
            if (parse_serial_item(entry, &ports[idx], (int)idx) != pdPASS) {
                cJSON_Delete(root);
                return pdFAIL;
            }
            ++idx;
        }
    }
    serial_cfg->num_ports = idx;

    app_cfg->network_if = NULL;
    app_cfg->serial_if = ser2net_esp32_get_serial_if(serial_cfg);

    size_t count = serial_cfg->num_ports;
    if (count > SER2NET_MAX_PORTS)
        count = SER2NET_MAX_PORTS;
    app_cfg->session_cfg.port_count = count;
    for (size_t i = 0; i < count; ++i) {
        const struct ser2net_esp32_serial_port_cfg *p = &serial_cfg->ports[i];
        app_cfg->session_cfg.port_ids[i] = p->port_id;
        app_cfg->session_cfg.tcp_ports[i] = p->tcp_port;
        app_cfg->session_cfg.port_modes[i] = p->mode;
        app_cfg->session_cfg.port_params[i].baud = p->baud_rate;
        switch (p->data_bits) {
        case UART_DATA_5_BITS: app_cfg->session_cfg.port_params[i].data_bits = 5; break;
        case UART_DATA_6_BITS: app_cfg->session_cfg.port_params[i].data_bits = 6; break;
        case UART_DATA_7_BITS: app_cfg->session_cfg.port_params[i].data_bits = 7; break;
        default: app_cfg->session_cfg.port_params[i].data_bits = 8; break;
        }
        app_cfg->session_cfg.port_params[i].parity = p->parity == UART_PARITY_DISABLE ? 0 :
                                                     (p->parity == UART_PARITY_ODD ? 1 : 2);
        switch (p->stop_bits) {
        case UART_STOP_BITS_1: app_cfg->session_cfg.port_params[i].stop_bits = 1; break;
#ifdef UART_STOP_BITS_1_5
        case UART_STOP_BITS_1_5: app_cfg->session_cfg.port_params[i].stop_bits = 15; break;
#endif
        case UART_STOP_BITS_2: app_cfg->session_cfg.port_params[i].stop_bits = 2; break;
        default: app_cfg->session_cfg.port_params[i].stop_bits = 1; break;
        }
        app_cfg->session_cfg.port_params[i].flow_control =
            (p->flow_ctrl == UART_HW_FLOWCTRL_CTS_RTS) ? 1 : 0;
        app_cfg->session_cfg.idle_timeout_ms[i] = p->idle_timeout_ms;
    }

    app_cfg->runtime_cfg.listener_count = 0;
    for (size_t i = 0; i < serial_cfg->num_ports && i < SER2NET_MAX_PORTS; ++i) {
        struct ser2net_esp32_network_cfg listener_cfg = {
            .listen_port = serial_cfg->ports[i].tcp_port,
            .backlog = serial_cfg->ports[i].tcp_backlog <= 0 ? 4 : serial_cfg->ports[i].tcp_backlog
        };
        const struct ser2net_network_if *net_if = ser2net_esp32_get_network_if(&listener_cfg);
        if (!net_if) {
            set_json_error("failed to allocate listener for port_id %d", serial_cfg->ports[i].port_id);
            cJSON_Delete(root);
            return pdFAIL;
        }
        app_cfg->runtime_cfg.listeners[app_cfg->runtime_cfg.listener_count].port_id = serial_cfg->ports[i].port_id;
        app_cfg->runtime_cfg.listeners[app_cfg->runtime_cfg.listener_count].tcp_port = serial_cfg->ports[i].tcp_port;
        app_cfg->runtime_cfg.listeners[app_cfg->runtime_cfg.listener_count].network = net_if;
        app_cfg->runtime_cfg.listener_count++;
        if (i == 0)
            app_cfg->network_if = net_if;
    }

    app_cfg->runtime_cfg.control_enabled = false;
    const cJSON *control = cJSON_GetObjectItemCaseSensitive(root, "control");
    if (control) {
        const cJSON *tcp_port = cJSON_GetObjectItemCaseSensitive(control, "tcp_port");
        if (!cJSON_IsNumber(tcp_port) || tcp_port->valueint <= 0 || tcp_port->valueint > 65535) {
            set_json_error("control.tcp_port invalid");
            cJSON_Delete(root);
            return pdFAIL;
        }
        app_cfg->runtime_cfg.control_enabled = true;
        app_cfg->runtime_cfg.control_ctx.tcp_port = (uint16_t) tcp_port->valueint;
        const cJSON *backlog = cJSON_GetObjectItemCaseSensitive(control, "backlog");
        if (backlog) {
            if (!cJSON_IsNumber(backlog) || backlog->valueint < 0) {
                set_json_error("control.backlog invalid");
                cJSON_Delete(root);
                return pdFAIL;
            }
            app_cfg->runtime_cfg.control_ctx.backlog = backlog->valueint;
        } else {
            app_cfg->runtime_cfg.control_ctx.backlog = 2;
        }
        app_cfg->runtime_cfg.control_ctx.ports = serial_cfg->ports;
        app_cfg->runtime_cfg.control_ctx.port_count = serial_cfg->num_ports;
        app_cfg->runtime_cfg.control_ctx.version = "ser2net-mcu";
    }

    cJSON_Delete(root);
    return pdPASS;
}
