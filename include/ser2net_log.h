/*
 * ser2net MCU - Logging shim
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

#ifndef SER2NET_LOG_H
#define SER2NET_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(SER2NET_USE_ESP_LOG)
#include "esp_log.h"

#ifndef SER2NET_LOGI
#define SER2NET_LOGI ESP_LOGI
#endif
#ifndef SER2NET_LOGW
#define SER2NET_LOGW ESP_LOGW
#endif
#ifndef SER2NET_LOGE
#define SER2NET_LOGE ESP_LOGE
#endif
#ifndef SER2NET_LOG_BUFFER_HEXDUMP
#define SER2NET_LOG_BUFFER_HEXDUMP ESP_LOG_BUFFER_HEXDUMP
#endif

#ifndef SER2NET_LOG_LEVEL_INFO
#define SER2NET_LOG_LEVEL_INFO ESP_LOG_INFO
#endif

#else /* generic stdio fallback */

#include <stdio.h>
#include <stddef.h>

#ifndef SER2NET_LOGI
#define SER2NET_LOGI(tag, fmt, ...) \
    do { printf("[I] %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#endif

#ifndef SER2NET_LOGW
#define SER2NET_LOGW(tag, fmt, ...) \
    do { printf("[W] %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#endif

#ifndef SER2NET_LOGE
#define SER2NET_LOGE(tag, fmt, ...) \
    do { printf("[E] %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#endif

#ifndef SER2NET_LOG_BUFFER_HEXDUMP
static inline void ser2net_log_hexdump(const char *tag,
                                       const void *data,
                                       size_t len)
{
    const unsigned char *bytes = (const unsigned char *) data;
    for (size_t i = 0; i < len; ++i) {
        if ((i % 16) == 0)
            printf("[D] %s: %04zx: ", tag, i);
        printf("%02x ", bytes[i]);
        if ((i % 16) == 15 || i + 1 == len)
            printf("\n");
    }
}
#define SER2NET_LOG_BUFFER_HEXDUMP(tag, data, len, level) \
    ser2net_log_hexdump(tag, data, len)
#endif

#ifndef SER2NET_LOG_LEVEL_INFO
#define SER2NET_LOG_LEVEL_INFO 0
#endif

#endif /* SER2NET_USE_ESP_LOG */

#ifdef __cplusplus
}
#endif

#endif /* SER2NET_LOG_H */
