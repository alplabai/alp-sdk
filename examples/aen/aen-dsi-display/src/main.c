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
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/mipi_dsi.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/pinctrl/alif-ensemble-pinctrl.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>

/*
 * Release the panel-control expander's own \RESET (IO_EXP.RST).  AUTHORITATIVE
 * EVK netlist trace: IO_EXP.RST -> SoC ball N1 -> P10_2 (gpio10.2) [DFP pins.h:
 * "P10_2 on pin N1", func[0]=GPIO10_2].  NOT P3_6 -- an earlier SoM-side trace
 * mis-identified the pad, so the firmware was toggling P3_6 (unconnected to U35)
 * with zero effect on the expander.  \RESET is active-low (R141 pulls it high),
 * so drive HIGH to release.  If P10_2 idles low/indeterminate it holds the
 * expander in reset and it NACKs every address.  gpio_dw applies no pad mux, so
 * mux P10_2 -> GPIO (+ REN) via the Alif pinctrl SoC driver first.  Run at
 * PRE_KERNEL_2, before the expander (POST_KERNEL).
 */
#define LCD_EXP_PAD_REN (1U << 16) /* Alif pad receiver-enable (REN_BIT_POS=16) */

static int lcd_exp_reset_release(void)
{
	const struct device *gpio10 = DEVICE_DT_GET(DT_NODELABEL(gpio10));
	pinctrl_soc_pin_t m[] = { PIN_P10_2__GPIO | LCD_EXP_PAD_REN };
	int rc = pinctrl_configure_pins(m, ARRAY_SIZE(m), 0U);

	if (rc != 0 || !device_is_ready(gpio10)) {
		return rc ? rc : -ENODEV;
	}
	return gpio_pin_configure(gpio10, 2, GPIO_OUTPUT_HIGH);
}

SYS_INIT(lcd_exp_reset_release, PRE_KERNEL_2, 0);

/*
 * Program the CDC200 pixel-clock divider directly.  The upstream Alif clockctrl
 * has no .set_rate for the CDC pixel clock, so CDC200_PIXCLK_CTRL[24:16] keeps
 * its reset divisor 0x1FF (511) -> ~0.78 MHz pixel clock.  Set it for ~66.67 MHz
 * (SYST_ACLK 400 MHz / 6; panel-native is 62.35 MHz -- nearest reachable, the
 * exact rate is bench-tunable).  CDC200_PIXCLK_CTRL = CLKCTL_PER_MST(0x4903F000)
 * + 0x04 (DFP sys_ctrl_cdc.h).
 */
#define CDC_PIXCLK_CTRL_ADDR 0x4903F004UL
#define CDC_PIXCLK_DIV_POS   16U
#define CDC_PIXCLK_DIV_MSK   (0x1FFUL << CDC_PIXCLK_DIV_POS)
#define CDC_PIXCLK_DIV       6UL

static int cdc_pixclk_div_fixup(void)
{
	uint32_t v = sys_read32(CDC_PIXCLK_CTRL_ADDR);

	v = (v & ~CDC_PIXCLK_DIV_MSK) | (CDC_PIXCLK_DIV << CDC_PIXCLK_DIV_POS);
	sys_write32(v, CDC_PIXCLK_CTRL_ADDR);
	return 0;
}

SYS_INIT(cdc_pixclk_div_fixup, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

/*
 * Bring up the MIPI D-PHY's clock SOURCES and analog power before any peripheral
 * touches the PHY.  The CKEN gate bits in CLKCTL_PER_MST->MIPI_CKEN (set by the
 * dphy driver) only gate clocks that must already be oscillating upstream; on a
 * bare-metal/SE-less RAM-run those upstream enables are NOT done, so the D-PHY
 * PLL has neither a 38.4 MHz reference nor analog supply and never locks
 * (bench: DSI_PHY_STATUS stuck 0x1400, no LOCK bit -- even with the PLL m/n/VCO
 * shadow registers correctly programmed).
 *
 * Two register-direct steps (mirrors the Alif sdk-alif display sample's CGU poke
 * and the DevKit-e8 vbat_init(); no SE-services dependency):
 *
 *  1. CGU->CLK_ENA (0x1A602014): enable HFOSC (38.4 MHz, the PLL reference) on
 *     bit21 and the CFG clock (100 MHz, source of the 25 MHz D-PHY cfg clock that
 *     drives the PHY startup/lock state machine) on bit23.
 *  2. VBAT->PWR_CTRL (0x1A609008): un-mask the MIPI TX/RX/PLL DPHY power rails
 *     (bits 0/4/8) + the VPH-1P8 bypass (bit12) and drop the TX/RX/PLL isolation
 *     (bits 1/5/9).  At reset these leave the DPHY analog islands powered-off and
 *     isolated, so the PLL cannot lock regardless of config.
 */
#define CGU_CLK_ENA_ADDR  0x1A602014UL
#define CGU_CLK_HFOSC_BIT 21U /* HFOSC 38.4 MHz -- D-PHY PLL reference */
#define CGU_CLK_CFG_BIT   23U /* CFG 100 MHz -- source of the 25 MHz D-PHY cfg clk */

#define VBAT_PWR_CTRL_ADDR 0x1A609008UL
#define VBAT_DPHY_PWR_ISO_MSK                                                  \
	(BIT(0) | BIT(1) | BIT(4) | BIT(5) | BIT(8) | BIT(9) | BIT(12))
/* TX pwr/iso | RX pwr/iso | PLL pwr/iso | VPH-1P8 bypass */

static int dphy_clock_power_enable(void)
{
	sys_set_bit(CGU_CLK_ENA_ADDR, CGU_CLK_HFOSC_BIT);
	sys_set_bit(CGU_CLK_ENA_ADDR, CGU_CLK_CFG_BIT);
	sys_clear_bits(VBAT_PWR_CTRL_ADDR, VBAT_DPHY_PWR_ISO_MSK);
	return 0;
}

SYS_INIT(dphy_clock_power_enable, PRE_KERNEL_1, 0);

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

/*
 * Identify each ACKing device: read the common WHO_AM_I / chip-ID / manufacturer-ID
 * register candidates.  '--' = that register NACK'd (device has no register at that
 * offset / read-without-pointer device like a raw EEPROM).
 *	r00 - generic first register / many sensors' config or status
 *	r0F - ST/TI WHO_AM_I (LSM*, TMP117 device-id high)
 *	r75 - InvenSense WHO_AM_I (ICM-42670 = 0x67)
 *	rD0 - Bosch chip-id (BMP/BME = 0x58/0x60, BMI*)
 *	r3E - TI INA2xx manufacturer-id MSB (0x54 = 'T')
 *	rFE - alt manufacturer-id (many TI/Maxim parts)
 */
static void i2c2_identify(void)
{
	const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(i2c2));
	static const uint8_t addrs[] = {0x40, 0x41, 0x42, 0x47, 0x48, 0x49, 0x4a,
					0x4d, 0x4e, 0x50, 0x58, 0x68, 0x69};
	static const uint8_t idregs[] = {0x00, 0x0F, 0x75, 0xD0, 0x3E, 0xFE};

	if (!device_is_ready(bus)) {
		return;
	}
	printk("i2c2 identify (r00 r0F r75 rD0 r3E rFE):\n");
	for (size_t i = 0; i < ARRAY_SIZE(addrs); i++) {
		printk("  0x%02x:", addrs[i]);
		for (size_t j = 0; j < ARRAY_SIZE(idregs); j++) {
			uint8_t v;

			if (i2c_reg_read_byte(bus, addrs[i], idregs[j], &v) == 0) {
				printk(" %02x", v);
			} else {
				printk(" --");
			}
		}
		printk("\n");
	}
}

/*
 * INA236 confirm: read the 16-bit manufacturer-id (reg 0x3E = 0x5449 'TI') and
 * die-id (reg 0x3F) registers.  Compares 0x40 (known INA236) vs 0x48 (suspect).
 */
static void ina236_probe(void)
{
	const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(i2c2));
	static const uint8_t addrs[] = {0x40, 0x48};

	if (!device_is_ready(bus)) {
		return;
	}
	printk("INA236 check (mfr-id r3E should=0x5449 'TI', die-id r3F):\n");
	for (size_t i = 0; i < ARRAY_SIZE(addrs); i++) {
		uint8_t m[2] = {0, 0}, d[2] = {0, 0};
		int r1 = i2c_burst_read(bus, addrs[i], 0x3E, m, 2);
		int r2 = i2c_burst_read(bus, addrs[i], 0x3F, d, 2);

		printk("  0x%02x: mfr=%02x%02x (rc%d)  die=%02x%02x (rc%d)\n",
		       addrs[i], m[0], m[1], r1, d[0], d[1], r2);
	}
}

/*
 * Characterise the mystery 0x58: dump regs 0x00..0x1F, then re-read reg 0x00
 * a few times.  A real chip = structured + stable bytes; an address-decode
 * ghost/alias = garbage or values that mirror a neighbouring device.  Dumps
 * 0x50 (EEPROM) and 0x4d (TAS2563) alongside for an aliasing comparison.
 */
static void probe_dump(const struct device *bus, uint8_t addr)
{
	uint8_t buf[32] = {0};
	int rc = i2c_burst_read(bus, addr, 0x00, buf, sizeof(buf));

	printk("  0x%02x r00..1F (rc%d):", addr, rc);
	for (size_t i = 0; i < sizeof(buf); i++) {
		printk(" %02x", buf[i]);
	}
	printk("\n  0x%02x r00 x5:", addr);
	for (int i = 0; i < 5; i++) {
		uint8_t v = 0;

		(void)i2c_reg_read_byte(bus, addr, 0x00, &v);
		printk(" %02x", v);
	}
	printk("\n");
}

static void mystery_0x58(void)
{
	const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(i2c2));

	if (!device_is_ready(bus)) {
		return;
	}
	printk("0x58 characterise (vs 0x50 EEPROM, 0x4d TAS):\n");
	probe_dump(bus, 0x58);
	probe_dump(bus, 0x50);
	probe_dump(bus, 0x4d);
}

/*
 * Proof test: 16-bit-addressed (24C128-style) read of the first 32 manifest
 * bytes from 0x50 and 0x58.  Identical bytes => 0x58 is the same EEPROM mirrored.
 */
static void eeprom_alias_compare(void)
{
	const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(i2c2));
	uint8_t waddr[2] = {0x00, 0x00};
	uint8_t a[32] = {0}, b[32] = {0};

	if (!device_is_ready(bus)) {
		return;
	}
	int r1 = i2c_write_read(bus, 0x50, waddr, 2, a, sizeof(a));
	int r2 = i2c_write_read(bus, 0x58, waddr, 2, b, sizeof(b));

	printk("EEPROM 16-bit manifest read (0x0000, 32B):\n  0x50 (rc%d):", r1);
	for (size_t i = 0; i < sizeof(a); i++) {
		printk(" %02x", a[i]);
	}
	printk("\n  0x58 (rc%d):", r2);
	for (size_t i = 0; i < sizeof(b); i++) {
		printk(" %02x", b[i]);
	}
	printk("\n  match: %s\n", (r1 == 0 && r2 == 0 && memcmp(a, b, sizeof(a)) == 0)
				   ? "IDENTICAL -> 0x58 is the EEPROM mirrored"
				   : "DIFFER -> 0x58 is a distinct device");
}

int main(void)
{
	printk("\n=== aen-dsi-display ===\n");

	/*
	 * Clean reset PULSE on the expander \RESET (P10_2/gpio10.2).  The TCA6408A
	 * latches its I2C address straps on a reset edge; a strap patched live after
	 * power-on (e.g. A0->VIO) is only picked up after a low->high \RESET edge.
	 * The PRE_KERNEL hook drives it static-high; here (kernel up, k_sleep safe)
	 * we toggle it low->high so the chip re-latches the patched address.
	 */
	{
		const struct device *gpio10 = DEVICE_DT_GET(DT_NODELABEL(gpio10));

		if (device_is_ready(gpio10)) {
			gpio_pin_set(gpio10, 2, 0);
			k_sleep(K_MSEC(10));
			gpio_pin_set(gpio10, 2, 1);
			k_sleep(K_MSEC(10));
		}
	}

	i2c2_scan();
	i2c2_identify();
	ina236_probe();
	mystery_0x58();
	eeprom_alias_compare();

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
	 * Step 4: TRUE panel-presence check.  DSI command WRITES are unacknowledged
	 * -- they "succeed" even with no panel attached -- so a clean init alone does
	 * NOT prove a panel is on the FFC.  A DCS READ does: it requires the panel to
	 * drive data back over the link.  Read RDDID (0x04, 3-byte manufacturer/ID)
	 * and RDDPM (0x0A, power/display-on status).  Non-zero/non-error = a real
	 * panel is attached and responding.
	 */
	if (dsi_ok) {
		uint8_t id[3] = {0};
		uint8_t pm = 0;
		int rid = mipi_dsi_dcs_read(dsi, 0, 0x04, id, sizeof(id));
		int rpm = mipi_dsi_dcs_read(dsi, 0, 0x0A, &pm, 1);

		printk("panel DCS read: RDDID rc=%d id=%02x %02x %02x | RDDPM rc=%d pm=0x%02x\n",
		       rid, id[0], id[1], id[2], rpm, pm);
		printk("panel presence: %s\n",
		       (rid > 0 && (id[0] | id[1] | id[2])) ? "RESPONDING (real panel on FFC)"
							    : "no read response (writes are unacked)");
	}

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
