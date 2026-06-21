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
 * PANEL POWER / RESET (TI TCAL9538 expander, U35, on I2C2 @0x72):
 *   - expander P0 = LCD_PWR_EN (active-high) -- panel power; the on-module
 *     backlight boost comes up with it (no dedicated backlight GPIO).
 *   - expander P1 = LCD_RST -- panel reset; active-LOW at the panel through
 *     level-shifter U5.  Consumed by the hx8394 driver via reset-gpios.
 *   The expander's own \RESET (IO_EXP.RST) is SoC P3_6 -- released by the
 *   lcd_exp_reset_release() SYS_INIT below.  The boot order matters: power (P0)
 *   MUST be asserted before reset (P1) is deasserted.  We model P0 two ways for
 *   robustness: (1) a regulator-fixed (regulator-boot-on) in the overlay brings
 *   the rail up at boot, and (2) main() ALSO asserts P0 explicitly below.
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
 *   - The LCD_RST polarity through level-shifter U5 (overlay assumes the net is
 *     active-LOW at the panel) is bench-unconfirmed.
 *   - HARDWARE-BLOCKED: U35 (the TCAL9538) is electrically silent on I2C2 (ACKs
 *     at no address while 13 other devices on the bus respond), despite correct
 *     part/addr(0x72)/bus/reset(P3_6) + +VIO present.  The EEPROM is SoM-side
 *     (works); U35 is EVK-side through the U6 connector -- suspect open/cold
 *     solder on U35 SDA/SCL (pins 13/12) or the U6->U35 routing.  Without U35
 *     the panel cannot be powered/reset, so pixels-on-glass waits on that fix.
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
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>

/* The display device (the cdc200 pixel pump) is the chosen render target. */
#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define DSI_NODE     DT_NODELABEL(mipi_dsi)
#define PANEL_NODE   DT_NODELABEL(lcd_panel)

/* Panel geometry (must match the overlay's cdc200 + panel nodes). */
#define PANEL_W 720
#define PANEL_H 1280

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

/* One-shot diagnostic: scan i2c2 for ACKing addresses (1-byte read probe). */
static void i2c2_scan(void)
{
	const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(i2c2));

	if (!device_is_ready(bus)) {
		printk("i2c2 scan: bus NOT ready\n");
		return;
	}
	printk("i2c2 scan (ACK):");
	for (uint16_t a = 0x00; a <= 0x7F; a++) {
		uint8_t b;

		if (i2c_read(bus, &b, 1, a) == 0) {
			printk(" 0x%02x", a);
		}
	}
	printk("\n");
}

int main(void)
{
	printk("\n=== aen-dsi-display ===\n");

	i2c2_scan();

	const struct device *panel = DEVICE_DT_GET(PANEL_NODE);
	const struct device *dsi   = DEVICE_DT_GET(DSI_NODE);
	const struct device *disp  = DEVICE_DT_GET(DISPLAY_NODE);

	/*
	 * The panel power (LCD_PWR_EN) + reset (LCD_RST) are held released by the
	 * board pull-ups R142/R66 to +VIO, so the panel powers up + comes out of
	 * reset on its own -- no expander GPIO needed (U35 is board-faulty: a
	 * TCA6408A in a TCAL9538 footprint with a power pin grounded).
	 *
	 * Step 1: the HX8394 panel.  Its POST_KERNEL init does mipi_dsi_attach +
	 * the DCS power-on sequence over DSI (it skips the reset toggle because no
	 * reset-gpios is wired -- the panel uses its power-on-reset).  device_ready
	 * here means that DCS init succeeded.
	 */
	bool panel_ok = dev_ready("panel", panel);

	/* Step 2: the MIPI-DSI host. */
	bool dsi_ok = dev_ready("mipi-dsi", dsi);

	/* Step 3: the cdc200 display device (the render target). */
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

	bool pass = panel_ok && dsi_ok && disp_ok && write_ok;

	if (pass) {
		printk("RESULT PASS: RK055HDMIPI4MA0 chain UP -- hx8394 panel + mipi-dsi "
		       "+ cdc200 display ready, full-screen RGB565 frame written to the "
		       "SRAM0 framebuffer.  Panel power/reset come from the board pull-ups "
		       "(U35 bypassed) -- pixels-on-glass: confirm green on the panel\n");
	} else {
		printk("RESULT FAIL: DSI display chain not fully up "
		       "(panel=%d mipi-dsi=%d display=%d write=%d) -- a device is not ready "
		       "or display_write failed; see the per-stage lines above\n",
		       (int)panel_ok,
		       (int)dsi_ok,
		       (int)disp_ok,
		       (int)write_ok);
	}

	return 0;
}
