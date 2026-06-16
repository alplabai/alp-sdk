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
 * Run on an E1M-AEN701 (E7 silicon -- GPU2D + DAVE2D + DMA2D
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
 *   │ AEN701 (GPU2D + DMA2D)        │ TBD       │ TBD       │ TBD     │
 *   │ AEN301 (pure-C blit, DMA2D)   │ TBD       │ TBD       │ TBD     │
 *   │ native_sim/native/64 (SDL2)   │ TBD       │ TBD       │ TBD     │
 *   └──────────────────────────────┴──────────┴──────────┴─────────┘
 *
 *
 * ── Why this matters ───────────────────────────────────────────
 *
 * Customers picking between Ensemble SKUs care about the cost of
 * rendering: a smart-thermostat UI that runs at 60 fps on E7 may
 * cap at 18 fps on E3.  This demo lets them measure that on their
 * actual board + panel combination without writing benchmark
 * code themselves.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/display.h>

#include <lvgl.h>
#include <lv_demo_benchmark.h>

LOG_MODULE_REGISTER(lvgl_benchmark, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("LVGL benchmark starting");

	/* Resolve the display via devicetree -- whichever node has
     * `zephyr,display` is the panel LVGL renders into. */
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display)) {
		LOG_ERR("display %s not ready", display->name);
		return 1;
	}

	/* Initialise LVGL (allocates framebuffer + draw context). */
	lv_init();

	/* Kick off the benchmark.  The scene set is fixed and lives
     * upstream in LVGL's demos/benchmark/.  Results print to the
     * Zephyr console via LVGL's lv_log() hook, which our prj.conf
     * routes to printk via CONFIG_LOG_PRINTK=y. */
	lv_demo_benchmark();

	display_blanking_off(display);

	/* Drain the LVGL task queue; the benchmark scene plays through
     * scene-by-scene and emits its summary when the last scene
     * completes.  Same 10 ms slice as the widgets demo. */
	while (1) {
		const uint32_t sleep_ms = lv_task_handler();
		k_msleep(MIN(sleep_ms, 10u));
	}

	return 0;
}
