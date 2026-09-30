/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bring the E1M EVK's HX8394 DSI panel (lcd_panel, e1m_evk_rk055hdmipi4ma0
 * shield) up with retries before main().
 *
 * Why: hx8394_init() returns -EIO when one DCS write stalls in the DSI
 * host's command FIFO ("Failed to write command FIFO", #2199). That happens
 * on some cold boots, and on every warm re-init of a panel another image
 * already drove. Zephyr never retries a failed boot-time device init, so the
 * app is left with a dead panel and the backlight off (the driver enables
 * bl-gpios only after the whole DCS sequence succeeds).
 *
 * How: the shield marks lcd_panel zephyr,deferred-init, so the kernel skips
 * it at POST_KERNEL and this APPLICATION-level hook initialises it. It must
 * run before the app's first display_blanking_off(): that switches the DSI
 * host to video mode, and the panel init re-attaches the host in command
 * mode. Attempt 0 is device_init(). device_init() refuses a second call and
 * the driver has no deinit, so each retry holds RESX (the driver's own
 * reset-gpios) low, then re-runs the driver's init entry, which does its
 * normal reset pulse and DCS sequence; on success the stale init_res is
 * cleared so device_is_ready() tells the truth. No DCS of its own is sent --
 * never SETOTP (0xBB) or SETID (0xC3), nothing that writes panel OTP.
 *
 * After a successful init the backlight enable is (re)set to a static high.
 * Never pulsed: short low pulses are the backlight driver's dimming protocol.
 *
 * Same approach, bench-proven over cold boots, as the Trace Runner demo's
 * panel bring-up.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define PANEL_NODE DT_NODELABEL(lcd_panel)

#if DT_NODE_HAS_STATUS_OKAY(PANEL_NODE) && DT_NODE_HAS_COMPAT(PANEL_NODE, himax_hx8394) && \
    DT_PROP(PANEL_NODE, zephyr_deferred_init)

#define PANEL_MAX_TRIES     6
#define PANEL_RESET_HOLD_MS 10 /* RESX low before a retry; the part needs >= 10 us */

static int alp_panel_init_retry(void)
{
	const struct device *const panel = DEVICE_DT_GET(PANEL_NODE);
	const struct gpio_dt_spec  rst   = GPIO_DT_SPEC_GET(PANEL_NODE, reset_gpios);
	const struct gpio_dt_spec  bl    = GPIO_DT_SPEC_GET(PANEL_NODE, bl_gpios);
	int                        rc    = -ENODEV;

	for (unsigned int attempt = 0; attempt < PANEL_MAX_TRIES; attempt++) {
		if (attempt == 0) {
			rc = device_init(panel);
		} else {
			(void)gpio_pin_configure_dt(&rst, GPIO_OUTPUT_INACTIVE);
			k_msleep(PANEL_RESET_HOLD_MS);
			rc = panel->ops.init(panel);
			if (rc == 0) {
				panel->state->init_res = 0;
			}
		}
		if (rc == 0 && !device_is_ready(panel)) {
			rc = -ENODEV;
		}
		if (rc == 0) {
			if (attempt > 0) {
				printk("panel: init succeeded on attempt %u/%u\n", attempt + 1, PANEL_MAX_TRIES);
			}
			break;
		}
		printk("panel: init attempt %u/%u -> %d\n", attempt + 1, PANEL_MAX_TRIES, rc);
	}
	if (rc != 0) {
		return 0; /* the app sees !device_is_ready(panel) and reports it */
	}

	/* Static high, idempotent when the driver already set it. */
	(void)gpio_pin_configure_dt(&bl, GPIO_OUTPUT_ACTIVE);
	return 0;
}

SYS_INIT(alp_panel_init_retry, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif
