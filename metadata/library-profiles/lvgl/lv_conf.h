/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * lv_conf.h for the Alp SDK's embedded targets.
 *
 * LVGL (https://github.com/lvgl/lvgl) requires an lv_conf.h on
 * the include path -- the upstream lib pulls in unset macros'
 * defaults from lv_conf_internal.h, so this profile sets only
 * the SDK-relevant overrides and lets the rest fall through.
 *
 * Consumers wanting different LVGL settings drop their own
 * lv_conf.h at the app's include root; the loader prefers the
 * app's profile over this one.
 *
 * Tuned for the displays the E1M family actually drives:
 *   - 128x64 mono OLEDs (chips/ssd1306)  -- LV_COLOR_DEPTH 1
 *   - 96x64 RGB OLEDs (chips/ssd1331)    -- LV_COLOR_DEPTH 16
 *   - 480x272 IPS LCDs on V2N            -- LV_COLOR_DEPTH 16
 *   - 800x480 5"+ panels on V2N-M1       -- LV_COLOR_DEPTH 16
 * 16-bit RGB565 is the practical baseline; apps on the mono OLED
 * override LV_COLOR_DEPTH=1 in their own lv_conf.h.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* ----------------------------------------------------------------- */
/* Color settings                                                     */
/* ----------------------------------------------------------------- */

/* RGB565 -- the practical baseline across the E1M display roster.
 * Override to 1 for mono OLEDs or to 32 for ARGB8888 if a custom
 * board ships a 24-bit-color panel. */
#define LV_COLOR_DEPTH 16

/* Standard sRGB ordering matches the Zephyr display driver default
 * for the supported panels. */
#define LV_COLOR_16_SWAP 0

/* ----------------------------------------------------------------- */
/* Memory settings                                                    */
/* ----------------------------------------------------------------- */

/* Use LVGL's built-in TLSF allocator -- avoids heap fragmentation
 * on Cortex-M targets where the C library's allocator isn't ideal. */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (48U * 1024U)

/* No automatic startup allocations -- the app's lv_init() controls
 * lifecycle. */
#define LV_MEM_AUTO_DEFRAG 1

/* ----------------------------------------------------------------- */
/* HAL + tick                                                         */
/* ----------------------------------------------------------------- */

/* Tick frequency the lv_tick_inc()-driving timer ticks at.  Apps
 * typically wire this to a 1 ms tick from Zephyr's k_timer / k_work. */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "zephyr/kernel.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (k_uptime_get_32())

/* Default DPI -- 130 covers the typical small/medium panels.
 * Apps override per-display. */
#define LV_DPI_DEF 130

/* ----------------------------------------------------------------- */
/* Feature gates -- minimise binary size by default                   */
/* ----------------------------------------------------------------- */

/* Anti-aliasing is worth the cycles on RGB565+; off on mono. */
#define LV_USE_ANTIALIAS 1

/* No log output by default -- apps enable for development. */
#define LV_USE_LOG 0

/* Drop the demos library; apps that want it pull it explicitly. */
#define LV_USE_DEMO_WIDGETS 0
#define LV_USE_DEMO_KEYPAD_AND_ENCODER 0
#define LV_USE_DEMO_BENCHMARK 0
#define LV_USE_DEMO_STRESS 0
#define LV_USE_DEMO_MUSIC 0

/* Filesystem integration off by default -- apps that need it wire
 * one of LV_USE_FS_STDIO / LV_USE_FS_POSIX / LV_USE_FS_FATFS
 * (or LittleFS via Zephyr) in their own lv_conf.h. */
#define LV_USE_FS_STDIO 0
#define LV_USE_FS_POSIX 0
#define LV_USE_FS_WIN32 0
#define LV_USE_FS_FATFS 0

/* PNG / BMP / JPG decoders off by default to save flash; apps
 * that want image loaders enable individually. */
#define LV_USE_PNG 0
#define LV_USE_BMP 0
#define LV_USE_SJPG 0
#define LV_USE_GIF 0
#define LV_USE_QRCODE 0

#endif /* LV_CONF_H */
