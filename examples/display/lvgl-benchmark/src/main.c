/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * lvgl-benchmark
 * ==============
 *
 * Wraps the upstream `lv_demo_benchmark()` -- the canonical LVGL
 * performance test that walks through a fixed scene set (gradient
 * fills, alpha blends, text rendering, image rotation, ...) and
 * reports per-scene FPS at the end.
 *
 *
 * ── What it measures ───────────────────────────────────────────
 *
 * Run on an E1M-AEN801 (E8 silicon -- GPU2D + DAVE2D + DMA2D
 * populated) vs E1M-AEN301 (E3 -- pure-C blit only) to see the
 * §D.lib.loader's `capabilities:`-driven binding choice land on the
 * frame-rate.  Run on native_sim/native/64 for the CPU-only
 * baseline.
 *
 * Typical results (paper-correct -- v1.0 HiL fills these in):
 *
 *   ┌──────────────────────────────┬──────────┬──────────┬─────────┐
 *   │ Target                        │ Scene A   │ Scene F   │ Total   │
 *   ├──────────────────────────────┼──────────┼──────────┼─────────┤
 *   │ AEN801 (GPU2D + DMA2D)        │ TBD       │ TBD       │ TBD     │
 *   │ AEN301 (pure-C blit, DMA2D)   │ TBD       │ TBD       │ TBD     │
 *   │ native_sim/native/64 (SDL2)   │ TBD       │ TBD       │ TBD     │
 *   └──────────────────────────────┴──────────┴──────────┴─────────┘
 *
 *
 * ── Why this matters ───────────────────────────────────────────
 *
 * Customers picking between Ensemble SKUs care about the cost of
 * rendering: a smart-thermostat UI that runs at 60 fps on E8 may
 * cap at 18 fps on E3.  This demo lets them measure that on their
 * actual board + panel combination without writing benchmark
 * code themselves.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <alp/display.h>
#include <alp/gui.h>

#include <lv_demo_benchmark.h>

LOG_MODULE_REGISTER(lvgl_benchmark, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("LVGL benchmark starting");

	/* Open the panel through the portable display surface -- see
     * lvgl-widgets-demo/src/main.c for the full rationale on this
     * open -> lv_init -> tick -> attach sequence, and why
     * CONFIG_LV_Z_AUTO_INIT=n (prj.conf) is required alongside it. */
	alp_display_config_t display_cfg = ALP_DISPLAY_CONFIG_DEFAULT(0);
	alp_display_t       *display     = alp_display_open(&display_cfg);
	if (display == NULL) {
		LOG_ERR("display open failed (err=%d)", (int)alp_last_error());
		return 1;
	}

	/* Initialise LVGL (allocates global state -- styles, timers, ...)
     * and wire its millisecond tick source ourselves: with
     * CONFIG_LV_Z_AUTO_INIT=n, Zephyr's lvgl module never runs the
     * SYS_INIT hook that would otherwise do both. */
	lv_init();
	lv_tick_set_cb(k_uptime_get_32);

	/* Bind LVGL's renderer to the panel (src/gui_lvgl.c). */
	alp_status_t attach_status = alp_gui_lvgl_attach(display);
	if (attach_status != ALP_OK) {
		LOG_ERR("alp_gui_lvgl_attach failed (err=%d)", (int)attach_status);
		return 1;
	}

	/* Kick off the benchmark.  The scene set is fixed and lives
     * upstream in LVGL's demos/benchmark/.  Results print to the
     * Zephyr console via LVGL's lv_log() hook, which our prj.conf
     * routes to printk via CONFIG_LOG_PRINTK=y. */
	lv_demo_benchmark();

	/* Drain the LVGL timer queue; the benchmark scene plays through
     * scene-by-scene and emits its summary when the last scene
     * completes.  Same 10 ms slice as the widgets demo. */
	while (1) {
		const uint32_t sleep_ms = lv_timer_handler();
		k_msleep(MIN(sleep_ms, 10u));
	}

	return 0;
}
