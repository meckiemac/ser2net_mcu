/*
 * ser2net MCU - Platform selection helpers
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

#ifndef SER2NET_PLATFORM_H
#define SER2NET_PLATFORM_H

#define SER2NET_PLATFORM_ESP32   1
#define SER2NET_PLATFORM_STM32   2
#define SER2NET_PLATFORM_RP2040  3

#ifndef SER2NET_TARGET
#define SER2NET_TARGET SER2NET_PLATFORM_ESP32
#endif

#endif /* SER2NET_PLATFORM_H */
