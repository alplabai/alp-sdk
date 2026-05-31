/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * lvgl-music-player
 * =================
 *
 * Wraps the upstream `lv_demo_music()` -- the most polished of
 * LVGL's bundled demos.  Renders an album-art carousel, time
 * slider, track list, and a real-time equaliser visualiser.
 *
 * This is a DISPLAY-ONLY demo.  lv_demo_music() animates the
 * progress bar + equaliser bands on its own timer; it does not
 * decode or play any audio, and this file makes no alp_i2s_* or
 * codec calls.  A real audio output path (codec over I²S + an MP3
 * decoder) is out of scope -- the point here is the LVGL UI.
 *
 *
 * ── Hardware wiring (E1M-EVK reference) ────────────────────────
 *
 *   ┌──────────────────┐   SPI      ┌────────────────────┐
 *   │  E1M-AEN701 SoM  │ ─────────▶ │ ST7789 240×320 TFT │
 *   │ + Cortex-M55 HP  │   GPIO     │  (display)         │
 *   └──────────────────┘            └────────────────────┘
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/display.h>

#include <lvgl.h>
#include <lv_demo_music.h>

LOG_MODULE_REGISTER(lvgl_music_player, LOG_LEVEL_INF);

int main(void)
{
    LOG_INF("LVGL music-player demo starting");

    const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (!device_is_ready(display)) {
        LOG_ERR("display %s not ready", display->name);
        return 1;
    }

    lv_init();

    /* Kick off the demo UI.  lv_demo_music() builds the full music-
     * player scene + spawns the periodic animation timer that
     * advances the progress bar + equaliser bands. */
    lv_demo_music();

    display_blanking_off(display);

    /* lv_demo_music() owns its own animation timer; from here on
     * main just pumps the LVGL task handler.  No audio path -- this
     * is a UI-only demo (see the file header). */
    while (1) {
        const uint32_t sleep_ms = lv_task_handler();
        k_msleep(MIN(sleep_ms, 10u));
    }

    return 0;
}
