/*
 * src/platform/panel.c -- brings the HX8394 panel up with retries.
 *
 * Why: on E1M-AEN803 2026W36-0009 about 1 cold boot in 2-14 left
 * the backlight OFF with the game, the CDC200 and DSI video all running:
 * GPIO5 DR/DDR (0x49005000/04) read 0/0 on those boots, 0x20/0x20 on good
 * ones -- P5_5, the shield's bl-gpios. Zephyr's hx8394_init() drives
 * bl-gpios only after every DCS write has succeeded; any failed write
 * returns -EIO first (the #2199 intermittent init stall), and a boot-time
 * device init is never retried.
 *
 * The hook: panel_deferred.overlay marks the panel zephyr,deferred-init, so
 * the kernel skips it at POST_KERNEL and main() calls tr_panel_up() before
 * tr_display_open(). It must run first: alp_display_open() ->
 * display_blanking_off(cdc200) switches the DSI host to video mode, and the
 * panel driver's init re-attaches the host (dsi_dw_attach powers it down
 * into command mode), so a panel init after the video switch would stop the
 * link. Nothing else references the panel device: the CDC200 and the DSI
 * host are initialised by the kernel as before, and only the panel driver
 * attaches the host.
 *
 * The retry: attempt 0 is device_init(). device_init() refuses a second call
 * (-EALREADY, the device is marked initialised even on failure) and the
 * hx8394 has no deinit, so each retry holds RESX (expander U35 P1, the
 * driver's own reset-gpios) low, then re-runs the driver's own init entry
 * (panel->ops.init == hx8394_init), which re-attaches the DSI host and does
 * its normal reset pulse (1 ms low, high, 50 ms) and DCS sequence. On
 * success the stale init_res is cleared so device_is_ready() tells the
 * truth. This path sends NO DCS of its own: never SETOTP (0xBB) or SETID
 * (0xC3), no OTP write of any kind -- only the driver's normal init.
 *
 * After a successful init the backlight pin is checked in the GPIO5
 * registers (output + high), and forced to a static high through the
 * panel's bl-gpios spec if the driver did not leave it so. Never pulsed:
 * short low pulses are the backlight driver's dimming protocol.
 *
 * tr_panel_init_tries is the bench-readable result (panel_retry.h layout).
 */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>

#include "platform/panel_retry.h"

#ifdef TR_PANEL_RVT121
/* Riverdi RVT121 (CMake -DTR_PANEL=rvt121): the shield's SN65DSI83 driver
 * brings the panel up at boot; there is no HX8394 and no lcd_panel node. The
 * only job left is the backlight: the shield's `backlight` pwm-leds node is a
 * UTIMER3 PWM on P10_7 (500 Hz), set to 30% duty here, once, as in
 * aen-lvds-display. */
#include <zephyr/drivers/pwm.h>

#define TR_BACKLIGHT_DUTY_PERCENT 30U

volatile uint32_t tr_panel_init_tries;

static const struct pwm_dt_spec backlight = PWM_DT_SPEC_GET(DT_NODELABEL(backlight));

uint32_t tr_panel_up(void)
{
	int rc = -ENODEV;

	if (pwm_is_ready_dt(&backlight)) {
		rc = pwm_set_dt(
		    &backlight, backlight.period, backlight.period * TR_BACKLIGHT_DUTY_PERCENT / 100U);
	}
	printk("panel   : backlight %u%% duty -> %d\n", TR_BACKLIGHT_DUTY_PERCENT, rc);
	tr_panel_init_tries = TR_PANEL_OK | 1u;
	return tr_panel_init_tries;
}
#else
#define PANEL_NODE DT_NODELABEL(lcd_panel)
#define BL_NODE    DT_GPIO_CTLR(PANEL_NODE, bl_gpios)

BUILD_ASSERT(DT_PROP(PANEL_NODE, zephyr_deferred_init),
             "panel_deferred.overlay must mark lcd_panel zephyr,deferred-init");

#define TR_PANEL_MAX_TRIES     4
#define TR_PANEL_RESET_HOLD_MS 10 /* RESX low before a retry; driver needs >= 10 us */

/* DesignWare GPIO: port A data (DR) and direction (DDR) registers. */
#define GPIO_DW_DR             0x00u
#define GPIO_DW_DDR            0x04u

volatile uint32_t tr_panel_init_tries;

static const struct device *const panel = DEVICE_DT_GET(PANEL_NODE);
static const struct gpio_dt_spec  rst   = GPIO_DT_SPEC_GET(PANEL_NODE, reset_gpios);
static const struct gpio_dt_spec  bl    = GPIO_DT_SPEC_GET(PANEL_NODE, bl_gpios);

static int panel_init(void *ctx, unsigned attempt)
{
	int rc;

	ARG_UNUSED(ctx);
	if (attempt == 0) {
		rc = device_init(panel);
	} else {
		(void)gpio_pin_configure_dt(&rst, GPIO_OUTPUT_INACTIVE); /* RESX low */
		k_msleep(TR_PANEL_RESET_HOLD_MS);
		rc = panel->ops.init(panel);
		if (rc == 0) {
			panel->state->init_res = 0;
		}
	}
	if (rc == 0 && !device_is_ready(panel)) {
		rc = -ENODEV;
	}
	printk("panel   : init attempt %u/%u -> %d\n", attempt + 1, TR_PANEL_MAX_TRIES, rc);
	return rc;
}

static bool panel_bl_on(void *ctx)
{
	uint32_t base = DT_REG_ADDR(BL_NODE);
	uint32_t bit  = BIT(bl.pin);

	ARG_UNUSED(ctx);
	return (sys_read32(base + GPIO_DW_DDR) & bit) && (sys_read32(base + GPIO_DW_DR) & bit);
}

static int panel_bl_force(void *ctx)
{
	ARG_UNUSED(ctx);
	printk("panel   : backlight pin not output-high after init -- forcing it\n");
	return gpio_pin_configure_dt(&bl, GPIO_OUTPUT_ACTIVE);
}

uint32_t tr_panel_up(void)
{
	static const tr_panel_ops_t ops = { NULL, panel_init, panel_bl_on, panel_bl_force };
	uint32_t                    w   = tr_panel_bringup(&ops, TR_PANEL_MAX_TRIES);

	tr_panel_init_tries = w;
	printk("panel   : %s after %u attempt(s), word 0x%08x\n",
	       (w & TR_PANEL_OK) ? "up" : "FAILED",
	       (unsigned)TR_PANEL_TRIES(w),
	       (unsigned)w);
	return w;
}
#endif /* TR_PANEL_RVT121 */
