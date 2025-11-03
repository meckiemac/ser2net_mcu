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
 * Lightweight control shell
 * -------------------------
 * Runs alongside the RFC2217 listeners to provide a tiny TCP-based
 * diagnostic console (commands: help/version/showport/monitor/quit).
 * Useful for remote health checks without attaching a serial monitor.
 */

#include "control_port.h"
#include "session_ops.h"
#include "adapters.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>
#include <limits.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define CONTROL_TAG "ser2net_control"
#define CTRL_MAX_LINE 128
#define DEFAULT_VERSION "ser2net-mcu"

static struct ser2net_control_context control_context;
static TaskHandle_t control_task_handle = NULL;
static int control_listen_fd = -1;
static bool control_running = false;

#define MONITOR_QUEUE_DEPTH 16
#define MONITOR_CHUNK       64

struct monitor_message {
    enum ser2net_monitor_stream stream;
    uint16_t tcp_port;
    size_t len;
    uint8_t data[MONITOR_CHUNK];
};

static QueueHandle_t monitor_queue = NULL;

enum monitor_type {
    MONITOR_NONE = 0,
    MONITOR_TCP,
    MONITOR_TERM
};

static struct {
    enum monitor_type type;
    uint16_t tcp_port;
} monitor_state = { MONITOR_NONE, 0 };

static bool
parse_pin_token(const char *token,
                struct ser2net_pin_config *pins,
                int *uart_out)
{
    if (!token || !*token)
        return false;

    char upper[24];
    size_t len = strnlen(token, sizeof(upper) - 1);
    for (size_t i = 0; i < len; ++i)
        upper[i] = (char) toupper((unsigned char) token[i]);
    upper[len] = '\0';

    const char *payload = NULL;
    enum {
        TOKEN_NONE,
        TOKEN_UART,
        TOKEN_TX,
        TOKEN_RX,
        TOKEN_RTS,
        TOKEN_CTS
    } type = TOKEN_NONE;

    bool disable = false;
    if (upper[0] == '-' && upper[1] != '\0') {
        disable = true;
        memmove(upper, upper + 1, len);
        len--;
        upper[len] = '\0';
    }

    if (strncmp(upper, "UART", 4) == 0) {
        type = TOKEN_UART;
        payload = upper + 4;
    } else if (strncmp(upper, "TX", 2) == 0) {
        type = TOKEN_TX;
        payload = upper + 2;
    } else if (strncmp(upper, "RX", 2) == 0) {
        type = TOKEN_RX;
        payload = upper + 2;
    } else if (strncmp(upper, "RTS", 3) == 0) {
        type = TOKEN_RTS;
        payload = upper + 3;
    } else if (strncmp(upper, "CTS", 3) == 0) {
        type = TOKEN_CTS;
        payload = upper + 3;
    } else {
        return false;
    }

    if (!payload || (!disable && *payload == '\0'))
        return false;

    int value = -1;
    if (disable) {
        if (type == TOKEN_UART)
            return false;
        value = INT_MIN;
    } else if (strcmp(payload, "NA") == 0 || strcmp(payload, "N/A") == 0) {
        if (type == TOKEN_RTS || type == TOKEN_CTS || type == TOKEN_TX || type == TOKEN_RX)
            value = INT_MIN;
        else
            return false;
    } else {
        char *end = NULL;
        long parsed = strtol(payload, &end, 10);
        if (!end || *end != '\0')
            return false;
        value = (int) parsed;
    }

    if (type == TOKEN_UART) {
        if (pins)
            pins->uart_num = value;
        if (uart_out)
            *uart_out = value;
    } else if (pins) {
        switch (type) {
        case TOKEN_TX: pins->tx_pin = value; break;
        case TOKEN_RX: pins->rx_pin = value; break;
        case TOKEN_RTS: pins->rts_pin = value; break;
        case TOKEN_CTS: pins->cts_pin = value; break;
        default: break;
        }
    }
    return true;
}

static const char *
format_pin_token(const char *prefix, int value, char *buf, size_t len)
{
    if (!prefix || !buf || len == 0)
        return "";
    if (value < 0)
        snprintf(buf, len, "%sNA", prefix);
    else
        snprintf(buf, len, "%s%d", prefix, value);
    return buf;
}

/* Helper to send raw strings; guards against NULL for convenience. */
static void send_str(int fd, const char *str)
{
    if (!str)
        return;
    send(fd, str, strlen(str), 0);
}

static void send_line(int fd, const char *line)
{
    send_str(fd, line);
    send(fd, "\r\n", 2, 0);
}

static void send_prompt(int fd)
{
    send_str(fd, "ser2net> ");
}

static void flush_monitor_queue(int fd)
{
    if (!monitor_queue)
        return;

    struct monitor_message msg;
    while (xQueueReceive(monitor_queue, &msg, 0) == pdPASS) {
        if (monitor_state.type == MONITOR_NONE)
            continue;
        if (monitor_state.tcp_port != msg.tcp_port)
            continue;
        if ((monitor_state.type == MONITOR_TCP && msg.stream != SER2NET_MONITOR_STREAM_TCP) ||
            (monitor_state.type == MONITOR_TERM && msg.stream != SER2NET_MONITOR_STREAM_TERM))
            continue;
        send(fd, msg.data, msg.len, 0);
    }
}

static const struct ser2net_esp32_serial_port_cfg *find_port_cfg(const char *spec);
static void handle_showport(int fd, const char *spec, bool short_format);
static void handle_version(int fd);
static bool get_port_index(const char *spec, size_t *out_index);
static void fill_params_from_cfg(const struct ser2net_esp32_serial_port_cfg *cfg,
                                 struct ser2net_serial_params *params);
static void handle_setporttimeout(int fd, char *spec, char *timeout_tok);
static bool parse_setportconfig(const char *config_str,
                                struct ser2net_serial_params *params,
                                struct ser2net_pin_config *pins);
static bool apply_runtime_serial(int fd, size_t index,
                                 const struct ser2net_serial_params *params,
                                 uint32_t idle_timeout_ms,
                                 bool apply_active,
                                 const struct ser2net_pin_config *pins);
static void handle_setportcontrol(int fd, char *spec, char *controls);
static void handle_setportenable(int fd, char *spec, char *state);

static bool handle_command_line(int fd, char *line)
{
    char *saveptr = NULL;
    char *cmd = strtok_r(line, " \t", &saveptr);
    if (!cmd)
        return true;

    if (strcasecmp(cmd, "help") == 0) {
        send_line(fd, "Commands:");
        send_line(fd, "  help               - show this help");
        send_line(fd, "  version            - display firmware version");
        send_line(fd, "  showport [port]    - detailed info for ports (tcp/uart)");
        send_line(fd, "  showshortport [p]  - single-line summary for ports");
        send_line(fd, "  monitor <type> <p> - stream data (tcp or term)");
        send_line(fd, "  monitor stop       - clear monitor request");
        send_line(fd, "  disconnect <port>  - terminate active session");
        send_line(fd, "  setporttimeout <port> <seconds>");
        send_line(fd, "  setportconfig <port> <baud/flags> [UARTn] [TXm|-TX] [RXk|-RX] [RTSs|-RTS] [CTSq|-CTS]");
        send_line(fd, "  setportcontrol <port> <controls>");
        send_line(fd, "  setportenable <port> <state>");
        send_line(fd, "  quit/exit          - close connection");
    } else if (strcasecmp(cmd, "version") == 0) {
        handle_version(fd);
    } else if (strcasecmp(cmd, "showport") == 0) {
        char *spec = strtok_r(NULL, " \t", &saveptr);
        handle_showport(fd, spec, false);
    } else if (strcasecmp(cmd, "showshortport") == 0) {
        char *spec = strtok_r(NULL, " \t", &saveptr);
        handle_showport(fd, spec, true);
    } else if (strcasecmp(cmd, "monitor") == 0) {
        char *type = strtok_r(NULL, " \t", &saveptr);
        if (!type) {
            send_line(fd, "Usage: monitor <tcp|term> <tcp port>");
            send_line(fd, "       monitor stop");
        } else if (strcasecmp(type, "stop") == 0) {
            monitor_state.type = MONITOR_NONE;
            monitor_state.tcp_port = 0;
            if (monitor_queue)
                xQueueReset(monitor_queue);
        } else {
            char *port_spec = strtok_r(NULL, " \t", &saveptr);
            if (!port_spec) {
                send_line(fd, "Usage: monitor <tcp|term> <tcp port>");
            } else if (strcasecmp(type, "tcp") != 0 && strcasecmp(type, "term") != 0) {
                send_line(fd, "Monitor type must be 'tcp', 'term', or 'stop'.");
            } else {
                const struct ser2net_esp32_serial_port_cfg *cfg = find_port_cfg(port_spec);
                if (!cfg) {
                    send_line(fd, "Unknown port specification.");
                } else if (monitor_state.type != MONITOR_NONE) {
                    send_line(fd, "Already monitoring a port (use 'monitor stop' first).");
                } else {
                    if (monitor_queue)
                        xQueueReset(monitor_queue);
                    monitor_state.type = strcasecmp(type, "tcp") == 0 ? MONITOR_TCP : MONITOR_TERM;
                    monitor_state.tcp_port = cfg->tcp_port;
                }
            }
        }
    } else if (strcasecmp(cmd, "disconnect") == 0) {
        char *spec = strtok_r(NULL, " \t", &saveptr);
        if (!spec) {
            send_line(fd, "Usage: disconnect <tcp port>");
        } else if (!control_context.disconnect_cb) {
            send_line(fd, "Disconnect not supported in this build.");
        } else {
            const struct ser2net_esp32_serial_port_cfg *cfg = find_port_cfg(spec);
            if (!cfg) {
                send_line(fd, "Unknown port specification.");
            } else if (control_context.disconnect_cb(cfg->tcp_port)) {
                send_line(fd, "Disconnect requested.");
            } else {
                send_line(fd, "No active session to disconnect.");
            }
        }
    } else if (strcasecmp(cmd, "setporttimeout") == 0) {
        char *spec = strtok_r(NULL, " \t", &saveptr);
        char *timeout_tok = strtok_r(NULL, " \t", &saveptr);
        handle_setporttimeout(fd, spec, timeout_tok);
    } else if (strcasecmp(cmd, "setportconfig") == 0) {
        char *spec = strtok_r(NULL, " \t", &saveptr);
        char *config_tokens = strtok_r(NULL, "", &saveptr);
        if (!spec || !config_tokens) {
            send_line(fd, "Usage: setportconfig <tcp port> <config tokens>");
        } else {
            struct ser2net_serial_params params;
            struct ser2net_pin_config pins;
            if (!parse_setportconfig(config_tokens, &params, &pins)) {
                send_line(fd, "Invalid config string.");
                return true;
            }

            size_t index;
            if (get_port_index(spec, &index)) {
                const struct ser2net_pin_config *pins_ptr = NULL;
                if (pins.uart_num >= 0 ||
                    pins.tx_pin >= 0 || pins.tx_pin == INT_MIN ||
                    pins.rx_pin >= 0 || pins.rx_pin == INT_MIN ||
                    pins.rts_pin >= 0 || pins.rts_pin == INT_MIN ||
                    pins.cts_pin >= 0 || pins.cts_pin == INT_MIN)
                    pins_ptr = &pins;
                if (apply_runtime_serial(fd, index, &params,
                                         control_context.ports[index].idle_timeout_ms,
                                         true, pins_ptr)) {
                    send_line(fd, "Port configuration updated.");
                }
            } else {
                if (!control_context.add_port_cb) {
                    send_line(fd, "Port not found and dynamic add not supported.");
                    return true;
                }

                char *end = NULL;
                long tcp_val = strtol(spec, &end, 0);
                if (!end || *end != '\0' || tcp_val <= 0 || tcp_val > 65535) {
                    send_line(fd, "Port not found and TCP spec invalid.");
                    return true;
                }
                if (pins.uart_num < 0) {
                    send_line(fd, "Creating a new port requires UARTn token.");
                    return true;
                }
                bool tx_provided = (pins.tx_pin >= 0);
                bool rx_provided = (pins.rx_pin >= 0);
                if (!tx_provided && !rx_provided) {
                    send_line(fd, "Creating a new port requires TX or RX pin.");
                    return true;
                }

                struct ser2net_esp32_serial_port_cfg cfg = {
                    .port_id = -1,
                    .uart_num = pins.uart_num,
                    .tx_pin = UART_PIN_NO_CHANGE,
                    .rx_pin = UART_PIN_NO_CHANGE,
                    .rts_pin = (pins.rts_pin == INT_MIN) ? UART_PIN_NO_CHANGE :
                               (pins.rts_pin >= 0 ? pins.rts_pin : UART_PIN_NO_CHANGE),
                    .cts_pin = (pins.cts_pin == INT_MIN) ? UART_PIN_NO_CHANGE :
                               (pins.cts_pin >= 0 ? pins.cts_pin : UART_PIN_NO_CHANGE),
                    .tcp_port = (uint16_t) tcp_val,
                    .tcp_backlog = 4,
                    .baud_rate = params.baud,
                    .data_bits = UART_DATA_8_BITS,
                    .parity = UART_PARITY_DISABLE,
                    .stop_bits = UART_STOP_BITS_1,
                    .flow_ctrl = params.flow_control ? UART_HW_FLOWCTRL_CTS_RTS : UART_HW_FLOWCTRL_DISABLE,
                    .mode = SER2NET_PORT_MODE_TELNET,
                    .idle_timeout_ms = 0,
                    .enabled = true
                };

                if (pins.tx_pin >= 0)
                    cfg.tx_pin = pins.tx_pin;
                else if (pins.tx_pin == INT_MIN)
                    cfg.tx_pin = UART_PIN_NO_CHANGE;

                if (pins.rx_pin >= 0)
                    cfg.rx_pin = pins.rx_pin;
                else if (pins.rx_pin == INT_MIN)
                    cfg.rx_pin = UART_PIN_NO_CHANGE;

                switch (params.data_bits) {
                case 5: cfg.data_bits = UART_DATA_5_BITS; break;
                case 6: cfg.data_bits = UART_DATA_6_BITS; break;
                case 7: cfg.data_bits = UART_DATA_7_BITS; break;
                default: cfg.data_bits = UART_DATA_8_BITS; break;
                }

                if (params.parity == 1)
                    cfg.parity = UART_PARITY_ODD;
                else if (params.parity == 2)
                    cfg.parity = UART_PARITY_EVEN;

                switch (params.stop_bits) {
                case 2: cfg.stop_bits = UART_STOP_BITS_2; break;
#ifdef UART_STOP_BITS_1_5
                case 15: cfg.stop_bits = UART_STOP_BITS_1_5; break;
#endif
                default: cfg.stop_bits = UART_STOP_BITS_1; break;
                }

                if (control_context.add_port_cb(&cfg) != pdPASS) {
                    send_line(fd, "Failed to add new port (duplicate or invalid).");
                } else {
                    if (control_context.port_count < SER2NET_MAX_PORTS)
                        control_context.port_count++;
                    send_line(fd, "Port created.");
                }
            }
        }
    } else if (strcasecmp(cmd, "setportcontrol") == 0) {
        char *spec = strtok_r(NULL, " \t", &saveptr);
        char *controls = strtok_r(NULL, "", &saveptr);
        handle_setportcontrol(fd, spec, controls);
    } else if (strcasecmp(cmd, "setportenable") == 0) {
        char *spec = strtok_r(NULL, " \t", &saveptr);
        char *state = strtok_r(NULL, " \t", &saveptr);
        handle_setportenable(fd, spec, state);
    } else if (strcasecmp(cmd, "quit") == 0 || strcasecmp(cmd, "exit") == 0) {
        return false;
    } else {
        send_line(fd, "Unknown command. Type 'help'.");
    }

    return true;
}

/* Turn UART word-length enum values into strings for the shell output. */
static const char *uart_word_length_to_str(uart_word_length_t len)
{
    switch (len) {
    case UART_DATA_5_BITS: return "5";
    case UART_DATA_6_BITS: return "6";
    case UART_DATA_7_BITS: return "7";
    case UART_DATA_8_BITS: return "8";
    default: return "?";
    }
}

/* Map UART parity enum to printable text. */
static const char *uart_parity_to_str(uart_parity_t parity)
{
    switch (parity) {
    case UART_PARITY_DISABLE: return "none";
    case UART_PARITY_EVEN: return "even";
    case UART_PARITY_ODD: return "odd";
    default: return "?";
    }
}

/* Map UART stop-bits enum to printable text. */
static const char *uart_stop_bits_to_str(uart_stop_bits_t stop_bits)
{
    switch (stop_bits) {
    case UART_STOP_BITS_1: return "1";
    case UART_STOP_BITS_1_5: return "1.5";
    case UART_STOP_BITS_2: return "2";
    default: return "?";
    }
}

/* Map flow-control enum to printable text. */
static const char *uart_flow_to_str(uart_hw_flowcontrol_t flow)
{
    switch (flow) {
    case UART_HW_FLOWCTRL_DISABLE: return "disabled";
    case UART_HW_FLOWCTRL_RTS: return "rts";
    case UART_HW_FLOWCTRL_CTS: return "cts";
    case UART_HW_FLOWCTRL_CTS_RTS: return "cts+rts";
    default: return "?";
    }
}

/* Locate a configured port using TCP port, port_id, or UART number. */
static const struct ser2net_esp32_serial_port_cfg *
find_port_cfg(const char *spec)
{
    if (!control_context.ports || control_context.port_count == 0 || !spec)
        return NULL;

    char *end = NULL;
    long val = strtol(spec, &end, 0);
    if (!spec || *spec == '\0' || (end && *end != '\0'))
        return NULL;
    if (val < 0 || val > 65535)
        return NULL;

    for (size_t i = 0; i < control_context.port_count; ++i) {
        const struct ser2net_esp32_serial_port_cfg *cfg = &control_context.ports[i];
        if ((int) cfg->tcp_port == val ||
            cfg->port_id == val ||
            cfg->uart_num == val)
            return cfg;
    }
    return NULL;
}

static const char *port_mode_to_str(enum ser2net_port_mode mode)
{
    switch (mode) {
    case SER2NET_PORT_MODE_RAW: return "raw";
    case SER2NET_PORT_MODE_RAWLP: return "rawlp";
    case SER2NET_PORT_MODE_TELNET:
    default:
        return "telnet";
    }
}

static bool get_port_index(const char *spec, size_t *out_index)
{
    if (!control_context.ports || !spec)
        return false;

    char *end = NULL;
    long value = strtol(spec, &end, 0);
    bool numeric = (spec && *spec && (!end || *end == '\0'));

    for (size_t i = 0; i < control_context.port_count; ++i) {
        const struct ser2net_esp32_serial_port_cfg *cfg = &control_context.ports[i];
        if (numeric) {
            if ((int)cfg->tcp_port == value || cfg->port_id == value || cfg->uart_num == value) {
                if (out_index)
                    *out_index = i;
                return true;
            }
        } else {
            char uart_buf[16];
            snprintf(uart_buf, sizeof(uart_buf), "uart%d", cfg->uart_num);
            if (strcasecmp(uart_buf, spec) == 0) {
                if (out_index)
                    *out_index = i;
                return true;
            }
        }
    }
    return false;
}

static void fill_params_from_cfg(const struct ser2net_esp32_serial_port_cfg *cfg,
                                 struct ser2net_serial_params *params)
{
    if (!cfg || !params)
        return;
    params->baud = cfg->baud_rate;
    switch (cfg->data_bits) {
    case UART_DATA_5_BITS: params->data_bits = 5; break;
    case UART_DATA_6_BITS: params->data_bits = 6; break;
    case UART_DATA_7_BITS: params->data_bits = 7; break;
    default: params->data_bits = 8; break;
    }
    if (cfg->parity == UART_PARITY_ODD)
        params->parity = 1;
    else if (cfg->parity == UART_PARITY_EVEN)
        params->parity = 2;
    else
        params->parity = 0;

    switch (cfg->stop_bits) {
    case UART_STOP_BITS_2: params->stop_bits = 2; break;
#ifdef UART_STOP_BITS_1_5
    case UART_STOP_BITS_1_5: params->stop_bits = 15; break;
#endif
    default: params->stop_bits = 1; break;
    }

    params->flow_control = (cfg->flow_ctrl == UART_HW_FLOWCTRL_CTS_RTS) ? 1 : 0;
}

/* Look up how many active sessions live on a given port_id. */
static int sessions_for_port_id(int port_id,
                                const int *port_ids,
                                const int *sessions,
                                size_t count)
{
    if (!port_ids || !sessions)
        return 0;
    for (size_t i = 0; i < count; ++i) {
        if (port_ids[i] == port_id)
            return sessions[i];
    }
    return 0;
}

/* Verbose variant of showport with multi-line formatting. */
static void describe_port_long(int fd,
                               const struct ser2net_esp32_serial_port_cfg *cfg,
                               int active_sessions)
{
    char line[128];
    char uart_buf[16];
    char tx_buf[16];
    char rx_buf[16];
    char rts_buf[16];
    char cts_buf[16];

    snprintf(line, sizeof(line), "Port %d (TCP %u):", cfg->port_id, cfg->tcp_port);
    send_line(fd, line);

    snprintf(line, sizeof(line), "  UART=%d backlog=%d active_sessions=%d mode=%s enabled=%s",
             cfg->uart_num, cfg->tcp_backlog, active_sessions,
             port_mode_to_str(cfg->mode), cfg->enabled ? "true" : "false");
    send_line(fd, line);

    snprintf(line, sizeof(line), "  Pins: %s %s %s %s %s",
             format_pin_token("UART", cfg->uart_num, uart_buf, sizeof(uart_buf)),
             format_pin_token("TX", cfg->tx_pin, tx_buf, sizeof(tx_buf)),
             format_pin_token("RX", cfg->rx_pin, rx_buf, sizeof(rx_buf)),
             format_pin_token("RTS", cfg->rts_pin, rts_buf, sizeof(rts_buf)),
             format_pin_token("CTS", cfg->cts_pin, cts_buf, sizeof(cts_buf)));
    send_line(fd, line);

    snprintf(line, sizeof(line),
             "  Defaults: baud=%d data_bits=%s parity=%s stop_bits=%s flow=%s",
             cfg->baud_rate,
             uart_word_length_to_str(cfg->data_bits),
             uart_parity_to_str(cfg->parity),
             uart_stop_bits_to_str(cfg->stop_bits),
             uart_flow_to_str(cfg->flow_ctrl));
    send_line(fd, line);
}

/* One-line summary used by showshortport/status. */
static void describe_port_short(int fd,
                                const struct ser2net_esp32_serial_port_cfg *cfg,
                                int active_sessions)
{
    char line[160];
    char uart_buf[16];
    char tx_buf[16];
    char rx_buf[16];
    char rts_buf[16];
    char cts_buf[16];
    snprintf(line, sizeof(line),
             "TCP=%u %s %s %s %s %s MODE=%s ENABLED=%s SESSIONS=%d",
             cfg->tcp_port,
             format_pin_token("UART", cfg->uart_num, uart_buf, sizeof(uart_buf)),
             format_pin_token("TX", cfg->tx_pin, tx_buf, sizeof(tx_buf)),
             format_pin_token("RX", cfg->rx_pin, rx_buf, sizeof(rx_buf)),
             format_pin_token("RTS", cfg->rts_pin, rts_buf, sizeof(rts_buf)),
             format_pin_token("CTS", cfg->cts_pin, cts_buf, sizeof(cts_buf)),
             port_mode_to_str(cfg->mode),
             cfg->enabled ? "true" : "false",
             active_sessions);
    send_line(fd, line);
}

/* Backend for showport/showshortport/status commands. */
static void handle_showport(int fd, const char *spec, bool short_format)
{
    if (!control_context.ports || control_context.port_count == 0) {
        send_line(fd, "No serial ports configured.");
        return;
    }

    int port_ids[SER2NET_MAX_PORTS] = {0};
    int sessions[SER2NET_MAX_PORTS] = {0};
    size_t stats_count = ser2net_get_port_stats(port_ids, sessions, SER2NET_MAX_PORTS);

    if (spec) {
        const struct ser2net_esp32_serial_port_cfg *cfg = find_port_cfg(spec);
        if (!cfg) {
            send_line(fd, "Unknown port specification.");
            return;
        }
        int session_count = sessions_for_port_id(cfg->port_id, port_ids, sessions, stats_count);
        if (short_format)
            describe_port_short(fd, cfg, session_count);
        else
            describe_port_long(fd, cfg, session_count);
        return;
    }

    for (size_t i = 0; i < control_context.port_count; ++i) {
        const struct ser2net_esp32_serial_port_cfg *cfg = &control_context.ports[i];
        int session_count = sessions_for_port_id(cfg->port_id, port_ids, sessions, stats_count);
        if (short_format)
            describe_port_short(fd, cfg, session_count);
        else
            describe_port_long(fd, cfg, session_count);
    }
}

/* Emit version banner (defaults to ser2net-mcu when unset). */
static void handle_version(int fd)
{
    char line[96];
    snprintf(line, sizeof(line), "Version: %s",
             control_context.version ? control_context.version : DEFAULT_VERSION);
    send_line(fd, line);
}

static bool apply_runtime_serial(int fd, size_t index,
                                 const struct ser2net_serial_params *params,
                                 uint32_t idle_timeout_ms,
                                 bool apply_active,
                                 const struct ser2net_pin_config *pins)
{
    if (!control_context.set_serial_config_cb) {
        send_line(fd, "Runtime does not support dynamic configuration.");
        return false;
    }
    const struct ser2net_esp32_serial_port_cfg *cfg = &control_context.ports[index];
    struct ser2net_esp32_serial_port_cfg *mutable_cfg =
        (struct ser2net_esp32_serial_port_cfg *)&control_context.ports[index];
    if (control_context.set_serial_config_cb(cfg->tcp_port, params,
                                             idle_timeout_ms, apply_active, pins) != pdPASS) {
        send_line(fd, "Failed to update configuration.");
        return false;
    }
    if (pins) {
        if (pins->uart_num >= 0)
            mutable_cfg->uart_num = pins->uart_num;
        if (pins->tx_pin >= 0 || pins->tx_pin == INT_MIN)
            mutable_cfg->tx_pin = (pins->tx_pin == INT_MIN) ? UART_PIN_NO_CHANGE : pins->tx_pin;
        if (pins->rx_pin >= 0 || pins->rx_pin == INT_MIN)
            mutable_cfg->rx_pin = (pins->rx_pin == INT_MIN) ? UART_PIN_NO_CHANGE : pins->rx_pin;
        if (pins->rts_pin >= 0 || pins->rts_pin == INT_MIN)
            mutable_cfg->rts_pin = (pins->rts_pin == INT_MIN) ? UART_PIN_NO_CHANGE : pins->rts_pin;
        if (pins->cts_pin >= 0 || pins->cts_pin == INT_MIN)
            mutable_cfg->cts_pin = (pins->cts_pin == INT_MIN) ? UART_PIN_NO_CHANGE : pins->cts_pin;
    }
    mutable_cfg->idle_timeout_ms = idle_timeout_ms;
    return true;
}

static void handle_setporttimeout(int fd, char *spec, char *timeout_tok)
{
    if (!spec || !timeout_tok) {
        send_line(fd, "Usage: setporttimeout <tcp port> <seconds>");
        return;
    }
    size_t index;
    if (!get_port_index(spec, &index)) {
        send_line(fd, "Unknown port specification.");
        return;
    }
    char *end = NULL;
    long seconds = strtol(timeout_tok, &end, 10);
    if (timeout_tok == end || seconds < 0) {
        send_line(fd, "Invalid timeout value.");
        return;
    }
    struct ser2net_serial_params params;
    fill_params_from_cfg(&control_context.ports[index], &params);
    if (apply_runtime_serial(fd, index, &params, (uint32_t)seconds * 1000U, false, NULL))
        send_line(fd, "Timeout updated.");
}

static bool parse_setportconfig(const char *config_str,
                                struct ser2net_serial_params *params,
                                struct ser2net_pin_config *pins)
{
    if (!config_str || !params)
        return false;

    *params = (struct ser2net_serial_params){
        .baud = 115200,
        .data_bits = 8,
        .parity = 0,
        .stop_bits = 1,
        .flow_control = 0
    };
    if (pins) {
        pins->uart_num = -1;
        pins->tx_pin = -1;
        pins->rx_pin = -1;
        pins->rts_pin = -1;
        pins->cts_pin = -1;
    }

    char buffer[128];
    strncpy(buffer, config_str, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    char *save = NULL;
    char *tok = strtok_r(buffer, " ,", &save);
    bool have_baud = false;
    while (tok) {
        if (pins && parse_pin_token(tok, pins, &pins->uart_num)) {
            tok = strtok_r(NULL, " ,", &save);
            continue;
        }

        if (!have_baud && isdigit((unsigned char)tok[0])) {
            long baud = strtol(tok, NULL, 10);
            if (baud > 0)
                params->baud = (int)baud;
            have_baud = true;
        } else if (strcasecmp(tok, "5DATABITS") == 0 || strcasecmp(tok, "5BITS") == 0) {
            params->data_bits = 5;
        } else if (strcasecmp(tok, "6DATABITS") == 0 || strcasecmp(tok, "6BITS") == 0) {
            params->data_bits = 6;
        } else if (strcasecmp(tok, "7DATABITS") == 0 || strcasecmp(tok, "7BITS") == 0) {
            params->data_bits = 7;
        } else if (strcasecmp(tok, "8DATABITS") == 0 || strcasecmp(tok, "8BITS") == 0) {
            params->data_bits = 8;
        } else if (strcasecmp(tok, "EVEN") == 0) {
            params->parity = 2;
        } else if (strcasecmp(tok, "ODD") == 0) {
            params->parity = 1;
        } else if (strcasecmp(tok, "NONE") == 0) {
            params->parity = 0;
        } else if (strcasecmp(tok, "1STOPBIT") == 0 || strcasecmp(tok, "1STOPBITS") == 0) {
            params->stop_bits = 1;
        } else if (strcasecmp(tok, "2STOPBITS") == 0) {
            params->stop_bits = 2;
        } else if (strcasecmp(tok, "1.5STOPBITS") == 0 || strcasecmp(tok, "1P5STOPBITS") == 0) {
            params->stop_bits = 15;
        } else if (strcasecmp(tok, "RTSCTS") == 0 || strcasecmp(tok, "+RTSCTS") == 0) {
            params->flow_control = 1;
        } else if (strcasecmp(tok, "-RTSCTS") == 0) {
            params->flow_control = 0;
        } else if (strcasecmp(tok, "LOCAL") == 0) {
            /* ignored */
        } else {
            return false;
        }
        tok = strtok_r(NULL, " ,", &save);
    }
    return true;
}

static void handle_setportcontrol(int fd, char *spec, char *controls)
{
    if (!spec || !controls) {
        send_line(fd, "Usage: setportcontrol <tcp port> <RTS/DTR tokens>");
        return;
    }

    size_t index;
    if (!get_port_index(spec, &index)) {
        send_line(fd, "Unknown port specification.");
        return;
    }

    uart_port_t uart_num = control_context.ports[index].uart_num;

    char buffer[128];
    strncpy(buffer, controls, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    char *save = NULL;
    char *tok = strtok_r(buffer, " \t", &save);
    bool ok = true;
    while (tok) {
        esp_err_t err = ESP_OK;
        if (strcasecmp(tok, "RTSHI") == 0 || strcasecmp(tok, "RTS1") == 0) {
            err = uart_set_rts(uart_num, 1);
        } else if (strcasecmp(tok, "RTSLO") == 0 || strcasecmp(tok, "RTS0") == 0) {
            err = uart_set_rts(uart_num, 0);
        } else if (strcasecmp(tok, "DTRHI") == 0 || strcasecmp(tok, "DTR1") == 0) {
            err = uart_set_dtr(uart_num, 1);
        } else if (strcasecmp(tok, "DTRLO") == 0 || strcasecmp(tok, "DTR0") == 0) {
            err = uart_set_dtr(uart_num, 0);
        } else {
            ok = false;
            break;
        }
        if (err != ESP_OK) {
            ok = false;
            break;
        }
        tok = strtok_r(NULL, " \t", &save);
    }

    if (ok)
        send_line(fd, "Port control updated.");
    else
        send_line(fd, "setportcontrol failed or unsupported token.");
}

static void handle_setportenable(int fd, char *spec, char *state)
{
    if (!spec || !state) {
        send_line(fd, "Usage: setportenable <tcp port> <off|raw|rawlp|telnet>");
        return;
    }
    if (!control_context.set_port_mode_cb) {
        send_line(fd, "setportenable not supported in this build.");
        return;
    }

    size_t index;
    if (!get_port_index(spec, &index)) {
        send_line(fd, "Unknown port specification.");
        return;
    }

    bool enable = true;
    enum ser2net_port_mode mode = control_context.ports[index].mode;

    if (strcasecmp(state, "off") == 0) {
        enable = false;
    } else if (strcasecmp(state, "raw") == 0) {
        mode = SER2NET_PORT_MODE_RAW;
    } else if (strcasecmp(state, "rawlp") == 0) {
        mode = SER2NET_PORT_MODE_RAWLP;
    } else if (strcasecmp(state, "telnet") == 0) {
        mode = SER2NET_PORT_MODE_TELNET;
    } else {
        send_line(fd, "Unknown mode. Use off/raw/rawlp/telnet.");
        return;
    }

    if (control_context.set_port_mode_cb(control_context.ports[index].tcp_port,
                                         mode, enable) != pdPASS) {
        send_line(fd, "Failed to update port mode.");
        return;
    }

    struct ser2net_esp32_serial_port_cfg *mutable_cfg =
        (struct ser2net_esp32_serial_port_cfg *)&control_context.ports[index];
    mutable_cfg->mode = mode;
    mutable_cfg->enabled = enable;

    send_line(fd, enable ? "Port enabled." : "Port disabled.");
}

static void serve_client(int client_fd)
{
    /* Main client REPL loop – respond to commands until connection closes. */
    send_line(client_fd, "ESP ser2net control port");
    handle_version(client_fd);
    send_line(client_fd, "Type 'help' for commands.");
    send_prompt(client_fd);

    int flags = fcntl(client_fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    char line[CTRL_MAX_LINE];
    size_t line_len = 0;
    bool keep_running = true;

    while (keep_running && control_running) {
        flush_monitor_queue(client_fd);

        uint8_t buf[64];
        int rv = recv(client_fd, buf, sizeof(buf), 0);
        if (rv > 0) {
            for (int i = 0; i < rv; ++i) {
                char ch = (char) buf[i];
                if (ch == '\r')
                    continue;
                if (ch == '\n') {
                    line[line_len] = '\0';
                    keep_running = handle_command_line(client_fd, line);
                    line_len = 0;
                    if (!keep_running)
                        break;
                    send_prompt(client_fd);
                    continue;
                }
                if (line_len + 1 < sizeof(line))
                    line[line_len++] = ch;
            }
            if (keep_running)
                continue;
            break;
        }

        if (rv == 0)
            break; /* remote closed */

        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (errno == EINTR)
            continue;

        break; /* other error */
    }

    flush_monitor_queue(client_fd);
    monitor_state.type = MONITOR_NONE;
    monitor_state.tcp_port = 0;
    if (monitor_queue)
        xQueueReset(monitor_queue);
}

static void control_task(void *arg)
{
    /* Dedicated FreeRTOS task that owns the control socket. */
    (void) arg;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(control_context.tcp_port)
    };

    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd < 0) {
        control_running = false;
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(listen_fd);
        control_running = false;
        vTaskDelete(NULL);
        return;
    }
    int backlog = control_context.backlog <= 0 ? 2 : control_context.backlog;
    if (listen(listen_fd, backlog) < 0) {
        close(listen_fd);
        control_running = false;
        vTaskDelete(NULL);
        return;
    }

    control_listen_fd = listen_fd;
    for (;;) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (client_fd < 0) {
            if (!control_running)
                break;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        serve_client(client_fd);
        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
        if (!control_running)
            break;
    }

    close(listen_fd);
    control_listen_fd = -1;
    control_task_handle = NULL;
    control_running = false;
    vTaskDelete(NULL);
}

bool ser2net_control_start(const struct ser2net_control_context *ctx)
{
    /* Launch the control port task (no-op if already running). */
    if (!ctx || ctx->tcp_port == 0)
        return false;
    if (control_running)
        return true;

    control_context = *ctx;
    if (!control_context.ports)
        control_context.port_count = 0;
    if (!control_context.version)
        control_context.version = DEFAULT_VERSION;
    monitor_state.type = MONITOR_NONE;
    monitor_state.tcp_port = 0;
    if (!monitor_queue) {
        monitor_queue = xQueueCreate(MONITOR_QUEUE_DEPTH, sizeof(struct monitor_message));
        if (!monitor_queue)
            return false;
    } else {
        xQueueReset(monitor_queue);
    }
    control_running = true;
    if (xTaskCreate(control_task, "ser2net_ctrl",
                    4096, NULL, tskIDLE_PRIORITY + 1,
                    &control_task_handle) != pdPASS) {
        control_running = false;
        control_task_handle = NULL;
        return false;
    }
    return true;
}

void ser2net_control_stop(void)
{
    /* Stop task and close listener (idempotent). */
    control_running = false;
    if (control_listen_fd >= 0) {
        shutdown(control_listen_fd, SHUT_RDWR);
        close(control_listen_fd);
        control_listen_fd = -1;
    }
    if (control_task_handle) {
        vTaskDelete(control_task_handle);
        control_task_handle = NULL;
    }
    monitor_state.type = MONITOR_NONE;
    monitor_state.tcp_port = 0;
    if (monitor_queue) {
        vQueueDelete(monitor_queue);
        monitor_queue = NULL;
    }
}

void ser2net_control_monitor_feed(uint16_t tcp_port,
                                  enum ser2net_monitor_stream stream,
                                  const uint8_t *data,
                                  size_t len)
{
    if (!control_running || !monitor_queue || !data || len == 0)
        return;

    enum monitor_type type = monitor_state.type;
    if (type == MONITOR_NONE)
        return;
    if (monitor_state.tcp_port != tcp_port)
        return;
    if ((type == MONITOR_TCP && stream != SER2NET_MONITOR_STREAM_TCP) ||
        (type == MONITOR_TERM && stream != SER2NET_MONITOR_STREAM_TERM))
        return;

    while (len > 0) {
        size_t chunk = len > MONITOR_CHUNK ? MONITOR_CHUNK : len;
        struct monitor_message msg = {
            .stream = stream,
            .tcp_port = tcp_port,
            .len = chunk
        };
        memcpy(msg.data, data, chunk);
        if (xQueueSend(monitor_queue, &msg, 0) != pdPASS)
            break;
        data += chunk;
        len -= chunk;
    }
}
