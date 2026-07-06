/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * lvgl-widgets-demo
 * =================
 *
 * Wraps the upstream `lv_demo_widgets()` from LVGL's demos/ tree.
 * Walks through every standard LVGL widget (buttons, sliders,
 * tabs, charts, meters, lists, ...) on a 240 x 320 ST7789 TFT
 * attached to the E1M-EVK SPI0 + a pair of GPIOs (D/C# + reset).
 *
 * This is a teaching artefact: ~50% of the file is comment
 * explaining the data flow + the SDK rules in play.  Customers
 * copy this directory + the matching board.yaml as a starting
 * point for their own LVGL apps.
 *
 *
 * ── Why this layout? ───────────────────────────────────────────
 *
 *   1. `board.yaml` is the only file that names hardware.  It
 *      declares the LVGL library plus the SPI/GPIO resources the
 *      board devicetree binds to Zephyr's MIPI DBI Type C display
 *      path.
 *
 *   2. `src/main.c` (this file) talks to the panel ONLY through
 *      the portable LVGL surface:
 *        - <lvgl.h>    for the UI API
 *        - <alp/gui.h> (optional) for the future LVGL bind glue
 *      No `gd32g553_*` / `alif_*` / `renesas_*` symbols appear
 *      anywhere -- per the SDK's "Portable peripheral surfaces
 *      only in app + library code" rule.  Board-specific
 *      backends live inside the SDK; this file stays board-
 *      portable.
 *
 *   3. Zephyr's `lvgl` module handles the display-driver registration
 *      automatically once CONFIG_LVGL + the right display alias are
 *      set in the devicetree overlay.  We just call lv_init() then
 *      hand off to the upstream demo.
 *
 *
 * ── Build matrix ───────────────────────────────────────────────
 *
 *   `west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/display/lvgl-widgets-demo`
 *   `west build -b native_sim/native/64 examples/display/lvgl-widgets-demo`
 *
 *   native_sim runs the demo in a desktop window via SDL2 -- handy
 *   for UI iteration without touching hardware.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/display.h>

#include <lvgl.h>

/* Upstream LVGL demo entrypoint -- pulled in by
 * CONFIG_LV_USE_DEMO_WIDGETS=y in prj.conf. */
#include <lv_demo_widgets.h>

LOG_MODULE_REGISTER(lvgl_widgets_demo, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("LVGL widgets demo starting");

	/* Zephyr's display subsystem is the bedrock LVGL renders into.
     * The DEVICE_DT_GET path resolves whichever node devicetree
     * marks as the default display -- on the E1M-EVK that's the
     * ST7789 panel behind the MIPI DBI Type C display node. */
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display)) {
		LOG_ERR("display %s not ready", display->name);
		return 1;
	}

	/* LVGL initialisation: allocate the framebuffer + draw context,
     * register the Zephyr display driver as LVGL's render target.
     * Zephyr's lvgl module ties these into lv_init() automatically
     * when CONFIG_LVGL=y; this call sets the global state. */
	lv_init();

	/* Render the upstream demo.  lv_demo_widgets() creates a tab
     * view with five tabs (Profile, Analytics, Shop, ...), each
     * exercising a different widget family.  Returns immediately;
     * subsequent UI updates are driven by lv_task_handler() below. */
	lv_demo_widgets();

	/* Turn the backlight on (board-specific GPIO; the Zephyr
     * display driver handles it via the `backlight-gpios` property
     * in the devicetree overlay if one is declared).  The display
     * starts off blank either way until LVGL renders the first
     * frame on the next handler tick. */
	display_blanking_off(display);

	/* Main loop: tick LVGL at the configured refresh rate.  Zephyr
     * gives us a real RTOS so we can sleep precisely between ticks
     * instead of busy-waiting.  The 10 ms slice keeps animations
     * smooth at 30 fps with headroom for input handlers. */
	while (1) {
		const uint32_t sleep_ms = lv_task_handler();
		k_msleep(MIN(sleep_ms, 10u));
	}

	return 0; /* unreachable. */
}
