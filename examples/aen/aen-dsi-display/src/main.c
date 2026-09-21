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
 * the DSI IRQ around a transfer so the read-to-clear error latches survive,
 * snapshot the expander before the panel regulator runs, and scan the panel
 * flex's touch I2C -- the one check that does not use the DSI link at all, so
 * it says whether the flex is seated and the panel side powered.  They print
 * raw values and decode nothing.  A production app needs none of this.
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

/*
 * DSI_PHY_STATUS bit 2: D-PHY clock lane is in LP stop state.  A BTA (needed
 * for every DCS read) has no chance of an answer while this is clear -- the
 * clock lane is still HS and the panel isn't listening for the turnaround.
 */
#define DSI_PHY_STATUS_STOPSTATECLKLANE BIT(2)

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

/*
 * BENCH DIAGNOSTIC -- the panel flex's TOUCH I2C (#2199).
 *
 * This is the one check in the app that does NOT go through the DSI link, so it
 * separates "the flex is not seated / the panel side is unpowered" from "the
 * DSI link is unhappy": the touch controller sits on the same flex as the
 * display and answers on its own bus.  If touch answers and DCS does not, the
 * flex is in and powered and the problem is the DSI side; if neither answers,
 * suspect the flex or the panel supply.
 *
 * The bus is SoC I2C1 (E1M_I2C1 / EVK_I2C_BUS_DSI_CSI): J6 pin 26/27 through a
 * level translator to E1M pads AH17 / AG17, i.e. Alif P7_2 (I2C1_SDA_C) and
 * P3_7 (I2C1_SCL_B).  The shield enables the node and its pinctrl group.
 *
 * Touch reset is expander U35 P3 (CTP_RST, out to J6 pin 28), driven HIGH here
 * to RELEASE reset.  The controller latches its 7-bit address out of reset --
 * 0x5D (default) or 0x14, the datasheet's 8-bit pairs 0xBA/0xBB and 0x28/0x29 --
 * and its power-on/firmware load takes tens of milliseconds, so the scan waits
 * 50 ms after the release or it reads a bus that is not answering yet.  Its INT
 * line goes to the CC3501E's GPIO_15, not to an Alif pad, so nothing here can
 * drive or observe INT.
 *
 * READ-ONLY on the touch side: no touch driver, no register write.  A
 * GT911-class part exposes its product ID at 16-bit register 0x8140 ("911" in
 * ASCII plus a NUL), which is read and printed raw if one of the two addresses
 * answers.
 */
#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(i2c1)) && DT_NODE_HAS_STATUS_OKAY(EXP_NODE)
#define TOUCH_I2C_READY 1

#define CTP_RST_EXP_PIN      3
#define CTP_RESET_SETTLE_MS  50
#define TOUCH_ADDR_DEFAULT   0x5D
#define TOUCH_ADDR_ALT       0x14
#define TOUCH_PRODUCT_ID_REG 0x8140

static void scan_i2c1_touch(const struct device *exp)
{
	const struct device *bus   = DEVICE_DT_GET(DT_NODELABEL(i2c1));
	uint16_t             hit   = 0;
	int                  rrc   = gpio_pin_configure(exp, CTP_RST_EXP_PIN, GPIO_OUTPUT_HIGH);
	uint8_t              id[4] = { 0 };

	printk(
	    "ctp-rst : expander P3 released high (rc%d), settling %u ms\n", rrc, CTP_RESET_SETTLE_MS);
	k_msleep(CTP_RESET_SETTLE_MS);

	if (!device_is_ready(bus)) {
		printk("i2c1 scan[touch]: bus not ready\n");
		return;
	}

	printk("i2c1 scan[touch]:");
	for (uint16_t addr = 0x08; addr < 0x78; addr++) {
		uint8_t v;

		if (i2c_read(bus, &v, 1, addr) == 0) {
			printk(" 0x%02x", addr);
			if (addr == TOUCH_ADDR_DEFAULT || addr == TOUCH_ADDR_ALT) {
				hit = addr;
			}
		}
	}
	printk("\n");

	if (hit == 0) {
		printk("touch   : NO ANSWER at 0x%02x or 0x%02x -- flex not seated, panel side "
		       "unpowered, or touch held in reset\n",
		       TOUCH_ADDR_DEFAULT,
		       TOUCH_ADDR_ALT);
		return;
	}

	/* 16-bit register address, big-endian on the wire. */
	uint8_t reg[2] = { TOUCH_PRODUCT_ID_REG >> 8, TOUCH_PRODUCT_ID_REG & 0xFF };
	int     prc    = i2c_write_read(bus, hit, reg, sizeof(reg), id, sizeof(id));

	printk("touch   : RESPONDING at 0x%02x (%s) product-id[0x%04x] rc=%d data=%02x %02x %02x "
	       "%02x\n",
	       hit,
	       hit == TOUCH_ADDR_DEFAULT ? "default" : "alternate",
	       TOUCH_PRODUCT_ID_REG,
	       prc,
	       id[0],
	       id[1],
	       id[2],
	       id[3]);
}

#endif /* i2c1 + lcd_exp okay */

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
 * One scanline of solid colour, reused for every row via display_write's
 * per-call descriptor.  A full 720x1280 frame is 1.8-2.8 MB depending on the
 * layer format -- far too big for a stack/static buffer in ITCM -- so we stream
 * it one row at a time straight into the SRAM0 framebuffer the driver owns.
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
#define PANEL_FMT_IS_RGB888                                                                        \
	DT_ENUM_HAS_VALUE(DT_NODELABEL(cdc200), pixel_fmt_l1, rgb_888)
#define PANEL_FMT_IS_RGB565                                                                        \
	DT_ENUM_HAS_VALUE(DT_NODELABEL(cdc200), pixel_fmt_l1, rgb_565)

BUILD_ASSERT(PANEL_FMT_IS_RGB888 || PANEL_FMT_IS_RGB565,
	     "aen-dsi-display fills only rgb-888 or rgb-565; teach it the shield's pixel-fmt-l1");

#if PANEL_FMT_IS_RGB888
#define PANEL_BYTES_PER_PIXEL 3U
#else
#define PANEL_BYTES_PER_PIXEL 2U
#endif

/* Solid green in whichever format the layer is actually configured for. */
static uint8_t row_buf[PANEL_W * PANEL_BYTES_PER_PIXEL];

static void fill_row_green(void)
{
	for (uint16_t i = 0; i < PANEL_W; i++) {
#if PANEL_FMT_IS_RGB888
		row_buf[i * 3U + 0U] = 0x00U; /* B */
		row_buf[i * 3U + 1U] = 0xFFU; /* G */
		row_buf[i * 3U + 2U] = 0x00U; /* R */
#else
		/* RGB565 little-endian 0x07E0 = green. */
		row_buf[i * 2U + 0U] = 0xE0U;
		row_buf[i * 2U + 1U] = 0x07U;
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

/*
 * Panel-state sentinel, prefixed onto every probe below that assumes the
 * panel is awake (Sleep Out, Display On).  A RESX pulse hardware-resets the
 * panel to Sleep In / Display Off / booster off if nothing re-wakes it
 * afterward -- see panel_wake_after_resx() -- so a probe that skips this
 * check can silently measure a sleeping panel and its null result gets read
 * as real evidence.  Never skips the probe itself; only marks the output.
 */
static void probe_report_panel_state(const char *prefix, const struct device *dsi)
{
	uint8_t  rddpm = 0xAAU;
	uint32_t int0  = 0;
	uint32_t int1  = 0;
	ssize_t  rc    = dcs_read_classified(dsi, 0x0AU, &rddpm, 1U, &int0, &int1);

	if (rc < 1) {
		printk("%s: panel-state RDDPM read FAILED rc=%d -- panel state UNKNOWN, not asleep\n",
		       prefix,
		       (int)rc);
		return;
	}

	int sleep_out  = (int)((rddpm & BIT(4)) != 0);
	int display_on = (int)((rddpm & BIT(2)) != 0);

	printk("%s: panel-state RDDPM=0x%02x sleep_out=%d display_on=%d\n",
	       prefix,
	       rddpm,
	       sleep_out,
	       display_on);

	if (!sleep_out || !display_on) {
		printk("%s: !! PANEL IS ASLEEP OR DISPLAY OFF -- this probe's result is NOT "
		       "valid evidence\n",
		       prefix);
	}
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

/*
 * THE decisive test (#2199): does SETEXTC (B9h FF 83 94) -- the extension-
 * command unlock -- actually land on this panel?
 *
 * It matters because if it does not, every manufacturer register stays at
 * power-on default, and the D5h POR default is all 18h = "constant VGL, every
 * gate closed".  No pixel would ever be driven, while RDDPM/RDDST/RDDID keep
 * answering normally because those are standard DCS -- exactly the symptom
 * this board shows.  The alternative is that the unlock works and the panel
 * module itself is faulty.
 *
 * Every GATED manufacturer READ on this board is dead (proven with a
 * positive control: SETPANEL (CCh), which this app's own init writes to
 * 0x03, reads back 00 by both the direct generic read and the FEh/FFh
 * mechanism), so no gated readback can answer this.  probe_gate_and_length()
 * (the ddb: probe) suggested gated WRITES land -- SETDDB (C4h) followed by
 * RDDDB (A1h) came back non-zero -- but it has no negative control, and
 * RDDDB had previously returned a stale FIFO echo of a just-sent payload, so
 * it is not safe to close on.
 *
 * The trick that makes this answerable: RDDDB (A1h) is a STANDARD DCS read,
 * and standard DCS reads work perfectly on this board.  So a gated register
 * can be written and verified through a working read path -- with an actual
 * negative control this time.
 *
 * Two arms, each starting with a hardware reset (the shield's HX8394 driver
 * already sent its own SETEXTC at POST_KERNEL, before main() ever runs;
 * SETEXTC is disabled again after reset, so the RESX pulse is what makes the
 * locked arm a genuine negative control):
 *
 *   A) gate CLOSED -- RESX, read RDDDB, SETDDB(P1) with NO SETEXTC first,
 *      flush the FIFO with an unrelated read (RDDMADCTL), read RDDDB again.
 *      P1 must NOT appear.
 *   B) gate OPEN -- RESX, read RDDDB, SETEXTC, SETDDB(P2), flush, read RDDDB
 *      again.  P2 SHOULD appear.
 *
 * P1 (11 22 33 44 55 66) and P2 (A5 5A C3 3C 96 69) are deliberately unlike
 * any payload the panel init sends, and unlike each other, so a stale echo
 * cannot fake either result.
 *
 * Runs FIRST of every dsi_ok probe in main() -- several later probes
 * (probe_gated_read_path() and beyond) send SETEXTC themselves and would
 * unlock the panel behind this test's back.
 *
 * RESX pulsing goes through exp_set_resx() below -- a guarded read-modify-write
 * on the expander's output register -- rather than open-coding an expander
 * write, so P0 (panel supply enable) and P3 (touch reset) are never disturbed.
 */

/*
 * Expander output register and the RESX bit.
 *
 * RESX is expander P1, confirmed two ways: the carrier netlist maps the panel
 * reset net to this expander's P1, and the bench `rstx:` probe read back
 * `cfg=0xf4`, i.e. bit 1 configured as an output.  P0 is the panel supply
 * enable and P3 is the touch-controller reset -- the read-modify-write below
 * exists so neither is ever clobbered.
 */
#define EXP_OUTPUT_REG 0x01U
#define EXP_RESX_BIT   BIT(1)

/*
 * Guarded RESX drive.  Returns false WITHOUT writing if the output-register
 * read fails: on a failed read `out` would stay 0 and the write would drive
 * P1 and P3 low too, asserting panel reset and holding the touch controller
 * in reset -- which reads exactly like "the panel rail is dead" and has
 * already cost this bring-up a wasted run.
 */
static bool exp_set_resx(int level)
{
	uint8_t out = 0;
	int     rr  = i2c_reg_read_byte_dt(&lcd_exp_i2c, EXP_OUTPUT_REG, &out);

	if (rr != 0) {
		printk("exp: RESX ABORT -- output-register read failed rc=%d, refusing RMW\n", rr);
		return false;
	}

	uint8_t want = level ? (out | EXP_RESX_BIT) : (uint8_t)(out & ~EXP_RESX_BIT);
	int     wr   = i2c_reg_write_byte_dt(&lcd_exp_i2c, EXP_OUTPUT_REG, want);

	printk("exp: RESX -> %s  out-read-rc=%d wrote=0x%02x write-rc=%d\n",
	       level ? "HIGH" : "LOW",
	       rr,
	       want,
	       wr);
	return true;
}

/*
 * Recovery sequence owed after ANY RESX pulse (datasheet Sec5.12 pp.86-87).
 * RESX hardware-resets the panel to Sleep In / Display Off / booster off, and
 * every manufacturer register (SETEXTC and everything gated behind it)
 * reverts to its power-on default too -- proven on the bench:
 * probe_setextc_gate_v2()'s own final RESX pulse left RDDST=00 71 00 00
 * (booster off, Sleep In, Display Off) and RDDPM=0x08 (the datasheet's
 * documented HW-reset default, Sec5.18.11 p.136), with every probe that ran
 * afterward none the wiser.  A caller that pulses RESX and does not call this
 * leaves every later probe measuring a panel that cannot drive glass.
 *
 * Re-sends SETEXTC first, purely so a caller that immediately touches a
 * gated register afterward does not have to reopen the gate itself --
 * EXIT_SLEEP_MODE/SET_DISPLAY_ON are standard DCS and do not need it.
 */
static void panel_wake_after_resx(const struct device *dsi)
{
	static const uint8_t extc_cmd[] = { 0xB9U, 0xFFU, 0x83U, 0x94U };
	struct mipi_dsi_msg  extc_w     = {
		.type   = MIPI_DSI_GENERIC_LONG_WRITE,
		.flags  = MIPI_DSI_MSG_USE_LPM,
		.tx_buf = extc_cmd,
		.tx_len = sizeof(extc_cmd),
	};
	ssize_t extc_rc = mipi_dsi_transfer(dsi, 0, &extc_w);

	printk("wake: SETEXTC (B9h FF 83 94) rc=%d\n", (int)extc_rc);

	struct mipi_dsi_msg slpout_w = {
		.type  = MIPI_DSI_DCS_SHORT_WRITE,
		.flags = MIPI_DSI_MSG_USE_LPM,
		.cmd   = 0x11U, /* EXIT_SLEEP_MODE */
	};
	ssize_t slpout_rc = mipi_dsi_transfer(dsi, 0, &slpout_w);

	/* Datasheet Sec5.12 -- 120 ms dwell after EXIT_SLEEP_MODE, not a guess. */
	k_msleep(120);

	struct mipi_dsi_msg dispon_w = {
		.type  = MIPI_DSI_DCS_SHORT_WRITE,
		.flags = MIPI_DSI_MSG_USE_LPM,
		.cmd   = 0x29U, /* SET_DISPLAY_ON */
	};
	ssize_t dispon_rc = mipi_dsi_transfer(dsi, 0, &dispon_w);

	uint8_t  rddst[4];
	uint8_t  rddpm      = 0xAAU;
	uint32_t rddst_int0 = 0;
	uint32_t rddst_int1 = 0;
	uint32_t rddpm_int0 = 0;
	uint32_t rddpm_int1 = 0;

	memset(rddst, 0xAAU, sizeof(rddst));
	(void)dcs_read_classified(dsi, 0x09U, rddst, sizeof(rddst), &rddst_int0, &rddst_int1);
	(void)dcs_read_classified(dsi, 0x0AU, &rddpm, 1U, &rddpm_int0, &rddpm_int1);

	int sleep_out  = (int)((rddpm & BIT(4)) != 0);
	int display_on = (int)((rddpm & BIT(2)) != 0);

	printk("wake: SLPOUT rc=%d DISPON rc=%d RDDST=%02x %02x %02x %02x RDDPM=%02x sleep_out=%d "
	       "display_on=%d\n",
	       (int)slpout_rc,
	       (int)dispon_rc,
	       rddst[0],
	       rddst[1],
	       rddst[2],
	       rddst[3],
	       rddpm,
	       sleep_out,
	       display_on);

	if (!sleep_out || !display_on) {
		printk("wake: !! PANEL DID NOT WAKE -- SLEEP IN OR DISPLAY OFF, every probe "
		       "after this one is measuring a panel that cannot drive glass\n");
	}
}

static void probe_setextc_gate_v2(const struct device *dsi)
{
	static const uint8_t extc_cmd[] = { 0xB9U, 0xFFU, 0x83U, 0x94U };
	static const uint8_t p1[]       = { 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U };
	static const uint8_t p2[]       = { 0xA5U, 0x5AU, 0xC3U, 0x3CU, 0x96U, 0x69U };
	uint8_t              ddb_a_base[5];
	uint8_t              ddb_a_after[5];
	uint8_t              ddb_b_base[5];
	uint8_t              ddb_b_after[5];
	ssize_t              r;

#define RESX_PULSE(label) \
	do { \
		if (!exp_set_resx(0)) { \
			printk("extc: %s ABORT -- RESX-low failed, no genuine negative " \
			       "control possible\n", \
			       (label)); \
			return; \
		} \
		k_msleep(20); \
		if (!exp_set_resx(1)) { \
			printk("extc: %s ABORT -- RESX-high failed\n", (label)); \
			return; \
		} \
		k_msleep(60); \
	} while (0)

#define DDB_READ(dst, tag) \
	do { \
		memset((dst), 0xAAU, sizeof(dst)); \
		r = dcs_read_lpm(dsi, 0xA1U, (dst), sizeof(dst)); \
		printk("extc: %s rc=%d data=%02x %02x %02x %02x %02x%s\n", \
		       (tag), \
		       (int)r, \
		       (dst)[0], \
		       (dst)[1], \
		       (dst)[2], \
		       (dst)[3], \
		       (dst)[4], \
		       (r == (ssize_t)sizeof(dst)) ? "" : "  <- READ FAILED, sentinel"); \
	} while (0)

#define MADCTL_FLUSH(tag) \
	do { \
		uint8_t v  = 0xAAU; \
		ssize_t rr = dcs_read_lpm(dsi, 0x0BU, &v, 1U); \
		printk("extc: %s RDDMADCTL(flush) rc=%d data=0x%02x%s\n", \
		       (tag), \
		       (int)rr, \
		       v, \
		       (rr == 1) ? "" : "  <- READ FAILED, sentinel"); \
	} while (0)

	/* --- Arm A: gate CLOSED (negative control). --- */
	RESX_PULSE("A");
	DDB_READ(ddb_a_base, "A baseline");
	{
		struct mipi_dsi_msg w = {
			.type   = MIPI_DSI_DCS_LONG_WRITE,
			.flags  = MIPI_DSI_MSG_USE_LPM,
			.cmd    = 0xC4U,
			.tx_buf = p1,
			.tx_len = sizeof(p1),
		};
		printk("extc: A SETDDB(C4h 11 22 33 44 55 66) rc=%d expect=%u -- no SETEXTC "
		       "sent\n",
		       (int)mipi_dsi_transfer(dsi, 0, &w),
		       (unsigned int)sizeof(p1));
	}
	k_msleep(20);
	MADCTL_FLUSH("A");
	DDB_READ(ddb_a_after, "A after-P1");

	/* --- Arm B: gate OPEN. --- */
	RESX_PULSE("B");
	DDB_READ(ddb_b_base, "B baseline");
	{
		struct mipi_dsi_msg w = {
			.type   = MIPI_DSI_GENERIC_LONG_WRITE,
			.flags  = MIPI_DSI_MSG_USE_LPM,
			.tx_buf = extc_cmd,
			.tx_len = sizeof(extc_cmd),
		};
		printk("extc: B SETEXTC (B9h FF 83 94) rc=%d expect=%u\n",
		       (int)mipi_dsi_transfer(dsi, 0, &w),
		       (unsigned int)sizeof(extc_cmd));
	}
	k_msleep(20);
	{
		struct mipi_dsi_msg w = {
			.type   = MIPI_DSI_DCS_LONG_WRITE,
			.flags  = MIPI_DSI_MSG_USE_LPM,
			.cmd    = 0xC4U,
			.tx_buf = p2,
			.tx_len = sizeof(p2),
		};
		printk("extc: B SETDDB(C4h A5 5A C3 3C 96 69) rc=%d expect=%u\n",
		       (int)mipi_dsi_transfer(dsi, 0, &w),
		       (unsigned int)sizeof(p2));
	}
	k_msleep(20);
	MADCTL_FLUSH("B");
	DDB_READ(ddb_b_after, "B after-P2");

#undef MADCTL_FLUSH
#undef DDB_READ
#undef RESX_PULSE

	/*
	 * --- Verdict. ---
	 *
	 * SETDDB (C4h) takes SIX payload bytes but RDDDB (A1h) returns only FIVE,
	 * so the pattern match compares the five bytes the read actually yields --
	 * NOT sizeof(p1)/sizeof(p2), which is 6 and reads one byte past the read
	 * buffer.  Confirmed on the bench: `SETDDB (C4h 5A A5 5A A5 5A A5) rc=6`
	 * came back as `RDDDB rc=5 5a a5 5a a5 5a`, i.e. the first five written
	 * bytes.  The compiler caught the 6-vs-5 overread here
	 * (-Wstringop-overread); left alone it would have made p1_seen/p2_seen
	 * compare out of bounds and decide this test wrongly.
	 */
	BUILD_ASSERT(sizeof(ddb_a_after) == 5U, "RDDDB returns 5 bytes");

	const size_t ddb_cmp = sizeof(ddb_a_after);

	bool armA_changed = memcmp(ddb_a_base, ddb_a_after, ddb_cmp) != 0;
	bool armB_changed = memcmp(ddb_b_base, ddb_b_after, ddb_cmp) != 0;
	bool p1_seen      = memcmp(ddb_a_after, p1, ddb_cmp) == 0;
	bool p2_seen      = memcmp(ddb_b_after, p2, ddb_cmp) == 0;

	printk("extc: VERDICT armA_changed=%d armB_changed=%d p1_seen=%d p2_seen=%d\n",
	       (int)armA_changed,
	       (int)armB_changed,
	       (int)p1_seen,
	       (int)p2_seen);

	if (!armA_changed && p2_seen) {
		printk("extc: SETEXTC GATES AS EXPECTED, gated writes land\n");
	} else if (p1_seen) {
		printk("extc: C4h IS NOT GATED -- this test cannot speak to SETEXTC, and "
		       "neither could the earlier ddb probe\n");
	} else if (!p2_seen) {
		printk("extc: SETEXTC DOES NOT LAND -- manufacturer registers are at POR\n");
	} else {
		printk("extc: INDETERMINATE -- raw: A-base=%02x %02x %02x %02x %02x "
		       "A-after=%02x %02x %02x %02x %02x B-base=%02x %02x %02x %02x %02x "
		       "B-after=%02x %02x %02x %02x %02x\n",
		       ddb_a_base[0],
		       ddb_a_base[1],
		       ddb_a_base[2],
		       ddb_a_base[3],
		       ddb_a_base[4],
		       ddb_a_after[0],
		       ddb_a_after[1],
		       ddb_a_after[2],
		       ddb_a_after[3],
		       ddb_a_after[4],
		       ddb_b_base[0],
		       ddb_b_base[1],
		       ddb_b_base[2],
		       ddb_b_base[3],
		       ddb_b_base[4],
		       ddb_b_after[0],
		       ddb_b_after[1],
		       ddb_b_after[2],
		       ddb_b_after[3],
		       ddb_b_after[4]);
	}

	/*
	 * Arm B's RESX pulse (above) left the panel hardware-reset -- every
	 * probe scheduled after this one must see an awake panel, not the
	 * Sleep In / Display Off state RESX leaves behind.
	 */
	panel_wake_after_resx(dsi);
}

/*
 * DSI_INT_ST1/ST0 bit positions this probe classifies on.  TO_LP_RX is the
 * link-level LP-RX (BTA reverse-turnaround) timeout; DPHY_ERR_4 is a D-PHY LP
 * contention flag.  Named locally rather than pulled from a driver-private
 * header, same reasoning as WEDGE_GEN_* above.
 */
#define TA_INT1_TO_LP_RX   BIT(1)  /* DSI_INT_1_TO_LP_RX */
#define TA_INT0_DPHY_ERR_4 BIT(20) /* DSI_INT_0_DPHY_ERR_4 */

/*
 * BENCH DIAGNOSTIC -- probe_ta_timing() (#2199).
 *
 * Every FAILED DCS read observed on this board carries INT_ST1 = TO_LP_RX
 * (bit 1, LP-RX timeout); every SUCCESSFUL read carries INT_ST0 bit 20 set
 * (DPHY_ERR_4, an LP contention flag) -- a link whose every good turnaround
 * still raises a contention flag is marginal.  This probe tests whether
 * SETMIPI's (BAh, datasheet Sec5.19.9 pp.209-210) reverse-turnaround timing
 * is why: the init sends `BA 61 03 68 6B B2 C0`, i.e. DSISETUP0=0x61
 * (bit4=0, TLPX=50 ns) and DSISETUP1=0x03 (bits[3:2]=00, T_TA-GO = 2 TLPX =
 * 100 ns at 50 ns TLPX) -- half the 4 TLPX the datasheet's own Figure 4.15
 * p.32 draws for the same transition.  If 100 ns is too tight for this host
 * to see the reverse turnaround, reads fail with LP-RX timeout and late
 * payloads land in the NEXT read's FIFO -- exactly the observed signature.
 *
 * Four variants, only DSISETUP0/DSISETUP1 (payload bytes 1-2) change; bytes
 * 3-6 (68 6B B2 C0) stay exactly what the init sends in every variant:
 *
 *   V0  61 03  baseline -- what the init sends, the control
 *   V1  61 0F  DSISETUP1[3:2]=11, T_TA-GO = 8 TLPX
 *   V2  71 03  DSISETUP0[4]=1,    TLPX = 100 ns
 *   V3  71 0F  both
 *
 * Every read goes through dcs_read_classified() -- masking the DSI IRQ around
 * the transfer so the read-to-clear INT_ST0/INT_ST1 latches survive to be
 * read back, instead of being consumed by the ISR first (see that helper's
 * banner).  A raw read here would make every int0/int1 in this probe
 * worthless.
 *
 * Runs in command mode only, BEFORE probe_read_wedge() and before anything
 * enters video mode (see the call site in main()) -- video contaminates the
 * read-success measurement this probe depends on.  Does NOT pulse RESX: the
 * panel must stay exactly where the driver's init left it (Sleep Out,
 * Display On), so RDDST is read once at entry to confirm that rather than
 * assumed.  Runs AFTER probe_setextc_gate_v2(), which must be the first
 * probe to send SETEXTC (see its own banner) -- this probe also sends
 * SETEXTC (SETMIPI is gated behind it, same as SETDISP/SETDDB), so it
 * respects that ordering rather than re-litigating it.
 *
 * All BAh writes here are volatile register writes, reversed by RESX or by
 * this probe's own restore step at the end -- never SETOTP (BBh) or SETID
 * (C3h), which burn OTP and are never used anywhere in this app.
 */
static void probe_ta_timing(const struct device *dsi)
{
	static const uint8_t extc_cmd[] = { 0xB9U, 0xFFU, 0x83U, 0x94U };
	static const struct {
		uint8_t setup0;
		uint8_t setup1;
	} variants[] = {
		{ 0x61U, 0x03U },
		{ 0x61U, 0x0FU },
		{ 0x71U, 0x03U },
		{ 0x71U, 0x0FU },
	};
	uint8_t  rddst[4];
	ssize_t  rddst_rc;
	uint32_t rddst_int0   = 0;
	uint32_t rddst_int1   = 0;
	int      best_variant = -1;
	int      best_ok      = -1;
	int      second_ok    = -1;

	probe_report_panel_state("ta", dsi);

	memset(rddst, 0xAAU, sizeof(rddst));
	rddst_rc = dcs_read_classified(dsi, 0x09U, rddst, sizeof(rddst), &rddst_int0, &rddst_int1);
	printk("ta: entry RDDST cmd=0x09 rc=%d data=%02x %02x %02x %02x int0=0x%08x int1=0x%08x\n",
	       (int)rddst_rc,
	       rddst[0],
	       rddst[1],
	       rddst[2],
	       rddst[3],
	       rddst_int0,
	       rddst_int1);

	for (size_t v = 0; v < ARRAY_SIZE(variants); v++) {
		uint8_t payload[6] = {
			variants[v].setup0, variants[v].setup1, 0x68U, 0x6BU, 0xB2U, 0xC0U,
		};
		struct mipi_dsi_msg extc_w = {
			.type   = MIPI_DSI_GENERIC_LONG_WRITE,
			.flags  = MIPI_DSI_MSG_USE_LPM,
			.tx_buf = extc_cmd,
			.tx_len = sizeof(extc_cmd),
		};
		ssize_t extc_rc = mipi_dsi_transfer(dsi, 0, &extc_w);

		printk("ta: V%u SETEXTC (B9h FF 83 94) rc=%d\n", (unsigned int)v, (int)extc_rc);

		struct mipi_dsi_msg mipi_w = {
			.type   = MIPI_DSI_DCS_LONG_WRITE,
			.flags  = MIPI_DSI_MSG_USE_LPM,
			.cmd    = 0xBAU,
			.tx_buf = payload,
			.tx_len = sizeof(payload),
		};
		ssize_t mipi_rc = mipi_dsi_transfer(dsi, 0, &mipi_w);

		printk("ta: V%u SETMIPI (BAh) payload=%02x %02x %02x %02x %02x %02x rc=%d\n",
		       (unsigned int)v,
		       payload[0],
		       payload[1],
		       payload[2],
		       payload[3],
		       payload[4],
		       payload[5],
		       (int)mipi_rc);

		k_sleep(K_MSEC(20));

		int      ok         = 0;
		int      to_lp_rx   = 0;
		int      bit20      = 0;
		uint32_t other_int0 = 0;
		uint32_t other_int1 = 0;

		for (int i = 0; i < 30; i++) {
			uint8_t  pm   = 0xAAU;
			uint32_t int0 = 0;
			uint32_t int1 = 0;
			ssize_t  rc   = dcs_read_classified(dsi, 0x0AU, &pm, 1U, &int0, &int1);

			if (rc > 0) {
				ok++;
			}
			if (int1 & TA_INT1_TO_LP_RX) {
				to_lp_rx++;
			}
			if (int0 & TA_INT0_DPHY_ERR_4) {
				bit20++;
			}
			other_int0 |= (int0 & ~(uint32_t)TA_INT0_DPHY_ERR_4);
			other_int1 |= (int1 & ~(uint32_t)TA_INT1_TO_LP_RX);

			printk("ta: V%u read[%2d] rc=%d data=0x%02x int0=0x%08x int1=0x%08x%s\n",
			       (unsigned int)v,
			       i,
			       (int)rc,
			       pm,
			       int0,
			       int1,
			       (rc <= 0) ? "  <- READ FAILED, sentinel" : "");
		}

		printk("ta: V%u payload=%02x %02x 68 6b b2 c0 ok=%d/30 to_lp_rx=%d bit20=%d "
		       "other_int0=0x%08x other_int1=0x%08x\n",
		       (unsigned int)v,
		       payload[0],
		       payload[1],
		       ok,
		       to_lp_rx,
		       bit20,
		       other_int0,
		       other_int1);

		if (ok > best_ok) {
			second_ok    = best_ok;
			best_ok      = ok;
			best_variant = (int)v;
		} else if (ok > second_ok) {
			second_ok = ok;
		}
	}

	/* Restore the baseline the init sent -- SETEXTC first, gate is closed again by RESX-less time. */
	{
		struct mipi_dsi_msg extc_w = {
			.type   = MIPI_DSI_GENERIC_LONG_WRITE,
			.flags  = MIPI_DSI_MSG_USE_LPM,
			.tx_buf = extc_cmd,
			.tx_len = sizeof(extc_cmd),
		};
		ssize_t extc_rc = mipi_dsi_transfer(dsi, 0, &extc_w);

		printk("ta: restore SETEXTC (B9h FF 83 94) rc=%d\n", (int)extc_rc);

		static const uint8_t baseline[6] = { 0x61U, 0x03U, 0x68U, 0x6BU, 0xB2U, 0xC0U };
		struct mipi_dsi_msg  mipi_w      = {
			.type   = MIPI_DSI_DCS_LONG_WRITE,
			.flags  = MIPI_DSI_MSG_USE_LPM,
			.cmd    = 0xBAU,
			.tx_buf = baseline,
			.tx_len = sizeof(baseline),
		};
		ssize_t mipi_rc = mipi_dsi_transfer(dsi, 0, &mipi_w);

		printk("ta: restore SETMIPI (BAh 61 03 68 6b b2 c0) rc=%d\n", (int)mipi_rc);
	}

	printk("ta: VERDICT best=V%d ok=%d/30 margin=%d over next-best\n",
	       best_variant,
	       best_ok,
	       best_ok - second_ok);
}

/*
 * BENCH DIAGNOSTIC -- panel-side DSI-RX health, never checked correctly before
 * (#2199).  dump_dsi_status()/check_scanout() are the Alif DSI HOST's own
 * int0/int1: they say whether the TX side thinks it sent clean video, not
 * whether the HX8394 thinks it RECEIVED clean video.  RDNUMPE (05h)/RDDSM
 * (0Eh)/RDDSDR (0Fh)/RDDST (09h) are the panel's own DSI-RX counters/status --
 * but they are standard DCS reads, and DCS reads answer only in command mode
 * (see the file banner).  The earlier attempt read them while video was
 * already streaming and got only rc=-5 on every field; this version drops to
 * command mode before every read that matters, so a real video interval gets
 * measured instead of POR defaults.
 *
 * Called after main()'s own display_blanking_off() already started video, so
 * the very first thing this probe does is drop back to command mode -- the
 * "before" snapshot below is taken there, not at whatever mode the caller
 * left the chain in.
 */
static void vrx2_snapshot(const struct device *dsi,
                          const char          *stage,
                          uint8_t             *numpe,
                          uint8_t             *rddsm,
                          uint8_t             *rddsdr,
                          uint8_t              rddst[4],
                          ssize_t             *numpe_rc,
                          ssize_t             *rddsm_rc,
                          ssize_t             *rddsdr_rc,
                          ssize_t             *rddst_rc)
{
	*numpe  = 0xAAU;
	*rddsm  = 0xAAU;
	*rddsdr = 0xAAU;
	memset(rddst, 0xAAU, 4U);

#define VRX2_PRE_READ(cmd) \
	do { \
		uint32_t phy = sys_read32(DSI_PHY_STATUS_ADDR); \
		printk("vrx2: %s pre-read cmd=0x%02x phy=0x%08x stopclk=%d\n", \
		       stage, \
		       (cmd), \
		       phy, \
		       (int)((phy & DSI_PHY_STATUS_STOPSTATECLKLANE) != 0)); \
	} while (0)

	VRX2_PRE_READ(0x05U);
	*numpe_rc = dcs_read_lpm(dsi, 0x05U, numpe, 1U);
	VRX2_PRE_READ(0x0EU);
	*rddsm_rc = dcs_read_lpm(dsi, 0x0EU, rddsm, 1U);
	VRX2_PRE_READ(0x0FU);
	*rddsdr_rc = dcs_read_lpm(dsi, 0x0FU, rddsdr, 1U);
	VRX2_PRE_READ(0x09U);
	*rddst_rc = dcs_read_lpm(dsi, 0x09U, rddst, 4U);

#undef VRX2_PRE_READ

	printk("vrx2: %s RDNUMPE cmd=0x05 rc=%d data=%02x%s\n",
	       stage,
	       (int)*numpe_rc,
	       *numpe,
	       (*numpe_rc < 0) ? "  <- READ FAILED, sentinel not data" : "");
	printk("vrx2: %s RDDSM   cmd=0x0e rc=%d data=%02x%s\n",
	       stage,
	       (int)*rddsm_rc,
	       *rddsm,
	       (*rddsm_rc < 0) ? "  <- READ FAILED, sentinel not data" : "");
	printk("vrx2: %s RDDSDR  cmd=0x0f rc=%d data=%02x%s\n",
	       stage,
	       (int)*rddsdr_rc,
	       *rddsdr,
	       (*rddsdr_rc < 0) ? "  <- READ FAILED, sentinel not data" : "");
	printk("vrx2: %s RDDST   cmd=0x09 rc=%d data=%02x %02x %02x %02x%s\n",
	       stage,
	       (int)*rddst_rc,
	       rddst[0],
	       rddst[1],
	       rddst[2],
	       rddst[3],
	       (*rddst_rc < 0) ? "  <- READ FAILED, sentinel not data" : "");
}

static void probe_video_rx_errors_v2(const struct device *dsi, const struct device *disp)
{
	uint8_t numpe_before, rddsm_before, rddsdr_before, rddst_before[4];
	uint8_t numpe_after, rddsm_after, rddsdr_after, rddst_after[4];
	ssize_t numpe_before_rc, rddsm_before_rc, rddsdr_before_rc, rddst_before_rc;
	ssize_t numpe_after_rc, rddsm_after_rc, rddsdr_after_rc, rddst_after_rc;
	int     rc;

	probe_report_panel_state("vrx2", dsi);

	/* Step 1 (after an implicit drop to command mode -- see the banner above). */
	rc = display_blanking_on(disp);
	printk("vrx2: pre-baseline blanking_on rc=%d\n", rc);
	vrx2_snapshot(dsi,
	              "before",
	              &numpe_before,
	              &rddsm_before,
	              &rddsdr_before,
	              rddst_before,
	              &numpe_before_rc,
	              &rddsm_before_rc,
	              &rddsdr_before_rc,
	              &rddst_before_rc);

	/* Step 2: start video. */
	rc = display_blanking_off(disp);
	{
		uint32_t phy = sys_read32(DSI_PHY_STATUS_ADDR);

		printk("vrx2: blanking_off rc=%d cdc-glb=0x%08x mode=0x%08x lpclk=0x%08x "
		       "phy=0x%08x stopclk=%d\n",
		       rc,
		       sys_read32(CDC_GLB_CTRL_ADDR),
		       sys_read32(DSI_MODE_CFG_ADDR),
		       sys_read32(DSI_LPCLK_CTRL_ADDR),
		       phy,
		       (int)((phy & DSI_PHY_STATUS_STOPSTATECLKLANE) != 0));
	}

	/* Step 3: hold 5 s of live scanout; host TX-side counters only. */
	k_msleep(5000);
	printk("vrx2: video 5s int0=0x%08x int1=0x%08x\n",
	       sys_read32(DSI_INT_ST0_ADDR),
	       sys_read32(DSI_INT_ST1_ADDR));

	/* Step 4: return to command mode; the same snapshot proves the mode changed. */
	rc = display_blanking_on(disp);
	{
		uint32_t phy = sys_read32(DSI_PHY_STATUS_ADDR);

		printk("vrx2: blanking_on rc=%d cdc-glb=0x%08x mode=0x%08x lpclk=0x%08x "
		       "phy=0x%08x stopclk=%d\n",
		       rc,
		       sys_read32(CDC_GLB_CTRL_ADDR),
		       sys_read32(DSI_MODE_CFG_ADDR),
		       sys_read32(DSI_LPCLK_CTRL_ADDR),
		       phy,
		       (int)((phy & DSI_PHY_STATUS_STOPSTATECLKLANE) != 0));
	}

	if (rc < 0) {
		printk("vrx2: SKIPPED -- could not re-enter command mode, no read attempted\n");
	} else {
		/* Step 5: re-read; decode RDDSM D0 only if that read succeeded. */
		vrx2_snapshot(dsi,
		              "after ",
		              &numpe_after,
		              &rddsm_after,
		              &rddsdr_after,
		              rddst_after,
		              &numpe_after_rc,
		              &rddsm_after_rc,
		              &rddsdr_after_rc,
		              &rddst_after_rc);

		int  rddsm_d0 = (rddsm_after_rc >= 0) ? (int)(rddsm_after & BIT(0)) : -1;
		bool reads_ok = (numpe_after_rc >= 0) && (rddsm_after_rc >= 0) && (rddsdr_after_rc >= 0) &&
		                (rddst_after_rc >= 0);

		/* Step 6: verdict. */
		printk("vrx2: VERDICT numpe_before=0x%02x numpe_after=0x%02x rddsm_d0=%d "
		       "reads_ok=%d\n",
		       numpe_before,
		       numpe_after,
		       rddsm_d0,
		       (int)reads_ok);
	}

	/* Step 7: resume video, matching the mode main() left the chain in. */
	rc = display_blanking_off(disp);
	printk("vrx2: resume blanking_off rc=%d\n", rc);
}

/*
 * BENCH DIAGNOSTIC -- SETDISP (B2h) SW free-running BIST (#2199), run now that
 * a gated write is proven to land (probe_setextc_gate_v2()).  DISP_BIST_EN
 * (bit 3 of parameter 11, HX8394 datasheet Sec5.19.3 p.198, "Set '1' enable SW
 * free running mode") switches the panel to its OWN internal pattern
 * generator, making the incoming DSI video irrelevant -- a clean pattern on
 * glass here proves the glass/gate/panel-controller side works independent of
 * what the host is sending, narrowing a still-black panel to the DPI/DSI-RX
 * chain instead of the panel itself.
 *
 * Parameters 1-6 are the init's own SETDISP values, byte for byte; 7-10 are
 * "-" reserved in the command's own map and go out as 00; parameter 11 is
 * 0xC0 | BIT(3).  BIST OFF restores the plain init form (its own 6-parameter
 * form, byte for byte).
 *
 * DISP_BIST_EN needs no incoming video at all -- that is the entire point of
 * the test -- so this runs in COMMAND MODE, after the framebuffer is written
 * but BEFORE main()'s first display_blanking_off() call (see the call site):
 * wedge: (probe_read_wedge()'s own banner) shows that once video mode has
 * run, the DCS read path can no longer be trusted, so a flat BIST measured
 * after video has started is indistinguishable from a flat BIST on a
 * blanked panel.  Running here, before any video-mode entry in main(), is
 * what makes the RDDPM checks below mean anything.
 *
 * Self-certifying: bist2_report_rddpm() reads RDDPM at entry, immediately
 * after the BIST-ON write, and again after the restore, so the log proves
 * the panel stayed awake across the whole measurement rather than only at
 * its start.  A failed read is reported as unverified -- never decoded from
 * the 0xAA sentinel.
 */
static bool bist2_report_rddpm(const struct device *dsi, const char *tag)
{
	uint8_t  rddpm = 0xAAU;
	uint32_t int0  = 0;
	uint32_t int1  = 0;
	ssize_t  rc    = dcs_read_classified(dsi, 0x0AU, &rddpm, 1U, &int0, &int1);

	if (rc < 1) {
		printk("bist2: %s RDDPM read FAILED rc=%d -- panel state UNVERIFIED\n", tag, (int)rc);
		return false;
	}

	int  sleep_out  = (int)((rddpm & BIT(4)) != 0);
	int  display_on = (int)((rddpm & BIT(2)) != 0);
	bool awake      = sleep_out && display_on;

	printk(
	    "bist2: %s RDDPM=0x%02x sleep_out=%d display_on=%d\n", tag, rddpm, sleep_out, display_on);
	if (awake) {
		printk("bist2: panel CONFIRMED awake at BIST time (RDDPM=0x%02x)\n", rddpm);
	}
	return awake;
}

static void probe_bist_v2(const struct device *dsi)
{
	static const uint8_t extc_cmd[] = { 0xB9U, 0xFFU, 0x83U, 0x94U };
	static const uint8_t bist_on[]  = { 0x00U, 0x80U, 0x64U, 0x0CU, 0x0DU, 0x2FU,
		                                0x00U, 0x00U, 0x00U, 0x00U, 0xC8U };
	static const uint8_t bist_off[] = { 0x00U, 0x80U, 0x64U, 0x0CU, 0x0DU, 0x2FU };
	ssize_t              rc;

	bist2_report_rddpm(dsi, "entry");

	{
		struct mipi_dsi_msg w = {
			.type   = MIPI_DSI_GENERIC_LONG_WRITE,
			.flags  = MIPI_DSI_MSG_USE_LPM,
			.tx_buf = extc_cmd,
			.tx_len = sizeof(extc_cmd),
		};
		rc = mipi_dsi_transfer(dsi, 0, &w);
		printk("bist2: SETEXTC (B9h FF 83 94) rc=%d\n", (int)rc);
	}

	{
		struct mipi_dsi_msg w = {
			.type   = MIPI_DSI_DCS_LONG_WRITE,
			.flags  = MIPI_DSI_MSG_USE_LPM,
			.cmd    = 0xB2U,
			.tx_buf = bist_on,
			.tx_len = sizeof(bist_on),
		};
		rc = mipi_dsi_transfer(dsi, 0, &w);
		printk("bist2: SETDISP BIST-ON B2 00 80 64 0C 0D 2F 00 00 00 00 C8 rc=%d\n", (int)rc);
	}

	bist2_report_rddpm(dsi, "after-bist-on");

	printk("bist2: BIST ON int0=0x%08x int1=0x%08x cdc-glb=0x%08x -- CAPTURE NOW\n",
	       sys_read32(DSI_INT_ST0_ADDR),
	       sys_read32(DSI_INT_ST1_ADDR),
	       sys_read32(CDC_GLB_CTRL_ADDR));
	k_msleep(10000);

	{
		struct mipi_dsi_msg w = {
			.type   = MIPI_DSI_GENERIC_LONG_WRITE,
			.flags  = MIPI_DSI_MSG_USE_LPM,
			.tx_buf = extc_cmd,
			.tx_len = sizeof(extc_cmd),
		};
		rc = mipi_dsi_transfer(dsi, 0, &w);
		printk("bist2: SETEXTC (B9h FF 83 94) rc=%d\n", (int)rc);
	}

	{
		struct mipi_dsi_msg w = {
			.type   = MIPI_DSI_DCS_LONG_WRITE,
			.flags  = MIPI_DSI_MSG_USE_LPM,
			.cmd    = 0xB2U,
			.tx_buf = bist_off,
			.tx_len = sizeof(bist_off),
		};
		rc = mipi_dsi_transfer(dsi, 0, &w);
		printk("bist2: SETDISP restore B2 00 80 64 0C 0D 2F rc=%d\n", (int)rc);
	}

	bist2_report_rddpm(dsi, "after-restore");

	printk("bist2: BIST OFF (restored) int0=0x%08x int1=0x%08x cdc-glb=0x%08x -- CAPTURE NOW\n",
	       sys_read32(DSI_INT_ST0_ADDR),
	       sys_read32(DSI_INT_ST1_ADDR),
	       sys_read32(CDC_GLB_CTRL_ADDR));
	k_msleep(10000);
}

/*
 * DSI_CMD_PKT_STATUS bit positions (zephyr/drivers/mipi_dsi/dsi_dw.h) -- defined
 * locally so this probe file does not include the driver-private header.
 */
#define WEDGE_GEN_RD_CMD_BUSY   BIT(6)
#define WEDGE_GEN_CMD_FULL      BIT(1)
#define WEDGE_GEN_PLD_W_FULL    BIT(3)
#define WEDGE_GEN_BUFF_CMD_FULL BIT(17)
#define WEDGE_GEN_BUFF_PLD_FULL BIT(19)

static void wedge_dump_pkt_status(const char *arm, const char *point)
{
	uint32_t pkt = sys_read32(DSI_CMD_PKT_STATUS_ADDR);

	printk("wedge: %s %s pkt=0x%08x rdbusy=%d cmdfull=%d pldwfull=%d buffcmdfull=%d "
	       "buffpldfull=%d int0=0x%08x int1=0x%08x\n",
	       arm,
	       point,
	       pkt,
	       (int)((pkt & WEDGE_GEN_RD_CMD_BUSY) != 0),
	       (int)((pkt & WEDGE_GEN_CMD_FULL) != 0),
	       (int)((pkt & WEDGE_GEN_PLD_W_FULL) != 0),
	       (int)((pkt & WEDGE_GEN_BUFF_CMD_FULL) != 0),
	       (int)((pkt & WEDGE_GEN_BUFF_PLD_FULL) != 0),
	       sys_read32(DSI_INT_ST0_ADDR),
	       sys_read32(DSI_INT_ST1_ADDR));
}

/*
 * BENCH DIAGNOSTIC -- probe_read_wedge() (#2199).
 *
 * The observation: DCS reads work perfectly in command mode until the FIRST
 * read attempted while in video mode times out (rc=-116); every read after
 * that fails rc=-5, including reads back in command mode, with the clock lane
 * confirmed stopped (stopclk=1) and no error interrupt raised at all.  Two
 * candidate causes this probe separates:
 *
 *   (A) merely ENTERING video mode breaks subsequent reads, or
 *   (B) a FAILED read attempted WHILE in video mode wedges the host's generic
 *       interface -- consistent with the earlier precedent where a probe left
 *       an orphaned entry in the generic FIFO and every later transaction
 *       failed until a power cycle.
 *
 * Three arms, run with a virgin read path -- this MUST run before anything
 * else attempts a read in video mode (see the call site in main()), or arm 1's
 * own baseline is already contaminated and the whole probe is void.
 *
 *   1) baseline: read RDDPM in command mode.
 *   2) enter video, hold 2 s, NO read while there, leave video, read RDDPM
 *      again.  Success refutes (A).
 *   3) enter video, hold 2 s, attempt (and expect to fail) a read WHILE still
 *      in video, leave video, read RDDPM again.  If arm 2 succeeded and this
 *      final read fails, (B) is confirmed.
 *
 * GEN_RD_CMD_BUSY stuck SET after a failed read is the specific wedge
 * signature, so pkt status is dumped immediately before/after every read and
 * every blanking_off()/blanking_on() call, alongside int0/int1.
 */
static void probe_read_wedge(const struct device *dsi, const struct device *disp)
{
	uint8_t pm;
	ssize_t arm1_rc;
	ssize_t arm2_rc;
	ssize_t arm3_mid_rc;
	ssize_t arm3_rc;

	probe_report_panel_state("wedge", dsi);

#define WEDGE_READ(tag, outvar) \
	do { \
		pm = 0xAAU; \
		wedge_dump_pkt_status((tag), "pre-read"); \
		(outvar) = dcs_read_lpm(dsi, 0x0AU, &pm, 1U); \
		wedge_dump_pkt_status((tag), "post-read"); \
		printk("wedge: %s RDDPM rc=%d data=0x%02x%s\n", \
		       (tag), \
		       (int)(outvar), \
		       pm, \
		       ((outvar) < 0) ? "  <- READ FAILED, sentinel not data" : ""); \
	} while (0)

#define WEDGE_BLANKING_OFF(tag) \
	do { \
		int brc; \
		wedge_dump_pkt_status((tag), "pre-blanking_off"); \
		brc = display_blanking_off(disp); \
		wedge_dump_pkt_status((tag), "post-blanking_off"); \
		printk("wedge: %s blanking_off rc=%d\n", (tag), brc); \
	} while (0)

#define WEDGE_BLANKING_ON(tag) \
	do { \
		int brc; \
		wedge_dump_pkt_status((tag), "pre-blanking_on"); \
		brc = display_blanking_on(disp); \
		wedge_dump_pkt_status((tag), "post-blanking_on"); \
		printk("wedge: %s blanking_on rc=%d\n", (tag), brc); \
	} while (0)

	/* --- Arm 1: baseline, command mode. --- */
	WEDGE_READ("arm1", arm1_rc);

	/* --- Arm 2: enter video, NO read while there, leave, re-read. --- */
	WEDGE_BLANKING_OFF("arm2");
	k_msleep(2000);
	WEDGE_BLANKING_ON("arm2");
	WEDGE_READ("arm2", arm2_rc);

	/* --- Arm 3: enter video, DO read while there (expected to fail), leave, re-read. --- */
	WEDGE_BLANKING_OFF("arm3");
	k_msleep(2000);
	WEDGE_READ("arm3-in-video", arm3_mid_rc);
	WEDGE_BLANKING_ON("arm3");
	WEDGE_READ("arm3", arm3_rc);

#undef WEDGE_BLANKING_ON
#undef WEDGE_BLANKING_OFF
#undef WEDGE_READ

	bool arm1_ok       = arm1_rc >= 0;
	bool arm2_ok       = arm2_rc >= 0;
	bool arm3_final_ok = arm3_rc >= 0;

	printk("wedge: VERDICT raw arm1_rc=%d arm2_rc=%d arm3_in_video_rc=%d arm3_final_rc=%d\n",
	       (int)arm1_rc,
	       (int)arm2_rc,
	       (int)arm3_mid_rc,
	       (int)arm3_rc);

	if (arm1_ok && arm2_ok && !arm3_final_ok) {
		printk("wedge: VERDICT (B) confirmed -- entering video alone is fine (arm2 "
		       "read succeeded), the FAILED in-video read (arm3-in-video) is what "
		       "wedges the interface (arm3 final read fails)\n");
	} else if (arm1_ok && !arm2_ok) {
		printk("wedge: VERDICT (A) supported -- merely entering video mode already "
		       "breaks the next read, with no read attempted while in video\n");
	} else if (arm1_ok && arm2_ok && arm3_final_ok) {
		printk("wedge: VERDICT neither (A) nor (B) -- both video-mode arms recovered "
		       "a working read afterward\n");
	} else {
		printk("wedge: VERDICT indeterminate -- see raw rc values above\n");
	}
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

#ifdef TOUCH_I2C_READY
	/*
	 * Step 1b (BENCH DIAGNOSTIC): the panel flex's touch bus, before any DSI
	 * read, so its verdict is independent of the DSI link's state.
	 */
	if (exp_ok) {
		scan_i2c1_touch(exp);
	}
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
		/*
		 * Runs before every other probe in this block: probe_gated_read_path()
		 * and later probes send SETEXTC themselves, which would contaminate
		 * this test's negative control.
		 */
		probe_setextc_gate_v2(dsi);
		panel_read_ok = probe_panel_reads(dsi);
		dump_dsi_status("after-read");

		/*
		 * BENCH DIAGNOSTIC (#2199): probe_ta_timing() must run here -- in
		 * command mode, after probe_setextc_gate_v2() (see its own banner
		 * for why that ordering), and before anything below enters video
		 * mode or calls probe_read_wedge(), which would contaminate the
		 * read-success measurement this probe depends on.
		 */
		probe_ta_timing(dsi);
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
		fill_row_green();

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
			printk("display_write: full 720x1280 green frame OK (%u bytes/pixel)\n",
			       (unsigned int)PANEL_BYTES_PER_PIXEL);

			/*
			 * BENCH DIAGNOSTIC (#2199): probe_bist_v2() runs HERE -- the
			 * framebuffer is written, panel_wake_after_resx() already ran
			 * (inside probe_setextc_gate_v2() above), and the DSI host is
			 * still in command mode, so DCS reads still work.  DISP_BIST_EN
			 * needs no video mode at all (see the probe's own banner), and
			 * this must run BEFORE probe_read_wedge() below deliberately
			 * touches video mode for the first time: per that probe's own
			 * finding, a read attempted while in video mode can wedge the
			 * read path even back in command mode, which would make bist2's
			 * own RDDPM self-certification unreliable if it ran any later.
			 */
			if (dsi_ok) {
				probe_bist_v2(dsi);
			}

			/*
			 * BENCH DIAGNOSTIC (#2199): probe_read_wedge() runs next, with a
			 * virgin read path, before anything below attempts a read in video
			 * mode -- see its banner.  The RDDPM/vid diagnostic read further
			 * down now runs after it returns, on purpose: it must not go
			 * first, or it wedges the interface before the experiment starts.
			 */
			if (dsi_ok) {
				probe_read_wedge(dsi, disp);
			}

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

				/*
				 * BENCH DIAGNOSTIC (#2199): panel-side DSI-RX health, now
				 * that the framebuffer is written and video is live.  See
				 * probe_video_rx_errors_v2() for why this runs here; its
				 * sibling probe_bist_v2() now runs earlier, in command mode
				 * before video ever started (see main()'s call site right
				 * before probe_read_wedge()) -- see that probe's own banner.
				 */
				probe_video_rx_errors_v2(dsi, disp);
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
		       "+ cdc200 display ready, full-screen green frame written and "
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
