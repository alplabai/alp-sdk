/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * lvgl-music-player
 * =================
 *
 * Wraps the upstream `lv_demo_music()` -- the most polished of
 * LVGL's bundled demos.  Renders an album-art carousel, time
 * slider, track list, and a real-time equaliser visualiser.
 *
 *
 * ── Hardware wiring (E1M-EVK reference) ────────────────────────
 *
 *   ┌──────────────────┐   SPI0     ┌────────────────────┐
 *   │  E1M-AEN701 SoM  │ ─────────▶ │ ST7789 240×320 TFT │
 *   │ + Cortex-M55 HE  │   GPIO     │  (display)         │
 *   └──────────────────┘            └────────────────────┘
 *           │
 *           │  I²C0 + I²S0
 *           ▼
 *   ┌────────────────────┐   line-out   ┌────────┐
 *   │ Wolfson WM8960     │ ───────────▶ │ headphone │
 *   │ codec              │              │  out      │
 *   └────────────────────┘              └────────┘
 *
 * The demo's UI itself doesn't drive audio -- it only animates the
 * progress bar + equaliser.  Wiring the actual MP3 decode is left
 * to the `audio` task in main(); we kick off the player but stub
 * the I²S frames to silence so the example builds clean on
 * native_sim too.
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

    /*
     * TODO(audio): wire the decoded PCM frames from minimp3 into
     * alp_i2s_write(...).  For native_sim the codec is absent --
     * we just animate the UI and hand silence to the I²S sink.
     * Real codec bring-up gates on the v1.0 HiL sweep.
     */

    while (1) {
        const uint32_t sleep_ms = lv_task_handler();
        k_msleep(MIN(sleep_ms, 10u));
    }

    return 0;
}
