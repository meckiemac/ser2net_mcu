#include "adapters.h"

#if SER2NET_TARGET == SER2NET_PLATFORM_ESP32

#include <string.h>
#include <stdbool.h>

#include "ser2net_log.h"

static const char *ESP32_SERIAL_TAG = "ser2net_adapter";

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

/**
 * @brief Locate the requested UART, install the driver, and return a handle.
 *
 * @param ctx Unused (kept for interface symmetry).
 * @param port_id Logical port identifier requested by the runtime.
 * @param out_serial Receives the opaque serial handle.
 * @return `pdPASS` when the UART is ready, otherwise `pdFAIL`.
 */
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

        struct esp32_serial_handle *handle = ser2net_os_malloc(sizeof(*handle));
        if (!handle)
            return pdFAIL;

        handle->state = state;
        *out_serial = handle;
        return pdPASS;
    }

    return pdFAIL;
}

/**
 * @brief Close a UART handle previously returned by ::esp32_open_serial().
 *
 * @param ctx Unused.
 * @param serial Handle to close.
 */
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
    ser2net_os_free(handle);
}

/**
 * @brief Read bytes from the UART into the provided buffer.
 *
 * @param ctx Unused.
 * @param serial UART handle returned by ::esp32_open_serial().
 * @param buf Destination buffer.
 * @param len Maximum number of bytes to read.
 * @param timeout_ticks FreeRTOS ticks to wait for data.
 * @return Number of bytes read or -1 on error.
 */
static int
esp32_serial_read(void *ctx, ser2net_serial_handle_t serial,
                  void *buf, size_t len, ser2net_tick_t timeout_ticks)
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

/**
 * @brief Write bytes to the UART.
 *
 * @param ctx Unused.
 * @param serial UART handle.
 * @param buf Payload to transmit.
 * @param len Payload size.
 * @return Number of bytes written or -1 on error.
 */
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

/**
 * @brief Apply baud/format/flow changes to an active UART.
 *
 * @param ctx Unused.
 * @param serial UART handle.
 * @param baud Desired baud rate.
 * @param data_bits Desired number of data bits.
 * @param parity RFC2217 parity code.
 * @param stop_bits RFC2217 stop-bit code (1,2,15=1.5).
 * @param flow_control 0=off, 1=CTS/RTS.
 */
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

    SER2NET_LOGI(ESP32_SERIAL_TAG,
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

/**
 * @brief Obtain the singleton serial adapter (installs UART drivers on demand).
 *
 * @param cfg Optional configuration blob listing all UARTs/pins.
 * @return Pointer to the shared ::ser2net_serial_if instance.
 */
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

/**
 * @brief Register a dynamically added UART/TCP tuple with the adapter.
 *
 * @param cfg Port description (pins, uart number, baud, etc.).
 * @return `pdPASS` on success, otherwise `pdFAIL`.
 */
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

/**
 * @brief Remove a port definition previously registered at runtime.
 *
 * Ensures the UART driver is torn down and compacts the configuration table so
 * subsequent additions reuse the freed slot.
 *
 * @param port_id Logical port identifier to remove.
 */
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

#endif /* SER2NET_PLATFORM_ESP32 */
