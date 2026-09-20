/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-dsi-display -- drive the RK055HDMIPI4MA0 (Rocktech 720x1280, Himax HX8394
 * controller, MIPI-DSI) panel through the full Alif Ensemble E8 C2-MIPI-DSI
 * DISPLAY chain on an E1M-AEN SoM (M55-HE), via the bench RAM-run + RAM-console
 * flow.  This is the pixels-on-glass successor to aen-dsi-regcheck (which only
 * proved the chain BINDS): it turns ON the cdc200 + dsi_dw display-class drivers
 * and renders a solid-color framebuffer.
 *
 * THE DISPLAY CHAIN (app -> glass):
 *
 *   display_write()  ->  cdc200@49031000  (tes,cdc-2.1)
 *                            -- the DPI/RGB pixel pump (CDC200).  Its L1
 *                               framebuffer lives in SRAM0 @0x02200000 (the
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
 * (the shield's 2 MiB at the top of SRAM0, 0x02200000) holds the L1
 * framebuffer.  The driver uses that address directly -- no linker section --
 * so the 1.84 MB framebuffer is not part of the ITCM RAM-run image.  The
 * driver flushes the data cache after every framebuffer write
 * (sys_cache_data_flush_range), so the CDC scanout sees coherent pixels.
 *
 * BENCH-TUNABLES / TBD (none block the build; all gate pixels-on-glass):
 *   - DPI pixel clock: the shield's cdc200 clock-frequency is the one knob
 *     (400 MHz / 7 = 57.14 MHz; the panel-native 62.346 MHz is unreachable).
 *     The SoC glue's CDC divider and the DSI host's lane timing both derive
 *     from it.
 *   - cdc200/mipi_dsi clock ids are the real re-authored ALIF_*_CLK values and
 *     are proved by this app's bench PASS gate.
 *
 * BENCH DIAGNOSTICS (not API usage, and not part of the PASS gate): every block
 * marked "BENCH DIAGNOSTIC" below exists to classify the two open defects on
 * e1m-aen-evk-01 (#2199) -- whole-panel DCS silence on some cold cycles, and
 * rx_len > 1 reads never answering.  They read registers the drivers own, mask
 * the DSI IRQ around a transfer so the read-to-clear error latches survive, and
 * snapshot the expander before the panel regulator runs.  They print raw values
 * and decode nothing.  A production app needs none of this.
 *
 * The PASS gate: the expander, the DSI host, AND the cdc200 display device are
 * all device_is_ready, a DCS read gets a non-zero panel answer, display_write()
 * of a solid-color frame returns 0, and display_blanking_off() starts scanout.
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
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>

/*
 * CDC200_PIXCLK_CTRL = CLKCTL_PER_MST(0x4903F000) + 0x04 (DFP sys_ctrl_cdc.h).
 * The SoC glue writes its divider (bits [24:16]); read here for the scanout
 * report only.
 */
#define CDC_PIXCLK_CTRL_ADDR 0x4903F004UL

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
#define DSI_VERSION_ADDR        (DSI_BASE_ADDR + 0x00UL)
#define DSI_VID_PKT_SIZE_ADDR   (DSI_BASE_ADDR + 0x3CUL)
#define DSI_VID_HLINE_TIME_ADDR (DSI_BASE_ADDR + 0x50UL)
#define DSI_VID_VACTIVE_ADDR    (DSI_BASE_ADDR + 0x60UL)

#define CDC_GLB_CTRL_ADDR 0x49031018UL /* cdc200 @0x49031000 + CDC_GLB_CTRL; bit0 CDC_EN */

#define DPHY_BASE_ADDR      0x4903F000UL
#define DPHY_PLL_STAT0_ADDR (DPHY_BASE_ADDR + 0x20UL)
#define DPHY_PLL_STAT1_ADDR (DPHY_BASE_ADDR + 0x24UL)
#define DPHY_TX_CTRL0_ADDR  (DPHY_BASE_ADDR + 0x30UL)
#define DPHY_TX_CTRL1_ADDR  (DPHY_BASE_ADDR + 0x34UL)

static void dump_dsi_status(const char *stage)
{
	uint32_t phy = sys_read32(DSI_PHY_STATUS_ADDR);

	printk("dsi status[%s]: ver=0x%08x pkt=%u hline=%u vact=%u cmd=0x%08x int0=0x%08x int1=0x%08x "
	       "phy=0x%08x "
	       "lock=%d mode=0x%08x cmdcfg=0x%08x pckhdl=0x%08x lpclk=0x%08x "
	       "lpcmd=0x%08x rdtime=0x%08x dphy-pll=%08x/%08x tx=%08x/%08x\n",
	       stage,
	       sys_read32(DSI_VERSION_ADDR),
	       sys_read32(DSI_VID_PKT_SIZE_ADDR),
	       sys_read32(DSI_VID_HLINE_TIME_ADDR),
	       sys_read32(DSI_VID_VACTIVE_ADDR),
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
 * Scanout health (bench diagnostic, not API usage).  With the DSI IRQ masked,
 * collect the read-to-clear INT_ST1 error latches over ~6 frames of video.
 * DPI_PLD_WR_ERR (bit 7) = the CDC feeds pixels faster than the DSI line time
 * was programmed for; DPI_BUFF_PLD_UNDER (bit 19) = slower; TO_HS_TX /
 * TO_LP_RX (bits 0/1) = link timeouts.  Healthy = CDC_EN set, the host in
 * video mode, and no INT_ST1 error at all.
 */
static bool check_scanout(void)
{
	unsigned int irq = DT_IRQN(DT_NODELABEL(mipi_dsi));

	irq_disable(irq);
	(void)sys_read32(DSI_INT_ST0_ADDR);
	(void)sys_read32(DSI_INT_ST1_ADDR);
	k_msleep(100);

	uint32_t int0 = sys_read32(DSI_INT_ST0_ADDR);
	uint32_t int1 = sys_read32(DSI_INT_ST1_ADDR);

	irq_enable(irq);
	printk(
	    "scanout 100 ms: int0=0x%08x int1=0x%08x dpi-wr-err=%d dpi-under=%d pixclk-ctrl=0x%08x\n",
	    int0,
	    int1,
	    (int)((int1 & BIT(7)) != 0),
	    (int)((int1 & BIT(19)) != 0),
	    sys_read32(CDC_PIXCLK_CTRL_ADDR));
	return (int1 == 0U) && (sys_read32(CDC_GLB_CTRL_ADDR) & BIT(0)) &&
	       (sys_read32(DSI_MODE_CFG_ADDR) == 0U);
}

/* The display device (the cdc200 pixel pump) is the chosen render target. */
#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define DSI_NODE     DT_NODELABEL(mipi_dsi)
#define PANEL_NODE   DT_NODELABEL(lcd_panel)
#define EXP_NODE     DT_NODELABEL(lcd_exp)
#define LCD_PWR_NODE DT_NODELABEL(lcd_pwr_en)

static const struct gpio_dt_spec lcd_pwr_gpio = GPIO_DT_SPEC_GET(LCD_PWR_NODE, enable_gpios);
static const struct gpio_dt_spec bl_gpio      = GPIO_DT_SPEC_GET(PANEL_NODE, bl_gpios);
static const struct i2c_dt_spec  lcd_exp_i2c  = I2C_DT_SPEC_GET(EXP_NODE);

/*
 * BENCH DIAGNOSTIC -- panel power at board POR (#2199).
 *
 * The question this answers: was the panel ALREADY powered when the board came
 * out of reset, or does this boot turn it on?  Only a read taken before
 * regulator_fixed asserts the enable pin can tell, so it runs from its own
 * SYS_INIT wedged into the one window where the expander exists but the
 * regulator has not run yet.  main() prints what it captured.
 *
 *   GPIO_PCA_SERIES_INIT_PRIORITY  the expander is up (POR regs restored)
 *   ---> LCD_EXP_POR_PRIO          this snapshot
 *   REGULATOR_FIXED_INIT_PRIORITY  panel supply on
 *
 * TCAL9538 registers: 0x00 input port, 0x01 output port, 0x03 configuration
 * (1 = input).  Printed raw -- this app decodes nothing.
 */
#if DT_NODE_HAS_STATUS_OKAY(EXP_NODE)

#define LCD_EXP_POR_PRIO 74

BUILD_ASSERT(LCD_EXP_POR_PRIO > CONFIG_GPIO_PCA_SERIES_INIT_PRIORITY &&
                 LCD_EXP_POR_PRIO < CONFIG_REGULATOR_FIXED_INIT_PRIORITY,
             "the POR snapshot must run after the expander and before the panel regulator");

/*
 * The same window is what the shield's RESX hog depends on, and nothing else in
 * the tree checks it: a Kconfig default cannot assert against another symbol.
 */
BUILD_ASSERT(CONFIG_GPIO_HOGS_INIT_PRIORITY > CONFIG_GPIO_PCA_SERIES_INIT_PRIORITY &&
                 CONFIG_GPIO_HOGS_INIT_PRIORITY < CONFIG_REGULATOR_FIXED_INIT_PRIORITY,
             "the lcd_exp RESX hog must run after the expander and before the panel regulator");

static uint8_t lcd_exp_por_val[3];
static int     lcd_exp_por_rc[3];

static int lcd_exp_por_snapshot(void)
{
	static const uint8_t regs[3] = { 0x00, 0x01, 0x03 };

	for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
		lcd_exp_por_rc[i] = i2c_reg_read_byte_dt(&lcd_exp_i2c, regs[i], &lcd_exp_por_val[i]);
	}
	return 0;
}

SYS_INIT(lcd_exp_por_snapshot, POST_KERNEL, LCD_EXP_POR_PRIO);

#endif /* lcd_exp okay */

/*
 * BENCH DIAGNOSTIC -- Alif P5_2, the audio-amp SD_N shared with the I2C2 bus
 * users (#2199).  0x4e appeared in one cold cycle's scan and not the next, and
 * SD_N low is the known reason the TAS2563 amps drop off I2C, so the two want
 * correlating.
 *
 * READ-ONLY, by design: P5_2 is NOT muxed to GPIO and NOT driven here.  Its
 * input receiver may be off, in which case the gpio5 port word reads 0 for that
 * bit regardless of the pad's real level -- so the pad config register is
 * printed alongside, and the three amp addresses' presence is printed
 * explicitly rather than inferred.  Muxing the pad to read it would change a
 * live amp-shutdown line, which is not a diagnostic's business.
 *
 * Pad register: PINMUX base 0x1a603000 + (port * 8 + pin) * 4; P5_2 is
 * (5 * 8 + 2) * 4 = 0xa8.  Bit 16 is REN, the input-receiver enable.
 */
#define P5_2_PAD_ADDR 0x1A6030A8UL
#define P5_2_PIN      2

static void scan_i2c2(const char *stage)
{
	const struct device *bus  = DEVICE_DT_GET(DT_NODELABEL(i2c2));
	const struct device *gp5  = DEVICE_DT_GET(DT_NODELABEL(gpio5));
	bool                 a48  = false;
	bool                 a4d  = false;
	bool                 a4e  = false;
	gpio_port_value_t    p5   = 0;
	int                  p5rc = -ENODEV;

	if (!device_is_ready(bus)) {
		printk("i2c2 scan[%s]: bus not ready\n", stage);
		return;
	}

	printk("i2c2 scan[%s]:", stage);
	for (uint16_t addr = 0x08; addr < 0x78; addr++) {
		uint8_t v;

		if (i2c_read(bus, &v, 1, addr) == 0) {
			printk(" 0x%02x", addr);
			a48 = a48 || (addr == 0x48);
			a4d = a4d || (addr == 0x4d);
			a4e = a4e || (addr == 0x4e);
		}
	}

	if (device_is_ready(gp5)) {
		p5rc = gpio_port_get_raw(gp5, &p5);
	}
	printk(" | 0x48=%d 0x4d=%d 0x4e=%d p5_2-pad=0x%08x p5-port=0x%08x(rc%d) p5_2=%d\n",
	       (int)a48,
	       (int)a4d,
	       (int)a4e,
	       sys_read32(P5_2_PAD_ADDR),
	       (uint32_t)p5,
	       p5rc,
	       (int)((p5 >> P5_2_PIN) & 1U));
	printk("  (p5_2 level valid only if the pad's REN bit16 is set; not muxed, not driven)\n");
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

/*
 * BENCH DIAGNOSTIC -- one DCS read with its own error class (#2199).
 *
 * INT_ST0/INT_ST1 are read-to-clear, and dsi_dw's ISR reads both on every
 * interrupt purely to log them, so by the time the caller sees an -EIO the
 * latch that says WHY is already gone.  Masking the DSI IRQ for the duration
 * and clearing the latches immediately before the transfer makes whatever is
 * set afterwards belong to THIS read: ACK-with-error/D-PHY bits in int0, the
 * timeout (TO_HS_TX bit 0 / TO_LP_RX bit 1), ECC, CRC, PKT_SIZE and
 * GEN_PLD_RD/RECEV bits in int1.  Raw, undecoded -- the host decodes.
 */
static ssize_t dcs_read_classified(const struct device *dsi,
                                   uint8_t              cmd,
                                   void                *buf,
                                   size_t               len,
                                   uint32_t            *int0,
                                   uint32_t            *int1)
{
	unsigned int irq = DT_IRQN(DSI_NODE);
	ssize_t      rc;

	irq_disable(irq);
	(void)sys_read32(DSI_INT_ST0_ADDR);
	(void)sys_read32(DSI_INT_ST1_ADDR);

	rc = dcs_read_lpm(dsi, cmd, buf, len);

	*int0 = sys_read32(DSI_INT_ST0_ADDR);
	*int1 = sys_read32(DSI_INT_ST1_ADDR);
	irq_enable(irq);
	return rc;
}

static bool probe_panel_reads(const struct device *dsi)
{
	/*
	 * The 11 standard reads, plus two that bracket the short/long response
	 * boundary the bench is measuring: every rx_len=1 read has answered while
	 * RDDID (3) and RDDDB (5) never have.  RDDST2 asks for 2 bytes, which the
	 * panel still answers with a SHORT packet, and RDDID1 asks for 1 byte of a
	 * normally-3-byte read -- so the pair separates "rx_len > 1" from
	 * "response is a LONG packet".
	 */
	static const struct {
		uint8_t     cmd;
		uint8_t     len;
		const char *name;
	} probes[] = {
		{ 0x04, 3, "RDDID" },     { 0x09, 4, "RDDST" },     { 0x0A, 1, "RDDPM" },
		{ 0x0B, 1, "RDDMADCTL" }, { 0x0C, 1, "RDDCOLMOD" }, { 0x0D, 1, "RDDIM" },
		{ 0x0E, 1, "RDDSM" },     { 0xA1, 5, "RDDDB" },     { 0xDA, 1, "RDID1" },
		{ 0xDB, 1, "RDID2" },     { 0xDC, 1, "RDID3" },     { 0x09, 2, "RDDST2" },
		{ 0x04, 1, "RDDID1" },
	};
	uint8_t buf[5];
	bool    any_nonzero = false;

	for (size_t i = 0; i < ARRAY_SIZE(probes); i++) {
		for (size_t j = 0; j < sizeof(buf); j++) {
			buf[j] = 0;
		}

		uint32_t int0 = 0;
		uint32_t int1 = 0;
		ssize_t  rc   = dcs_read_classified(dsi, probes[i].cmd, buf, probes[i].len, &int0, &int1);
		bool     nonzero = false;
		for (size_t j = 0; j < probes[i].len; j++) {
			nonzero = nonzero || (buf[j] != 0);
		}
		any_nonzero = any_nonzero || (rc > 0 && nonzero);

		printk("panel DCS read: %-9s cmd=0x%02x len=%u rc=%d data=%02x %02x %02x %02x %02x "
		       "int0=0x%08x int1=0x%08x\n",
		       probes[i].name,
		       probes[i].cmd,
		       probes[i].len,
		       (int)rc,
		       buf[0],
		       buf[1],
		       buf[2],
		       buf[3],
		       buf[4],
		       int0,
		       int1);
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
#if DT_NODE_HAS_STATUS_OKAY(EXP_NODE)
	/* Captured before regulator_fixed ran -- see lcd_exp_por_snapshot(). */
	printk("lcd-exp regs[por]: in=%02x(rc%d) out=%02x(rc%d) cfg=%02x(rc%d)\n",
	       lcd_exp_por_val[0],
	       lcd_exp_por_rc[0],
	       lcd_exp_por_val[1],
	       lcd_exp_por_rc[1],
	       lcd_exp_por_val[2],
	       lcd_exp_por_rc[2]);
#endif
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

	/*
	 * Step 2: HX8394 init does mipi_dsi_attach + DCS power-on over DSI, and
	 * drives bl-gpios high only if all of it succeeded -- so the level is the
	 * panel driver's own verdict.
	 */
	bool panel_ok = dev_ready("panel", panel);

	printk("%-8s: level=%d\n", "backlight", gpio_pin_get_dt(&bl_gpio));

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
	bool write_ok   = false;
	bool scanout_ok = false;
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
			write_ok = true;
			printk("display_write: full 720x1280 frame OK (0x%04x)\n", FILL_COLOR_RGB565);
			/* Start scanout: DSI video mode + CDC_EN, so the FB reaches glass. */
			rc = display_blanking_off(disp);
			printk("blanking_off: rc=%d cdc-glb=0x%08x\n", rc, sys_read32(CDC_GLB_CTRL_ADDR));
			if (rc == 0 && dsi_ok) {
				dump_dsi_status("after-blanking-off");
				scanout_ok = check_scanout();

				/*
				 * BENCH DIAGNOSTIC (#2199): RDDPM once more, now that
				 * video is streaming.  The PASS run reported sleep-out +
				 * normal mode + display-on with the BOOSTER bit (bit 7)
				 * clear and the glass black under a lit backlight, so the
				 * open question is whether the booster comes up when the
				 * scanout starts.  Informational; the gate is above.
				 */
				uint8_t  pm    = 0;
				uint32_t pint0 = 0;
				uint32_t pint1 = 0;
				ssize_t  prc   = dcs_read_classified(dsi, 0x0A, &pm, 1, &pint0, &pint1);

				printk("panel DCS read: %-9s cmd=0x0a len=1 rc=%d data=%02x "
				       "int0=0x%08x int1=0x%08x\n",
				       "RDDPM/vid",
				       (int)prc,
				       pm,
				       pint0,
				       pint1);
			}
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

	bool pass = exp_ok && pwr_ok && panel_ok && dsi_ok && disp_ok && panel_read_ok && write_ok &&
	            scanout_ok;

	if (pass) {
		printk("RESULT PASS: RK055HDMIPI4MA0 chain UP -- hx8394 panel + mipi-dsi "
		       "+ cdc200 display ready, full-screen RGB565 frame written and "
		       "scanning out cleanly; pixels-on-glass: confirm green on the panel\n");
	} else {
		printk("RESULT FAIL: DSI display chain not fully up "
		       "(lcd-exp=%d lcd-reg=%d panel=%d mipi-dsi=%d display=%d dcs-read=%d write=%d "
		       "scanout=%d) -- see the per-stage lines above\n",
		       (int)exp_ok,
		       (int)pwr_ok,
		       (int)panel_ok,
		       (int)dsi_ok,
		       (int)disp_ok,
		       (int)panel_read_ok,
		       (int)write_ok,
		       (int)scanout_ok);
	}

	return 0;
}
