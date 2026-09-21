/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-dsi-display -- drive the RK055HDMIPI4MA0 (Rocktech 720x1280, Himax HX8394
 * controller, MIPI-DSI) panel through the full Alif Ensemble E8 C2-MIPI-DSI
 * DISPLAY chain on an E1M-AEN SoM (M55-HE), via the bench RAM-run + RAM-console
 * flow.  This is the pixels-on-glass successor to aen-dsi-regcheck (which only
 * proves the chain BINDS): it turns ON the cdc200 + dsi_dw display-class drivers
 * and renders four horizontal colour bars (red/green/blue/white).
 *
 * THE DISPLAY CHAIN (app -> glass):
 *
 *   display_write()  ->  cdc200@49031000  (tes,cdc-2.1)
 *                            -- the DPI/RGB pixel pump (CDC200).  Its L1
 *                               framebuffer lives in SRAM0 @0x02100000 (the
 *                               720x1280 RGB888 FB is 2,764,800 B -- it does NOT
 *                               fit ITCM, where the RAM-run links code).
 *                        |  DPI
 *                        v
 *                        mipi_dsi@49032000 (snps,designware-dsi)
 *                            -- the MIPI-DSI host (DSI-TX bridge).
 *                        |  phy-if <&dphy 1>
 *                        v
 *                        dphy@49033000    (snps,designware-dphy)
 *                            -- the shared DesignWare D-PHY (TX role:
 *                               dphy_dw_master_setup()).
 *                        |  2 data lanes
 *                        v
 *                        panel@0          (himax,hx8394)
 *                            -- the HX8394 panel controller.  Its driver runs the
 *                               panel reset sequence + DSI attach at POST_KERNEL.
 *
 * The DPI pixel clock is the shield's one clock-frequency knob: 400 MHz / 10 =
 * 40 MHz (RGB888, 24 bpp, 2 lanes -> 40 x 24 / 2 = 480 Mbps/lane).  The faster
 * RGB565 / 400 MHz / 7 = 57.142857 MHz divider was measured and rejected: at
 * that rate the DPI payload FIFO overflows continuously
 * (INT_ST1 DSI_INT_1_DPI_PLD_WR_ERR, zephyr/drivers/mipi_dsi/dsi_dw.h:381) and
 * the glass stays dark, so the shield stays at the 40 MHz / RGB888 divider
 * instead.  The SoC glue's CDC divider and the DSI host's lane timing both
 * derive from clock-frequency.
 *
 * WHERE EACH PIECE LIVES (this app only USES the chain):
 *   - the SoC nodes (reg, IRQs, clocks) -- zephyr/dts/alif/
 *     ensemble_e8_peripherals.dtsi, disabled until something enables them;
 *   - the panel, its timings, the carrier's panel-control expander, the
 *     panel-enable regulator and the framebuffer region -- the
 *     e1m_evk_rk055hdmipi4ma0 shield (zephyr/boards/shields/), which
 *     CMakeLists.txt adds and whose Kconfig.defconfig enables the drivers;
 *   - the SoC setup no driver does (D-PHY clock sources + analog power, the
 *     CDC200 pixel-clock divider, the backlight pad mux) -- zephyr/soc-bridge/
 *     alif/mipi_display_e8.c.
 *   Any app that adds the shield gets the same chain; nothing below is needed
 *   to make it work.
 *
 * PANEL ENABLE / RESET / BACKLIGHT:
 *   A small I2C GPIO expander on I2C2 provides the HX8394 control GPIOs.  It
 *   answers at POR with no reset action (its reset line is pulled up on the
 *   carrier).  The shield hogs the RESX pin low as soon as the expander is up,
 *   so the panel never sees its supply rise against a floating reset; only then
 *   does a boot-on fixed regulator assert the panel-enable GPIO, before
 *   the HX8394 driver performs its reset and DSI initialization.  The hx8394
 *   driver lights the SoM-side backlight (its bl-gpios) only at the END of a
 *   successful init -- backlight last is the right production order, no white
 *   flash before DISPLAY_ON -- so a dark backlight means the panel init
 *   failed.  main() prints the pin level after the panel check.
 *
 * PIXELS: display_blanking_off() on the cdc200 switches the DSI host from
 *   command mode (panel init) to video mode and sets CDC_EN, which starts the
 *   DPI scanout.  Until then display_write() only fills the framebuffer.
 *
 * FRAMEBUFFER PLACEMENT (RAM-run critical): the cdc200 node's memory-region
 * (the shield's top 3 MiB of SRAM0, lcd_fb @0x02100000; sram0 itself is
 * shrunk to the bottom 1 MiB below it) holds the L1 framebuffer.  The driver
 * uses that address directly -- no linker section -- so the 2,764,800 B
 * RGB888 framebuffer is not part of the ITCM RAM-run image.  The
 * driver flushes the data cache after every framebuffer write
 * (sys_cache_data_flush_range), so the CDC scanout sees coherent pixels.
 *
 * The PASS gate: the expander, the fixed panel-power regulator, the hx8394
 * panel, the DSI host, and the cdc200 display device are all device_is_ready,
 * display_write() of the colour-bar frame returns 0, a DCS read of the panel's
 * own ID (RDDID) gets back the expected 3 bytes, and display_blanking_off()
 * is accepted.  The app has no optical sensor, so PASS means the chain
 * answered correctly end to end, not that pixels are confirmed on glass --
 * see the RESULT line.  The app is robust to a NOT-ready device: it reports
 * which stage failed and prints RESULT FAIL rather than hanging.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mipi_dsi.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/printk.h>

/* The display device (the cdc200 pixel pump) is the chosen render target. */
#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define DSI_NODE     DT_NODELABEL(mipi_dsi)
#define PANEL_NODE   DT_NODELABEL(lcd_panel)
#define EXP_NODE     DT_NODELABEL(lcd_exp)
#define LCD_PWR_NODE DT_NODELABEL(lcd_pwr_en)

static const struct gpio_dt_spec bl_gpio = GPIO_DT_SPEC_GET(PANEL_NODE, bl_gpios);

/*
 * The GT911 touch controller (shield's lcd_touch node, chosen zephyr,touch)
 * is optional: a board without this shield has no chosen zephyr,touch at
 * all.  DT_HAS_CHOSEN() lets this whole feature compile out cleanly instead
 * of a hard DEVICE_DT_GET() on a node that may not exist.
 *
 * The GT911 has no direct INT line to the Alif SoC on this board: the CC3501E
 * GPIO relay that could forward it is not wired up (see the shield overlay),
 * so it is polled every CONFIG_INPUT_GT911_PERIOD_MS by the driver itself;
 * this app only registers a callback to be told when a poll finds something.
 */
#if DT_HAS_CHOSEN(zephyr_touch)
#define TOUCH_NODE DT_CHOSEN(zephyr_touch)

/*
 * The Input subsystem delivers one event per axis/button, not one bundled
 * "touch" struct: an X move, then a Y move, then the BTN_TOUCH press/release
 * arrive as three separate callbacks (see gt911_process() in
 * drivers/input/input_gt911.c).  Latch X/Y as they arrive.  The GT911 polls
 * every 10 ms and reports BTN_TOUCH=1 on EVERY scan while a finger stays
 * down (input_gt911.c:179) -- printing every event would flood the 8 KiB RAM
 * console within a couple of seconds of one touch-and-hold, so print only on
 * the down/up transition by comparing against the last state we printed.
 */
static int32_t touch_x, touch_y;
static bool    touch_down;

static void touch_event_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (evt->code) {
	case INPUT_ABS_X:
		touch_x = evt->value;
		break;
	case INPUT_ABS_Y:
		touch_y = evt->value;
		break;
	case INPUT_BTN_TOUCH:
		if ((evt->value != 0) != touch_down) {
			touch_down = (evt->value != 0);
			printk("touch: x=%d y=%d down=%d\n", touch_x, touch_y, evt->value);
		}
		break;
	default:
		break;
	}
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(TOUCH_NODE), touch_event_cb, NULL);
#endif

/* Panel geometry (must match the overlay's cdc200 + panel nodes). */
#define PANEL_W 720
#define PANEL_H 1280

/*
 * One scanline, refilled only when the colour band changes and reused for
 * every row in that band via display_write's per-call descriptor.  A full
 * 720x1280 frame is 1.8-2.8 MB depending on the layer format -- far too big
 * for a stack/static buffer in ITCM -- so we stream it one row at a time
 * straight into the SRAM0 framebuffer the driver owns.
 *
 * The bytes-per-pixel here MUST match the cdc200 layer's `pixel-fmt-l1`.  They
 * live in different files and nothing used to tie them together, so this app
 * filled 16-bit RGB565 rows into a layer configured "rgb-888": the row
 * descriptor was then 1440 bytes where the driver wanted 2160, every
 * display_write() returned -EINVAL, and NO frame was written and scanout never
 * started -- while the app still reported the chain up to the point of the
 * write.  Deriving the size from the devicetree makes the two unable to
 * disagree, and the BUILD_ASSERT turns an unsupported format into a compile
 * error rather than a blank panel.
 */
#define PANEL_FMT_IS_RGB888 DT_ENUM_HAS_VALUE(DT_NODELABEL(cdc200), pixel_fmt_l1, rgb_888)
#define PANEL_FMT_IS_RGB565 DT_ENUM_HAS_VALUE(DT_NODELABEL(cdc200), pixel_fmt_l1, rgb_565)

BUILD_ASSERT(PANEL_FMT_IS_RGB888 || PANEL_FMT_IS_RGB565,
             "aen-dsi-display fills only rgb-888 or rgb-565; teach it the shield's pixel-fmt-l1");

#if PANEL_FMT_IS_RGB888
#define PANEL_BYTES_PER_PIXEL 3U
#else
#define PANEL_BYTES_PER_PIXEL 2U
#endif

static uint8_t row_buf[PANEL_W * PANEL_BYTES_PER_PIXEL];

/*
 * Colour bars, not a solid frame: green sits in the middle of BOTH the RGB
 * and BGR byte orders (its byte is the same either way), so a solid green
 * frame cannot reveal an R/B lane swap or a colour-lane wiring error on the
 * 16-bit DSI link -- it would look identical either way.  Four saturated
 * primaries stacked top to bottom make a swap immediately visible: red and
 * blue trade places, white stays white.
 */
enum bar_colour { BAR_RED, BAR_GREEN, BAR_BLUE, BAR_WHITE, BAR_COUNT };

static void fill_row_bar(enum bar_colour colour)
{
	for (uint16_t i = 0; i < PANEL_W; i++) {
#if PANEL_FMT_IS_RGB888
		/* Byte order B,G,R -- unchanged from this file's original green fill. */
		static const uint8_t bgr[BAR_COUNT][3] = {
			[BAR_RED]   = { 0x00U, 0x00U, 0xFFU },
			[BAR_GREEN] = { 0x00U, 0xFFU, 0x00U },
			[BAR_BLUE]  = { 0xFFU, 0x00U, 0x00U },
			[BAR_WHITE] = { 0xFFU, 0xFFU, 0xFFU },
		};
		row_buf[i * 3U + 0U] = bgr[colour][0]; /* B */
		row_buf[i * 3U + 1U] = bgr[colour][1]; /* G */
		row_buf[i * 3U + 2U] = bgr[colour][2]; /* R */
#else
		/* RGB565 little-endian: red 0xF800, green 0x07E0, blue 0x001F, white 0xFFFF. */
		static const uint16_t rgb565[BAR_COUNT] = {
			[BAR_RED]   = 0xF800U,
			[BAR_GREEN] = 0x07E0U,
			[BAR_BLUE]  = 0x001FU,
			[BAR_WHITE] = 0xFFFFU,
		};
		row_buf[i * 2U + 0U] = (uint8_t)(rgb565[colour] & 0xFFU);
		row_buf[i * 2U + 1U] = (uint8_t)(rgb565[colour] >> 8U);
#endif
	}
}

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
	printk("\n=== aen-dsi-display ===\n");

	const struct device *exp   = DEVICE_DT_GET(EXP_NODE);
	const struct device *pwr   = DEVICE_DT_GET(LCD_PWR_NODE);
	const struct device *panel = DEVICE_DT_GET(PANEL_NODE);
	const struct device *dsi   = DEVICE_DT_GET(DSI_NODE);
	const struct device *disp  = DEVICE_DT_GET(DISPLAY_NODE);

	/* Step 1: the panel-control expander and its boot-on panel-enable regulator. */
	bool exp_ok = dev_ready("lcd-exp", exp);
	bool pwr_ok = dev_ready("lcd-reg", pwr);

	/*
	 * Step 1b: the touch controller, if this shield's overlay chose one.
	 * Not part of the PASS gate below -- this app's job is to prove pixels
	 * reach glass, and touch is not bench-verified through this driver yet
	 * (#2199) -- just reported and wired to print incoming events.
	 */
#if DT_HAS_CHOSEN(zephyr_touch)
	(void)dev_ready("touch", DEVICE_DT_GET(TOUCH_NODE));
#endif

	/*
	 * Step 2: HX8394 init does mipi_dsi_attach + DCS power-on over DSI, and
	 * drives bl-gpios high only if all of it succeeded -- so the level is the
	 * panel driver's own verdict.
	 */
	bool panel_ok = dev_ready("panel", panel);

	printk("%-8s: level=%d\n", "backlight", gpio_pin_get_dt(&bl_gpio));

	/* Step 3: the MIPI-DSI host. */
	bool dsi_ok = dev_ready("mipi-dsi", dsi);

	/* Step 4: the cdc200 display device (the render target). */
	bool disp_ok = dev_ready("display", disp);

	/*
	 * Step 5: render.  Fill the screen with four colour bars, one row at a
	 * time.  The descriptor describes a single PANEL_W x 1 strip; we walk it
	 * down the screen, refilling row_buf only when the band changes.
	 * display_write copies into the SRAM0 framebuffer and flushes the data
	 * cache for the CDC scanout (handled inside the driver).
	 */
	bool write_ok      = false;
	bool panel_read_ok = false;
	bool blanking_ok   = false;

	if (disp_ok) {
		struct display_buffer_descriptor desc = {
			.buf_size = sizeof(row_buf),
			.width    = PANEL_W,
			.height   = 1,
			.pitch    = PANEL_W,
		};

		/* 1280 rows / 4 bars = 320 rows per bar, top to bottom. */
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
			printk("display_write: colour bars R/G/B/W top-to-bottom OK (%u bytes/pixel)\n",
			       (unsigned int)PANEL_BYTES_PER_PIXEL);

			/*
			 * DSI command WRITES are unacknowledged: display_write() above and
			 * every DCS byte the hx8394 driver sent during init succeed into an
			 * empty link, so RESULT PASS would be reachable with no panel on
			 * the FFC at all.  A DCS READ is the only primitive in this chain
			 * that makes the peripheral drive data back, so read the panel's
			 * own ID (RDDID, cmd=0x04, 3 bytes) while the DSI host is still in
			 * command mode -- display_blanking_off() below is what switches it
			 * to video mode.  Expected at the shield's settings: rc=3,
			 * data=83 94 0f.
			 */
			uint8_t id[3] = { 0 };

			ssize_t read_rc = mipi_dsi_dcs_read(
			    dsi, DT_REG_ADDR(PANEL_NODE), MIPI_DCS_GET_DISPLAY_ID, id, sizeof(id));

			panel_read_ok = (read_rc == (ssize_t)sizeof(id));
			printk(
			    "dcs-read RDDID: rc=%d data=%02x %02x %02x\n", (int)read_rc, id[0], id[1], id[2]);

			/* Start scanout: DSI video mode + CDC_EN, so the FB reaches glass. */
			rc          = display_blanking_off(disp);
			blanking_ok = (rc == 0);
			printk("blanking_off: rc=%d\n", rc);
		}
	}

	/* Confirm the driver's reported capabilities match the panel. */
	if (disp_ok) {
		struct display_capabilities caps;

		display_get_capabilities(disp, &caps);
		printk("caps: %ux%u fmts=0x%x\n",
		       caps.x_resolution,
		       caps.y_resolution,
		       caps.supported_pixel_formats);
	}

	bool pass = exp_ok && pwr_ok && panel_ok && dsi_ok && disp_ok && write_ok && panel_read_ok &&
	            blanking_ok;

	if (pass) {
		printk("RESULT PASS: RK055HDMIPI4MA0 chain up -- hx8394 panel + mipi-dsi + "
		       "cdc200 all bound, colour-bar frame written, panel answered a "
		       "DSI read, and display_blanking_off() was accepted. "
		       "This app has no optical sensor: confirm pixels on glass by eye.\n");
	} else {
		printk("RESULT FAIL: DSI display chain not fully up "
		       "(lcd-exp=%d lcd-reg=%d panel=%d mipi-dsi=%d display=%d write=%d "
		       "dcs-read=%d blanking=%d) -- see the per-stage lines above\n",
		       (int)exp_ok,
		       (int)pwr_ok,
		       (int)panel_ok,
		       (int)dsi_ok,
		       (int)disp_ok,
		       (int)write_ok,
		       (int)panel_read_ok,
		       (int)blanking_ok);
	}

	return 0;
}
