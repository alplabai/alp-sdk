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
 *      portable Alp surfaces:
 *        - <alp/display.h>  opens the panel (alp_display_open)
 *        - <alp/gui.h>      binds it to LVGL (alp_gui_lvgl_attach)
 *        - <lvgl.h>         the UI API itself
 *      No `gd32g553_*` / `alif_*` / `renesas_*` symbols, and no
 *      direct `<zephyr/drivers/display.h>` calls, appear anywhere
 *      -- per the SDK's "portable peripheral surfaces only in app
 *      + library code" rule.  Board-specific backends live inside
 *      the SDK; this file stays board-portable.
 *
 *   3. Zephyr's own `lvgl` module auto-init is turned OFF
 *      (CONFIG_LV_Z_AUTO_INIT=n in prj.conf).  Left on, it would
 *      create its OWN lv_display_t bound straight to the
 *      `zephyr,display` chosen node the moment CONFIG_LVGL=y is
 *      set -- a second writer racing alp_gui_lvgl_attach()'s
 *      display for the same panel.  With auto-init off, this file
 *      owns the whole sequence: open the panel, `lv_init()`, wire
 *      LVGL's tick source, attach, then run the demo.
 *
 *
 * ── Build matrix ───────────────────────────────────────────────
 *
 *   `west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/display/lvgl-widgets-demo`
 *   `west build -b native_sim/native/64 examples/display/lvgl-widgets-demo`
 *
 *   native_sim runs the demo against the dummy display driver --
 *   handy for exercising the LVGL + <alp/display.h> plumbing
 *   without touching hardware (no framebuffer is shown on-screen;
 *   see the per-example native_sim.conf for why).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <alp/display.h>
#include <alp/gui.h>

/* Upstream LVGL demo entrypoint -- pulled in by
 * CONFIG_LV_USE_DEMO_WIDGETS=y in prj.conf. */
#include <lv_demo_widgets.h>

LOG_MODULE_REGISTER(lvgl_widgets_demo, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("LVGL widgets demo starting");

	/* Open the panel through the portable display surface.  On the
     * E1M-EVK this resolves the ST7789 behind the MIPI DBI Type C
     * node; on native_sim it resolves the dummy display driver --
     * both wired via the `alp-display0` devicetree alias the
     * zephyr_drv backend looks up (see board.yaml + the board
     * overlay for each target). */
	alp_display_config_t display_cfg = ALP_DISPLAY_CONFIG_DEFAULT(0);
	alp_display_t       *display     = alp_display_open(&display_cfg);
	if (display == NULL) {
		LOG_ERR("display open failed (err=%d)", (int)alp_last_error());
		return 1;
	}

	/* LVGL initialisation: allocate LVGL's own global state
     * (styles, the default theme, the timer list, ...).  Normally
     * Zephyr's lvgl module does this for us from a SYS_INIT hook,
     * but that hook -- and the tick registration below -- only run
     * under CONFIG_LV_Z_AUTO_INIT=y (modules/lvgl/lvgl.c), which
     * this app turns off so it can bind its OWN display instead of
     * Zephyr's auto-created one. */
	lv_init();

	/* LVGL needs a monotonic millisecond tick to drive animations,
     * timers, and input debouncing.  Zephyr's lvgl module would
     * normally register this same callback from its SYS_INIT hook;
     * with CONFIG_LV_Z_AUTO_INIT=n that hook never runs, so wire it
     * by hand here -- same callback, just moved to the app side of
     * the auto-init boundary. */
	lv_tick_set_cb(k_uptime_get_32);

	/* Bind LVGL's renderer to the panel: allocates an lv_display_t
     * sized to the panel's reported geometry and wires its
     * flush_cb straight to alp_display_blit() (src/gui_lvgl.c). */
	alp_status_t attach_status = alp_gui_lvgl_attach(display);
	if (attach_status != ALP_OK) {
		LOG_ERR("alp_gui_lvgl_attach failed (err=%d)", (int)attach_status);
		return 1;
	}

	/* Render the upstream demo.  lv_demo_widgets() creates a tab
     * view with five tabs (Profile, Analytics, Shop, ...), each
     * exercising a different widget family.  Returns immediately;
     * subsequent UI updates are driven by lv_timer_handler() below. */
	lv_demo_widgets();

	/* Main loop: tick LVGL at the configured refresh rate.  Zephyr
     * gives us a real RTOS so we can sleep precisely between ticks
     * instead of busy-waiting.  The 10 ms slice keeps animations
     * smooth at 30 fps with headroom for input handlers.
     * lv_timer_handler() is LVGL v9's native name for what this
     * file called lv_task_handler() before the migration (the v8
     * name is still available as a compat shim, but the direct
     * name reads clearer here). */
	while (1) {
		const uint32_t sleep_ms = lv_timer_handler();
		k_msleep(MIN(sleep_ms, 10u));
	}

	return 0; /* unreachable. */
}
