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
 *   │  E1M-AEN801 SoM  │ ─────────▶ │ ST7789 240×320 TFT │
 *   │ + Cortex-M55 HP  │   GPIO     │  (display)         │
 *   └──────────────────┘            └────────────────────┘
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <alp/display.h>
#include <alp/gui.h>

#include <lv_demo_music.h>

LOG_MODULE_REGISTER(lvgl_music_player, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("LVGL music-player demo starting");

	/* Open the panel through the portable display surface, then
     * bind LVGL to it via <alp/gui.h> -- see
     * lvgl-widgets-demo/src/main.c for the full rationale on this
     * open -> lv_init -> tick -> attach sequence, and why
     * CONFIG_LV_Z_AUTO_INIT=n (prj.conf) is required alongside it. */
	alp_display_config_t display_cfg = ALP_DISPLAY_CONFIG_DEFAULT(0);
	alp_display_t       *display     = alp_display_open(&display_cfg);
	if (display == NULL) {
		LOG_ERR("display open failed (err=%d)", (int)alp_last_error());
		return 1;
	}

	lv_init();
	lv_tick_set_cb(k_uptime_get_32);

	alp_status_t attach_status = alp_gui_lvgl_attach(display);
	if (attach_status != ALP_OK) {
		LOG_ERR("alp_gui_lvgl_attach failed (err=%d)", (int)attach_status);
		return 1;
	}

	/* Kick off the demo UI.  lv_demo_music() builds the full music-
     * player scene + spawns the periodic animation timer that
     * advances the progress bar + equaliser bands. */
	lv_demo_music();

	/* lv_demo_music() owns its own animation timer; from here on
     * main just pumps the LVGL timer handler.  No audio path -- this
     * is a UI-only demo (see the file header). */
	while (1) {
		const uint32_t sleep_ms = lv_timer_handler();
		k_msleep(MIN(sleep_ms, 10u));
	}

	return 0;
}
