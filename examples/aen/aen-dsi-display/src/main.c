/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-dsi-display -- drive the RK055HDMIPI4MA0 (Rocktech 720x1280, Himax HX8394
 * controller, MIPI-DSI) panel through the full Alif Ensemble E8 C2-MIPI-DSI
 * DISPLAY chain on the E1M-AEN801 (M55-HE), via the bench RAM-run + RAM-console
 * flow.  This is the pixels-on-glass successor to aen-dsi-regcheck (which only
 * proved the chain BINDS): it turns ON the cdc200 + dsi_dw display-class drivers
 * and renders a solid-color framebuffer.
 *
 * THE DISPLAY CHAIN (app -> glass):
 *
 *   display_write()  ->  cdc200@49031000  (tes,cdc-2.1)
 *                            -- the DPI/RGB pixel pump (CDC200).  Its L1
 *                               framebuffer lives in SRAM0 @0x02000000 (the
 *                               720x1280 RGB565 FB is 1.84 MB -- it does NOT fit
 *                               ITCM, where the RAM-run links code).
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
 * PANEL POWER / RESET (TI TCAL9538 expander, U35, on I2C0 @0x72):
 *   - expander P0 = LCD_PWR_EN (active-high) -- panel power; the on-module
 *     backlight boost comes up with it (no dedicated backlight GPIO).
 *   - expander P1 = LCD_RST -- panel reset; active-LOW at the panel through
 *     level-shifter U5.  Consumed by the hx8394 driver via reset-gpios.
 *   The boot order matters: power (P0) MUST be asserted before reset (P1) is
 *   deasserted.  We model P0 two ways for robustness: (1) a regulator-fixed
 *   (regulator-boot-on) in the overlay brings the rail up at boot, and (2) main()
 *   ALSO asserts P0 explicitly below before checking the panel -- a teaching
 *   demonstration of the sequence and a guard if init ordering surprises us.
 *
 * FRAMEBUFFER PLACEMENT (RAM-run critical -- see CMakeLists.txt for the full
 * note): the cdc200 driver is built with -DNO_RELOCATE_SRAM0, which pins the L1
 * framebuffer at a FIXED SRAM0 address (0x02000000) with no linker section.  The
 * driver flushes the data cache after every framebuffer write
 * (sys_cache_data_flush_range), so the CDC scanout sees coherent pixels.
 *
 * BENCH-TUNABLES / TBD (none block the build; all gate pixels-on-glass):
 *   - DPI pixel clock: the panel-native 62.346 MHz is NOT exactly reachable from
 *     the Alif 400/480 MHz clock tree (nearest ~66.67/60 MHz).  The achieved
 *     rate is a bench knob; the overlay carries the panel-native value.
 *   - cdc200/mipi_dsi clock ids are the real re-authored ALIF_*_CLK values, but
 *     their on-silicon effect is BENCH-UNVERIFIED.
 *   - Expander \RESET (SoC ball N1 "SPI0_CS1") GPIO is UNKNOWN -- omitted.
 *   - The LCD_RST polarity through level-shifter U5 (overlay assumes the net is
 *     active-LOW at the panel) is bench-unconfirmed.
 *
 * The PASS gate: the expander, the DSI host, AND the cdc200 display device are
 * all device_is_ready, and display_write() of a solid-color frame returns 0.
 * The app is robust to a NOT-ready device: it reports which stage failed and
 * prints RESULT FAIL rather than hanging.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

/* The display device (the cdc200 pixel pump) is the chosen render target. */
#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define DSI_NODE     DT_NODELABEL(mipi_dsi)
#define EXP_NODE     DT_NODELABEL(lcd_exp)

/* Panel geometry (must match the overlay's cdc200 + panel nodes). */
#define PANEL_W 720
#define PANEL_H 1280

/*
 * LCD_PWR_EN = expander P0, active-high.  A GPIO_DT_SPEC built directly on the
 * expander GPIO controller + pin 0, so main() can assert panel power explicitly
 * (the regulator-fixed in the overlay also drives this rail at boot).
 */
static const struct gpio_dt_spec lcd_pwr_en = {
	.port     = DEVICE_DT_GET(EXP_NODE),
	.pin      = 0,
	.dt_flags = GPIO_ACTIVE_HIGH,
};

/*
 * One scanline of solid color, reused for every row via display_write's
 * per-call descriptor.  A full 720x1280 RGB565 frame is 1.84 MB -- far too big
 * for a stack/static buffer in ITCM, so we stream it one row at a time straight
 * into the SRAM0 framebuffer the driver owns.  RGB565 little-endian: 0xF800=red,
 * 0x07E0=green, 0x001F=blue, 0xFFFF=white.
 */
#define FILL_COLOR_RGB565 0x07E0U /* solid green -- easy to spot on glass */

static uint16_t row_buf[PANEL_W];

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

	const struct device *exp  = DEVICE_DT_GET_OR_NULL(EXP_NODE);
	const struct device *dsi  = DEVICE_DT_GET_OR_NULL(DSI_NODE);
	const struct device *disp = DEVICE_DT_GET(DISPLAY_NODE);

	/*
	 * Step 1: the TCAL9538 panel-control expander.  It must be ready before we
	 * can drive LCD_PWR_EN.  (The hx8394 panel driver, which runs at
	 * POST_KERNEL, also depends on this expander for its reset-gpios.)
	 */
	bool exp_ok = dev_ready("expander", exp);

	/*
	 * Step 2: assert LCD_PWR_EN (expander P0) HIGH -- panel power on.  This must
	 * precede reset deassertion; the on-module backlight boost rises with it.
	 * The overlay's regulator-fixed already did this at boot, but we re-assert
	 * explicitly as the canonical teaching sequence (and a guard).
	 */
	bool pwr_ok = false;
	if (exp_ok && gpio_is_ready_dt(&lcd_pwr_en)) {
		int rc = gpio_pin_configure_dt(&lcd_pwr_en, GPIO_OUTPUT_ACTIVE);
		if (rc == 0) {
			/* Give the rail + backlight boost a moment to settle. */
			k_msleep(20);
			pwr_ok = true;
			printk("LCD_PWR_EN (exp P0): asserted HIGH\n");
		} else {
			printk("LCD_PWR_EN (exp P0): configure failed (%d)\n", rc);
		}
	} else if (exp_ok) {
		printk("LCD_PWR_EN (exp P0): expander GPIO not ready\n");
	}

	/*
	 * Step 3: the MIPI-DSI host.  (The hx8394 panel attached to it during its
	 * POST_KERNEL init; if that failed, the host may still be ready but the
	 * panel will not have come up -- the display write below is the real test.)
	 */
	bool dsi_ok = dev_ready("mipi-dsi", dsi);

	/* Step 4: the cdc200 display device (the render target). */
	bool disp_ok = dev_ready("display", disp);

	/*
	 * Step 5: render.  Fill the screen with a solid color, one row at a time.
	 * The descriptor describes a single PANEL_W x 1 strip; we walk it down the
	 * screen.  display_write copies into the SRAM0 framebuffer and flushes the
	 * data cache for the CDC scanout (handled inside the driver).
	 */
	bool write_ok = false;
	if (disp_ok) {
		for (uint16_t i = 0; i < PANEL_W; i++) {
			row_buf[i] = FILL_COLOR_RGB565;
		}

		struct display_buffer_descriptor desc = {
			.buf_size = sizeof(row_buf),
			.width    = PANEL_W,
			.height   = 1,
			.pitch    = PANEL_W,
		};

		int rc = 0;
		for (uint16_t y = 0; y < PANEL_H; y++) {
			rc = display_write(disp, 0, y, &desc, row_buf);
			if (rc != 0) {
				printk("display_write row %u failed (%d)\n", y, rc);
				break;
			}
		}
		if (rc == 0) {
			/* Take the panel out of blanking so the FB reaches glass. */
			(void)display_blanking_off(disp);
			write_ok = true;
			printk("display_write: full 720x1280 frame OK (0x%04x)\n", FILL_COLOR_RGB565);
		}
	}

	/*
	 * Optional: confirm the driver's reported capabilities match the panel.
	 * Informational only -- not part of the PASS gate.
	 */
	if (disp_ok) {
		struct display_capabilities caps;

		display_get_capabilities(disp, &caps);
		printk("caps: %ux%u fmts=0x%x\n",
		       caps.x_resolution,
		       caps.y_resolution,
		       caps.supported_pixel_formats);
	}

	bool pass = exp_ok && pwr_ok && dsi_ok && disp_ok && write_ok;

	if (pass) {
		printk("RESULT PASS: RK055HDMIPI4MA0 chain UP -- expander+power ready, "
		       "mipi-dsi + cdc200 display ready, full-screen RGB565 frame written "
		       "to the SRAM0 framebuffer.  Pixels-on-glass is bench-confirmed "
		       "separately (pixel-clock + LCD_RST polarity are bench-tunable)\n");
	} else {
		printk("RESULT FAIL: DSI display chain not fully up "
		       "(expander=%d power=%d mipi-dsi=%d display=%d write=%d) -- a device "
		       "is not ready or display_write failed; see the per-stage lines above\n",
		       (int)exp_ok,
		       (int)pwr_ok,
		       (int)dsi_ok,
		       (int)disp_ok,
		       (int)write_ok);
	}

	return 0;
}
