/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-lvds-display -- drive the Riverdi RVT121HVDFWCA0-B (12.1" 1280x800
 * LVDS) panel through the Alif Ensemble E8 MIPI-DSI chain PLUS a TI
 * SN65DSI83 DSI-to-LVDS bridge on a maintainer-built adapter PCB, on an
 * E1M-AEN SoM (M55-HE), via the bench RAM-run + RAM-console flow.  Renders
 * four horizontal colour bars (red/green/blue/white), the same PASS-gate
 * shape as aen-dsi-display -- but the chain and the app are otherwise NOT
 * the same code, because this panel is not a DSI peripheral at all.
 *
 * THE DISPLAY CHAIN (app -> glass):
 *
 *   display_write()  ->  cdc200@49031000  (tes,cdc-2.1)
 *                            -- the DPI/RGB pixel pump.  Its L1 framebuffer
 *                               lives in SRAM0 @0x02200000 (1280x800 RGB565 =
 *                               2,048,000 B -- does NOT fit ITCM, where the
 *                               RAM-run links code).
 *                        |  DPI
 *                        v
 *                        mipi_dsi@49032000 (snps,designware-dsi)
 *                            -- the MIPI-DSI host (DSI-TX bridge).
 *                        |  phy-if <&dphy 1>
 *                        v
 *                        dphy@49033000    (snps,designware-dphy)
 *                            -- the shared DesignWare D-PHY (TX role).
 *                        |  2 data lanes, RGB666 packed (18 bpp), burst
 *                        v
 *                        bridge@2c        (ti,sn65dsi83, I2C1, on the
 *                                          adapter PCB)
 *                            -- converts DSI to single-link FlatLink LVDS.
 *                               INIT-ONLY: it runs its own CSR programming
 *                               sequence once, at POST_KERNEL/APPLICATION
 *                               priority, and implements no Zephyr
 *                               display-class API -- see
 *                               display_sn65dsi83.c's header comment for the
 *                               full EN-pin/clock-lane ordering this
 *                               requires.
 *                        |  FlatLink single-link LVDS, VESA-24
 *                        v
 *                        Riverdi RVT121HVDFWCA0-B panel (off-shield)
 *
 * WHY THIS APP HAS NO DCS READ (unlike aen-dsi-display's RDDID check): a DSI
 * *panel* answers Display Command Set reads over the same DSI link its
 * pixels ride.  This bridge is not a DSI panel -- it is controlled entirely
 * over I2C1, and it has no `panel@N` devicetree node at all (see the shield
 * overlay).  The equivalent proof that the chain answered for real, not just
 * accepted unacknowledged writes into an empty link, is
 * sn65dsi83_read_errors(): it is a genuine I2C *read* of the bridge's own
 * link-error register (CSR 0xE5), so it can only succeed if a real
 * SN65DSI83 is on the bus and answering.
 *
 * THE ID CHECK ALREADY HAPPENED, IMPLICITLY: display_sn65dsi83.c's init
 * reads CSR 0x00..0x08 and compares it against the bridge's fixed ID bytes
 * BEFORE it writes a single configuration register (see that driver's
 * sn65dsi83_check_id()) -- so `device_is_ready(bridge)` below already means
 * "a real SN65DSI83 answered and every CSR write it needed afterwards
 * succeeded".  There is nothing left for this app to re-check at that level;
 * sn65dsi83_read_errors() instead answers a DIFFERENT question -- whether
 * the link is STILL clean after display_blanking_off() re-starts the DSI
 * clock lane and the CDC begins scanning out, which init cannot see because
 * it runs before either of those things happens.
 *
 * PANEL POWER / EN SEQUENCING: unlike aen-dsi-display's HX8394 panel driver
 * (reset pulse, then DCS init), this bridge's own driver owns the entire
 * power-up: EN low, attach + clock-lane HS, EN high, ID check, CSR program,
 * PLL lock, soft reset.  All of it runs inside device_is_ready(bridge) --
 * there is no separate "step 2" for this app to narrate the way
 * aen-dsi-display narrates the HX8394's reset/backlight sequence.
 *
 * FRAMEBUFFER PLACEMENT (RAM-run critical) and the PASS gate follow the same
 * shape as aen-dsi-display -- see that example if either needs more detail
 * than repeated here.  The app has no optical sensor: PASS means the chain
 * answered correctly end to end, not that pixels are confirmed on glass --
 * see the RESULT line.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/display/sn65dsi83.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/printk.h>

/* The display device (the cdc200 pixel pump) is the chosen render target. */
#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define DSI_NODE     DT_NODELABEL(mipi_dsi)
#define BRIDGE_NODE  DT_NODELABEL(bridge)
#define EXP_NODE     DT_NODELABEL(lcd_exp)
#define TOUCH_NODE   DT_NODELABEL(touch)

/* Panel geometry (must match the shield's cdc200 node). */
#define PANEL_W 1280
#define PANEL_H 800

/*
 * One scanline of RGB565 (2 bytes/pixel), refilled only when the colour band
 * changes and reused for every row in that band -- the same streaming
 * approach aen-dsi-display uses, for the same reason: a full 1280x800 RGB565
 * frame is 2,048,000 B, far too big for a stack/static buffer in ITCM.
 *
 * Unlike aen-dsi-display, this shield has exactly one supported framebuffer
 * format (pixel-fmt-l1 = "rgb-565" in the shield overlay, not selectable),
 * so there is no BUILD_ASSERT-guarded #if here for a second format -- the
 * DSI LINK format (RGB666 packed, bridge@2c's pixel-format) is a separate
 * decision from the framebuffer's bytes-per-pixel and does not change this.
 */
static uint8_t row_buf[PANEL_W * 2U];

/*
 * Colour bars, not a solid frame: green sits in the middle of BOTH the RGB
 * and BGR byte orders, so a solid green frame cannot reveal an R/B swap
 * anywhere in the chain (aen-dsi-display's main.c explains this in full).
 * RGB565 little-endian.
 */
enum bar_colour { BAR_RED, BAR_GREEN, BAR_BLUE, BAR_WHITE, BAR_COUNT };

static void fill_row_bar(enum bar_colour colour)
{
	static const uint16_t rgb565[BAR_COUNT] = {
		[BAR_RED]   = 0xF800U,
		[BAR_GREEN] = 0x07E0U,
		[BAR_BLUE]  = 0x001FU,
		[BAR_WHITE] = 0xFFFFU,
	};

	for (uint16_t i = 0; i < PANEL_W; i++) {
		row_buf[i * 2U + 0U] = (uint8_t)(rgb565[colour] & 0xFFU);
		row_buf[i * 2U + 1U] = (uint8_t)(rgb565[colour] >> 8U);
	}
}

/*
 * Touch: the ILI2511 input driver polls the controller and emits Zephyr input
 * events (INPUT_ABS_X/Y + INPUT_BTN_TOUCH).  An app never reads the
 * controller itself -- it registers a callback on the touch device and gets
 * one event per axis, with `sync` set on the last event of a report.  So
 * collect X and Y, and print once per report.  Touch the glass on the bench
 * and watch 'ram_console_buf' for "touch:" lines.
 */
static void on_touch(struct input_event *evt, void *user_data)
{
	static uint16_t x;
	static uint16_t y;
	static bool     down;

	ARG_UNUSED(user_data);

	switch (evt->code) {
	case INPUT_ABS_X:
		x = (uint16_t)evt->value;
		break;
	case INPUT_ABS_Y:
		y = (uint16_t)evt->value;
		break;
	case INPUT_BTN_TOUCH:
		down = (evt->value != 0);
		break;
	default:
		break;
	}
	if (evt->sync) {
		printk("touch: %s x=%u y=%u\n", down ? "down" : "up  ", x, y);
	}
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(TOUCH_NODE), on_touch, NULL);

static bool dev_ready(const char *name, const struct device *dev)
{
	if (dev == NULL) {
		printk("%-8s: <none> (node disabled or no driver built)\n", name);
		return false;
	}
	if (!device_is_ready(dev)) {
		printk("%-8s: present but NOT ready\n", name);
		return false;
	}
	printk("%-8s: READY\n", name);
	return true;
}

int main(void)
{
	printk("\n=== aen-lvds-display ===\n");

	const struct device *exp    = DEVICE_DT_GET(EXP_NODE);
	const struct device *bridge = DEVICE_DT_GET(BRIDGE_NODE);
	const struct device *dsi    = DEVICE_DT_GET(DSI_NODE);
	const struct device *disp   = DEVICE_DT_GET(DISPLAY_NODE);

	/* Step 1: the panel-control expander (the bridge's EN pin lives behind it). */
	bool exp_ok = dev_ready("lcd-exp", exp);

	/*
	 * Step 2: the SN65DSI83 bridge.  Its own driver already did the entire
	 * EN-pin/clock-lane sequencing, an I2C read that verified a real bridge
	 * answered, and the whole CSR programming sequence -- device_is_ready()
	 * is the verdict on all of it at once (see the file header comment).
	 */
	bool bridge_ok = dev_ready("bridge", bridge);

	/* Step 3: the MIPI-DSI host. */
	bool dsi_ok = dev_ready("mipi-dsi", dsi);

	/* Step 4: the cdc200 display device (the render target). */
	bool disp_ok = dev_ready("display", disp);

	/*
	 * Step 5: render.  Fill the screen with four colour bars, one row at a
	 * time, the same streaming approach as aen-dsi-display.
	 */
	bool write_ok    = false;
	bool blanking_ok = false;

	if (disp_ok) {
		struct display_buffer_descriptor desc = {
			.buf_size = sizeof(row_buf),
			.width    = PANEL_W,
			.height   = 1,
			.pitch    = PANEL_W,
		};

		/* 800 rows / 4 bars = 200 rows per bar, top to bottom. */
		BUILD_ASSERT((PANEL_H % BAR_COUNT) == 0,
		             "PANEL_H must be a multiple of BAR_COUNT, or the last "
		             "rows compute a bar_colour past BAR_WHITE and index the "
		             "colour tables out of bounds");
		const uint16_t  rows_per_bar = PANEL_H / BAR_COUNT;
		enum bar_colour band         = BAR_COUNT; /* invalid: forces the first fill */
		int             rc           = 0;

		for (uint16_t y = 0; y < PANEL_H; y++) {
			enum bar_colour want = (enum bar_colour)(y / rows_per_bar);

			if (want != band) {
				band = want;
				fill_row_bar(band);
			}

			rc = display_write(disp, 0, y, &desc, row_buf);
			if (rc != 0) {
				printk("display_write row %u failed (%d)\n", y, rc);
				break;
			}
		}

		if (rc == 0) {
			write_ok = true;
			printk("display_write: colour bars R/G/B/W top-to-bottom written "
			       "(2 bytes/pixel)\n");

			/*
			 * Start scanout: the DSI host is ALREADY in video mode (the
			 * bridge driver's init put it there before EN ever went high --
			 * see display_sn65dsi83.c), so dsi_dw_set_mode() inside
			 * cdc200_blanking_off() early-returns 0 here and only CDC_EN
			 * actually changes.
			 */
			rc          = display_blanking_off(disp);
			blanking_ok = (rc == 0);
			printk("blanking_off: rc=%d\n", rc);
		}
	}

	/*
	 * Step 6: the one genuine I2C READ this app makes of the bridge -- its
	 * link-error register, CSR 0xE5.  Unlike a DCS read, this says nothing
	 * about the PANEL (the bridge cannot see past its own LVDS output), but
	 * it is real evidence the bridge is alive and reporting a clean link
	 * (or is not) after blanking_off restarted the pixel feed -- something
	 * the driver's own init, which runs before any of that, cannot observe.
	 */
	bool    err_read_ok = false;
	uint8_t err_status  = 0xFFU;

	if (bridge_ok) {
		int rc = sn65dsi83_read_errors(bridge, &err_status);

		err_read_ok = (rc == 0);
		printk("sn65dsi83 CSR 0xE5 (link errors): rc=%d value=0x%02x%s\n",
		       rc,
		       err_status,
		       (rc == 0 && err_status != 0U) ? " -- NONZERO, see the datasheet's "
		                                       "IRQ/error table for the bit meaning"
		                                     : "");
	}

	/* Touch is reported, not gated: a dead touch controller must not hide a
	 * working display (the RK055 bring-up used a silent touch as the tell). */
	(void)dev_ready("touch", DEVICE_DT_GET(TOUCH_NODE));

	/* Confirm the driver's reported capabilities match the panel. */
	if (disp_ok) {
		struct display_capabilities caps;

		display_get_capabilities(disp, &caps);
		printk("caps: %ux%u fmts=0x%x\n",
		       caps.x_resolution,
		       caps.y_resolution,
		       caps.supported_pixel_formats);
	}

	/*
	 * err_status is deliberately NOT part of the pass/fail gate: a nonzero
	 * CSR 0xE5 is worth printing (and worth investigating), but this app has
	 * no error-recovery policy to attach a hard failure to, and a latched
	 * bit here does not mean the frame above never reached the panel --
	 * only that the bridge itself is not fully clean.
	 */
	bool pass = exp_ok && bridge_ok && dsi_ok && disp_ok && write_ok && blanking_ok && err_read_ok;

	if (pass) {
		printk("RESULT PASS: RVT121HVDFWCA0-B chain up -- SN65DSI83 bridge + mipi-dsi + "
		       "cdc200 all bound, colour-bar frame written, the bridge answered a CSR "
		       "read, and display_blanking_off() was accepted. This app has no optical "
		       "sensor: confirm pixels on glass by eye.\n");
	} else {
		printk("RESULT FAIL: LVDS display chain not fully up "
		       "(lcd-exp=%d bridge=%d mipi-dsi=%d display=%d write=%d blanking=%d "
		       "err-read=%d) -- see the per-stage lines above\n",
		       (int)exp_ok,
		       (int)bridge_ok,
		       (int)dsi_ok,
		       (int)disp_ok,
		       (int)write_ok,
		       (int)blanking_ok,
		       (int)err_read_ok);
	}

	return 0;
}
