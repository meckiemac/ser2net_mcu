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
 *  ser2net - Configuration helpers
 *
 *  Provides convenience wrappers around the runtime/session/adapter
 *  layers so application code can start/stop the service with a single
 *  call.
 */

#include "config.h"

#include <string.h>

static struct ser2net_runtime_config g_runtime_cfg;

/**
 * @brief Populate ::ser2net_runtime_config with conservative defaults.
 *
 * The defaults favour stability over absolute throughput so applications can
 * tweak only the fields they care about.
 *
 * @param cfg Destination structure (must not be NULL).
 */
void
ser2net_runtime_config_init(struct ser2net_runtime_config *cfg)
{
    if (!cfg)
        return;

    memset(cfg, 0, sizeof(*cfg));
    cfg->listener_task_name = "ser2net_listen";
    cfg->listener_task_priority = tskIDLE_PRIORITY + 2;
    cfg->listener_task_stack_words = 4096;
    cfg->session_task_name = "ser2net_session";
    cfg->session_task_priority = tskIDLE_PRIORITY + 1;
    cfg->session_task_stack_words = 4096;
    cfg->max_sessions = 4;
    cfg->accept_poll_ticks = pdMS_TO_TICKS(200);
    cfg->session_block_ticks = pdMS_TO_TICKS(20);
    cfg->listener_count = 0;
    memset(cfg->listeners, 0, sizeof(cfg->listeners));
    cfg->control_enabled = false;
    memset(&cfg->control_ctx, 0, sizeof(cfg->control_ctx));
    cfg->config_changed_cb = NULL;
    cfg->config_changed_ctx = NULL;
}

/**
 * @brief Initialise ::ser2net_basic_session_cfg with safe buffer sizes.
 *
 * @param cfg Destination structure (must not be NULL).
 */
void
ser2net_session_config_init(struct ser2net_basic_session_cfg *cfg)
{
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->net_buf_size = 512;
    cfg->serial_buf_size = 512;
}

/**
 * @brief Convenience façade that ties adapters, session ops, and runtime start.
 *
 * @param cfg Aggregated application configuration (adapters + defaults).
 * @return `pdPASS` when the runtime is fully operational.
 */
BaseType_t
ser2net_start(const struct ser2net_app_config *cfg)
{
    if (!cfg || !cfg->serial_if)
        return pdFAIL;
    if (!cfg->network_if && cfg->runtime_cfg.listener_count > 0)
        return pdFAIL;

    const struct ser2net_session_ops *session_ops =
        ser2net_basic_session_ops_init(cfg->network_if,
                                       cfg->serial_if,
                                       &cfg->session_cfg);

    g_runtime_cfg = cfg->runtime_cfg;
    g_runtime_cfg.network = cfg->network_if;
    g_runtime_cfg.serial = cfg->serial_if;
    g_runtime_cfg.session_ops = session_ops;

    return ser2net_runtime_start(&g_runtime_cfg);
}

/**
 * @brief Stop the runtime that was launched via ::ser2net_start().
 */
void
ser2net_stop(void)
{
    ser2net_runtime_stop();
}
