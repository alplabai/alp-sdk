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
 * PANEL ENABLE / RESET:
 *   A small I2C GPIO expander on I2C2 provides the HX8394 control GPIOs.  The
 *   expander reset line is released before POST_KERNEL device init; a boot-on
 *   fixed regulator then asserts the panel-enable GPIO before the HX8394 driver
 *   performs its reset and DSI initialization.
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
 *   - cdc200/mipi_dsi clock ids are the real re-authored ALIF_*_CLK values and
 *     are proved by this app's bench PASS gate.
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
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/mipi_dsi.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/pinctrl/alif-ensemble-pinctrl.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>

/*
 * Pulse the panel-control expander reset before the PCA driver probes it.
 * gpio_dw does not apply pad mux, so mux P10_2 to GPIO first.  The PCA expander
 * is intentionally moved to POST_KERNEL priority 60 in prj.conf; this hook runs
 * at priority 55 so the expander sees a clean low->high reset edge first.
 */
#define LCD_EXP_PAD_REN (1U << 16) /* Alif pad receiver-enable (REN_BIT_POS=16) */

static int lcd_exp_reset_release(void)
{
	const struct device *gpio10 = DEVICE_DT_GET(DT_NODELABEL(gpio10));
	pinctrl_soc_pin_t    m[]    = { PIN_P10_2__GPIO | LCD_EXP_PAD_REN };
	int                  rc     = pinctrl_configure_pins(m, ARRAY_SIZE(m), 0U);

	if (rc != 0 || !device_is_ready(gpio10)) {
		return rc ? rc : -ENODEV;
	}
	rc = gpio_pin_configure(gpio10, 2, GPIO_OUTPUT_LOW);
	if (rc != 0) {
		return rc;
	}
	k_busy_wait(10 * USEC_PER_MSEC);

	rc = gpio_pin_set(gpio10, 2, 1);
	if (rc != 0) {
		return rc;
	}
	k_busy_wait(10 * USEC_PER_MSEC);

	return 0;
}

SYS_INIT(lcd_exp_reset_release, POST_KERNEL, 55);

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

/* Minimal DesignWare DSI host status dump for bench diagnostics. */
#define DSI_BASE_ADDR           0x49032000UL
#define DSI_PCKHDL_CFG_ADDR     (DSI_BASE_ADDR + 0x2CUL)
#define DSI_MODE_CFG_ADDR       (DSI_BASE_ADDR + 0x34UL)
#define DSI_DPI_LP_CMD_TIM_ADDR (DSI_BASE_ADDR + 0x18UL)
#define DSI_CMD_MODE_CFG_ADDR   (DSI_BASE_ADDR + 0x68UL)
#define DSI_CMD_PKT_STATUS_ADDR (DSI_BASE_ADDR + 0x74UL)
#define DSI_LPCLK_CTRL_ADDR     (DSI_BASE_ADDR + 0x94UL)
#define DSI_PHY_STATUS_ADDR     (DSI_BASE_ADDR + 0xB0UL)
#define DSI_INT_ST0_ADDR        (DSI_BASE_ADDR + 0xBCUL)
#define DSI_INT_ST1_ADDR        (DSI_BASE_ADDR + 0xC0UL)
#define DSI_PHY_TMR_RD_CFG_ADDR (DSI_BASE_ADDR + 0xF4UL)
#define DSI_PHY_LOCK            BIT(0)

#define DPHY_BASE_ADDR      0x4903F000UL
#define DPHY_PLL_STAT0_ADDR (DPHY_BASE_ADDR + 0x20UL)
#define DPHY_PLL_STAT1_ADDR (DPHY_BASE_ADDR + 0x24UL)
#define DPHY_TX_CTRL0_ADDR  (DPHY_BASE_ADDR + 0x30UL)
#define DPHY_TX_CTRL1_ADDR  (DPHY_BASE_ADDR + 0x34UL)

static void dump_dsi_status(const char *stage)
{
	uint32_t phy = sys_read32(DSI_PHY_STATUS_ADDR);

	printk("dsi status[%s]: cmd=0x%08x int0=0x%08x int1=0x%08x phy=0x%08x "
	       "lock=%d mode=0x%08x cmdcfg=0x%08x pckhdl=0x%08x lpclk=0x%08x "
	       "lpcmd=0x%08x rdtime=0x%08x dphy-pll=%08x/%08x tx=%08x/%08x\n",
	       stage,
	       sys_read32(DSI_CMD_PKT_STATUS_ADDR),
	       sys_read32(DSI_INT_ST0_ADDR),
	       sys_read32(DSI_INT_ST1_ADDR),
	       phy,
	       (int)((phy & DSI_PHY_LOCK) != 0),
	       sys_read32(DSI_MODE_CFG_ADDR),
	       sys_read32(DSI_CMD_MODE_CFG_ADDR),
	       sys_read32(DSI_PCKHDL_CFG_ADDR),
	       sys_read32(DSI_LPCLK_CTRL_ADDR),
	       sys_read32(DSI_DPI_LP_CMD_TIM_ADDR),
	       sys_read32(DSI_PHY_TMR_RD_CFG_ADDR),
	       sys_read32(DPHY_PLL_STAT0_ADDR),
	       sys_read32(DPHY_PLL_STAT1_ADDR),
	       sys_read32(DPHY_TX_CTRL0_ADDR),
	       sys_read32(DPHY_TX_CTRL1_ADDR));
}

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

#define VBAT_PWR_CTRL_ADDR    0x1A609008UL
#define VBAT_DPHY_PWR_ISO_MSK (BIT(0) | BIT(1) | BIT(4) | BIT(5) | BIT(8) | BIT(9) | BIT(12))
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
#define EXP_NODE     DT_NODELABEL(lcd_exp)
#define LCD_PWR_NODE DT_NODELABEL(lcd_pwr_en)

static const struct gpio_dt_spec lcd_pwr_gpio = GPIO_DT_SPEC_GET(LCD_PWR_NODE, enable_gpios);
static const struct i2c_dt_spec  lcd_exp_i2c  = I2C_DT_SPEC_GET(EXP_NODE);

static void scan_i2c2(const char *stage)
{
	const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(i2c2));

	if (!device_is_ready(bus)) {
		printk("i2c2 scan[%s]: bus not ready\n", stage);
		return;
	}

	printk("i2c2 scan[%s]:", stage);
	for (uint16_t addr = 0x08; addr < 0x78; addr++) {
		uint8_t v;

		if (i2c_read(bus, &v, 1, addr) == 0) {
			printk(" 0x%02x", addr);
		}
	}
	printk("\n");
}

static void dump_lcd_exp_regs(const char *stage)
{
	uint8_t in  = 0;
	uint8_t out = 0;
	uint8_t pol = 0;
	uint8_t cfg = 0;
	int     r0  = i2c_reg_read_byte_dt(&lcd_exp_i2c, 0x00, &in);
	int     r1  = i2c_reg_read_byte_dt(&lcd_exp_i2c, 0x01, &out);
	int     r2  = i2c_reg_read_byte_dt(&lcd_exp_i2c, 0x02, &pol);
	int     r3  = i2c_reg_read_byte_dt(&lcd_exp_i2c, 0x03, &cfg);

	printk("lcd-exp regs[%s]: in=%02x(rc%d) out=%02x(rc%d) pol=%02x(rc%d) cfg=%02x(rc%d)\n",
	       stage,
	       in,
	       r0,
	       out,
	       r1,
	       pol,
	       r2,
	       cfg,
	       r3);
}

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

static ssize_t dcs_read_lpm(const struct device *dsi, uint8_t cmd, void *buf, size_t len)
{
	struct mipi_dsi_msg msg = {
		.type   = MIPI_DSI_DCS_READ,
		.flags  = MIPI_DSI_MSG_USE_LPM,
		.cmd    = cmd,
		.rx_len = len,
		.rx_buf = buf,
	};

	return mipi_dsi_transfer(dsi, 0, &msg);
}

static bool probe_panel_reads(const struct device *dsi)
{
	static const struct {
		uint8_t     cmd;
		uint8_t     len;
		const char *name;
	} probes[] = {
		{ 0x04, 3, "RDDID" },     { 0x09, 4, "RDDST" },     { 0x0A, 1, "RDDPM" },
		{ 0x0B, 1, "RDDMADCTL" }, { 0x0C, 1, "RDDCOLMOD" }, { 0x0D, 1, "RDDIM" },
		{ 0x0E, 1, "RDDSM" },     { 0xA1, 5, "RDDDB" },     { 0xDA, 1, "RDID1" },
		{ 0xDB, 1, "RDID2" },     { 0xDC, 1, "RDID3" },
	};
	uint8_t buf[5];
	bool    any_nonzero = false;

	for (size_t i = 0; i < ARRAY_SIZE(probes); i++) {
		for (size_t j = 0; j < sizeof(buf); j++) {
			buf[j] = 0;
		}

		ssize_t rc      = dcs_read_lpm(dsi, probes[i].cmd, buf, probes[i].len);
		bool    nonzero = false;
		for (size_t j = 0; j < probes[i].len; j++) {
			nonzero = nonzero || (buf[j] != 0);
		}
		any_nonzero = any_nonzero || (rc > 0 && nonzero);

		printk("panel DCS read: %-9s cmd=0x%02x rc=%d data=%02x %02x %02x %02x %02x\n",
		       probes[i].name,
		       probes[i].cmd,
		       (int)rc,
		       buf[0],
		       buf[1],
		       buf[2],
		       buf[3],
		       buf[4]);
	}

	printk("panel presence: %s\n",
	       any_nonzero ? "RESPONDING (DCS readback non-zero)"
	                   : "no read response (writes are unacked)");
	return any_nonzero;
}

int main(void)
{
	printk("\n=== aen-dsi-display ===\n");

	const struct device *exp   = DEVICE_DT_GET(EXP_NODE);
	const struct device *pwr   = DEVICE_DT_GET(LCD_PWR_NODE);
	const struct device *panel = DEVICE_DT_GET(PANEL_NODE);
	const struct device *dsi   = DEVICE_DT_GET(DSI_NODE);
	const struct device *disp  = DEVICE_DT_GET(DISPLAY_NODE);

	/* Step 1: the panel-control expander must be ready for reset-gpios. */
	bool exp_ok = dev_ready("lcd-exp", exp);
	bool pwr_ok = dev_ready("lcd-reg", pwr);
	scan_i2c2("entry");
	if (exp_ok) {
		dump_lcd_exp_regs("before-pwr-set");
	}
	if (exp_ok) {
		int rc = gpio_pin_set_dt(&lcd_pwr_gpio, 1);

		if (rc == 0) {
			printk("%-8s: asserted\n", "lcd-pwr");
			k_sleep(K_MSEC(20));
		} else {
			int rc2 = gpio_port_set_bits_raw(exp, BIT(lcd_pwr_gpio.pin));
			int rc3 = i2c_recover_bus(lcd_exp_i2c.bus);

			printk("%-8s: assert failed (%d), raw-set=%d recover=%d\n", "lcd-pwr", rc, rc2, rc3);
			scan_i2c2("after-recover");
			exp_ok = false;
		}
		dump_lcd_exp_regs("after-pwr-set");
	}

	/* Step 2: HX8394 init does mipi_dsi_attach + DCS power-on over DSI. */
	bool panel_ok = dev_ready("panel", panel);

	/* Step 3: the MIPI-DSI host. */
	bool dsi_ok = dev_ready("mipi-dsi", dsi);
	if (dsi_ok) {
		dump_dsi_status("before-read");
	}

	/* Step 4: the cdc200 display device (the render target). */
	bool disp_ok = dev_ready("display", disp);

	/*
	 * Step 5: TRUE panel-presence check.  DSI command WRITES are unacknowledged
	 * -- they "succeed" even with no panel attached -- so a clean init alone does
	 * NOT prove a panel is on the FFC.  A DCS READ does: it requires the panel to
	 * drive data back over the link.  Read RDDID (0x04, 3-byte manufacturer/ID)
	 * and RDDPM (0x0A, power/display-on status).  Non-zero/non-error = a real
	 * panel is attached and responding.
	 */
	bool panel_read_ok = false;
	if (dsi_ok) {
		panel_read_ok = probe_panel_reads(dsi);
		dump_dsi_status("after-read");
	}

	/*
	 * Step 6: render.  Fill the screen with a solid color, one row at a time.
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

	bool pass = exp_ok && pwr_ok && panel_ok && dsi_ok && disp_ok && panel_read_ok && write_ok;

	if (pass) {
		printk("RESULT PASS: RK055HDMIPI4MA0 chain UP -- hx8394 panel + mipi-dsi "
		       "+ cdc200 display ready, full-screen RGB565 frame written to the "
		       "SRAM0 framebuffer; pixels-on-glass: confirm green on the panel\n");
	} else {
		printk("RESULT FAIL: DSI display chain not fully up "
		       "(lcd-exp=%d lcd-reg=%d panel=%d mipi-dsi=%d display=%d dcs-read=%d write=%d) -- "
		       "a device is not ready "
		       "or display_write failed; see the per-stage lines above\n",
		       (int)exp_ok,
		       (int)pwr_ok,
		       (int)panel_ok,
		       (int)dsi_ok,
		       (int)disp_ok,
		       (int)panel_read_ok,
		       (int)write_ok);
	}

	return 0;
}
