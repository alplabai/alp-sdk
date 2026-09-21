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
 * E1M-AEN803 2026W36-0009 (#2199) -- whole-panel DCS silence on some cold cycles, and
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
	uint8_t              id[4] = { 0 };
	int                  lo_rc, hi_rc, lvl_lo, lvl_hi;

	/*
	 * This used to be a single gpio_pin_configure(..., GPIO_OUTPUT_HIGH) and
	 * a print claiming the reset was "released".  It was not a reset at all:
	 * the expander's output register already reads bit 3 high at POR
	 * (`lcd-exp regs[por]: out=fd`), so configuring P3 as an output-high
	 * produces NO falling edge and the GT911 is never reset.  Every run of
	 * this bring-up has reported `touch : NO ANSWER` off the back of that,
	 * and it was being read as evidence about the panel-side flex.
	 *
	 * Drive a real low->high pulse instead, and read the expander's INPUT
	 * register at both ends so the log shows the pin actually moved rather
	 * than only that the write returned 0.
	 *
	 * KNOWN LIMIT, and it may make this scan inconclusive either way: the
	 * GT911 latches its I2C address during reset release from the INT pin --
	 * INT low selects 0x5D, INT high selects 0x14, which is exactly the pair
	 * scanned below.  CTP_INT_L is J6 pin 29 and the shield routes it to
	 * CC3501E GPIO_15, NOT an Alif pad, so this app cannot drive it.  With
	 * INT undriven the latched address is indeterminate, so a silent bus
	 * after this pulse still does not prove the part is absent.
	 */
	lo_rc  = gpio_pin_configure(exp, CTP_RST_EXP_PIN, GPIO_OUTPUT_LOW);
	lvl_lo = gpio_pin_get(exp, CTP_RST_EXP_PIN);
	printk("ctp-rst : P3 driven LOW rc=%d readback=%d, holding 20 ms\n", lo_rc, lvl_lo);
	k_msleep(20);

	hi_rc  = gpio_pin_set(exp, CTP_RST_EXP_PIN, 1);
	lvl_hi = gpio_pin_get(exp, CTP_RST_EXP_PIN);
	printk("ctp-rst : P3 released HIGH rc=%d readback=%d, settling %u ms "
	       "(INT undriven -- GT911 address latch is indeterminate)\n",
	       hi_rc,
	       lvl_hi,
	       CTP_RESET_SETTLE_MS);
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
#define PANEL_FMT_IS_RGB888 DT_ENUM_HAS_VALUE(DT_NODELABEL(cdc200), pixel_fmt_l1, rgb_888)
#define PANEL_FMT_IS_RGB565 DT_ENUM_HAS_VALUE(DT_NODELABEL(cdc200), pixel_fmt_l1, rgb_565)

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
 * BENCH DIAGNOSTIC -- probe_reinit_180ms() (#2199), prefix `reinit:`.
 *
 * Zephyr's hx8394_init() releases RESX and waits only 50 ms
 * (display_hx8394.c:584-589) before sending the manufacturer init --
 * SETPOWER/SETGIP0-2/SETVCOM among it.  The mainline Linux driver for this
 * exact panel waits 180 ms (panel-himax-hx8394.c:577-579), and the
 * datasheet documents both: Figure 5.28 says >50 ms after hardware reset,
 * Figure 5.27 says 180 ms.  If the panel's internal power-on has not
 * finished when the gated registers arrive, the digital core still latches
 * them -- every command acknowledges, every ID reads back correctly -- but
 * the analog blocks never take the configuration.  That matches everything
 * measured on this board, and a marginal 50 ms would also be intermittent,
 * matching panel init failing roughly one cold boot in three.
 *
 * This probe hardware-resets the panel with the VENDOR 180 ms dwell instead
 * of the driver's 50 ms, then re-sends the driver's own init byte strings,
 * in the driver's own order -- transcribed from display_hx8394.c (read, not
 * retyped from memory) -- through reinit_tx(), which mirrors that file's own
 * hx8394_mipi_tx() write-type dispatch, so the on-wire framing matches the
 * driver exactly, not just the payload bytes.
 *
 * Runs in COMMAND MODE, before any video-mode entry: called immediately
 * after probe_setextc_gate_v2() (see the call site in main()), whose own
 * final RESX pulse already left the panel awake via panel_wake_after_resx()
 * -- so this probe starts from a known state.  Everything that runs after
 * it (probe_ta_timing(), probe_bist_v2(), probe_frm_vcom_sweep()) now
 * measures a panel brought up with the vendor dwell instead of the driver's
 * 50 ms floor: if any of them move the glass afterward, the dwell was the
 * bug.
 *
 * RESX pulsing goes through exp_set_resx() (see its own banner) rather than
 * an open-coded expander write, so P0 (panel supply enable) and P3 (touch
 * reset) are never disturbed.  Never sends SETOTP (BBh) or SETID (C3h) --
 * both burn OTP irreversibly and are never used anywhere in this app; C3
 * appears only as payload data inside SETDDB (C4h) elsewhere in this file,
 * never as a command byte.  Every read goes through dcs_read_classified();
 * a failed read is reported as UNKNOWN, never decoded from the 0xAA
 * sentinel.
 */

/*
 * Mirrors display_hx8394.c's own hx8394_mipi_tx(): every write below is a
 * GENERIC_* MIPI-DSI message in LPM, dispatched by payload length exactly as
 * the driver dispatches it, so the on-wire framing matches the driver's real
 * init rather than an approximation of it.
 */
static ssize_t reinit_tx(const struct device *dsi, const uint8_t *buf, size_t len)
{
	struct mipi_dsi_msg msg = {
		.tx_buf = buf,
		.tx_len = len,
		.flags  = MIPI_DSI_MSG_USE_LPM,
	};

	switch (len) {
	case 0U:
		msg.type = MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM;
		break;
	case 1U:
		msg.type = MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM;
		break;
	case 2U:
		msg.type = MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM;
		break;
	default:
		msg.type = MIPI_DSI_GENERIC_LONG_WRITE;
		break;
	}

	return mipi_dsi_transfer(dsi, 0, &msg);
}

static void probe_reinit_180ms(const struct device *dsi)
{
	/*
	 * Byte strings transcribed directly from display_hx8394.c's own
	 * enable_extension/setmipi/address_config/power_config/... arrays
	 * (read from that file, not retyped from memory) -- see it for the
	 * #define expansion behind each OR'd byte.  setmipi[1] bakes in
	 * num_of_lanes=2 (this shield's DSISETUP0), confirmed by the driver's
	 * own on-wire bytes reported in probe_ta_timing()'s banner:
	 * "BA 61 03 68 6B B2 C0".
	 */
	static const uint8_t extc[]     = { 0xB9U, 0xFFU, 0x83U, 0x94U };
	static const uint8_t setmipi[]  = { 0xBAU, 0x61U, 0x03U, 0x68U, 0x6BU, 0xB2U, 0xC0U };
	static const uint8_t madctl[]   = { 0x36U, 0x02U };
	static const uint8_t setpower[] = {
		0xB1U, 0x48U, 0x12U, 0x72U, 0x09U, 0x32U, 0x54U, 0x71U, 0x71U, 0x57U, 0x47U,
	};
	static const uint8_t setdisp[] = { 0xB2U, 0x00U, 0x80U, 0x64U, 0x0CU, 0x0DU, 0x2FU };
	static const uint8_t setcyc[]  = {
		0xB4U, 0x73U, 0x74U, 0x73U, 0x74U, 0x73U, 0x74U, 0x01U, 0x0CU, 0x86U, 0x75U,
		0x00U, 0x3FU, 0x73U, 0x74U, 0x73U, 0x74U, 0x73U, 0x74U, 0x01U, 0x0CU, 0x86U,
	};
	static const uint8_t setgip0[] = {
		0xD3U, 0x00U, 0x00U, 0x07U, 0x07U, 0x40U, 0x07U, 0x0CU, 0x00U, 0x08U, 0x10U, 0x08U,
		0x00U, 0x08U, 0x54U, 0x15U, 0x0AU, 0x05U, 0x0AU, 0x02U, 0x15U, 0x06U, 0x05U, 0x06U,
		0x47U, 0x44U, 0x0AU, 0x0AU, 0x4BU, 0x10U, 0x07U, 0x07U, 0x0CU, 0x40U,
	};
	static const uint8_t setgip1[] = {
		0xD5U, 0x1CU, 0x1CU, 0x1DU, 0x1DU, 0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U,
		0x07U, 0x08U, 0x09U, 0x0AU, 0x0BU, 0x24U, 0x25U, 0x18U, 0x18U, 0x26U, 0x27U, 0x18U,
		0x18U, 0x18U, 0x18U, 0x18U, 0x18U, 0x18U, 0x18U, 0x18U, 0x18U, 0x18U, 0x18U, 0x18U,
		0x18U, 0x18U, 0x18U, 0x20U, 0x21U, 0x18U, 0x18U, 0x18U, 0x18U,
	};
	static const uint8_t setgip2[] = {
		0xD6U, 0x1CU, 0x1CU, 0x1DU, 0x1DU, 0x07U, 0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U,
		0x00U, 0x0BU, 0x0AU, 0x09U, 0x08U, 0x21U, 0x20U, 0x18U, 0x18U, 0x27U, 0x26U, 0x18U,
		0x18U, 0x18U, 0x18U, 0x18U, 0x18U, 0x18U, 0x18U, 0x18U, 0x18U, 0x18U, 0x18U, 0x18U,
		0x18U, 0x18U, 0x18U, 0x25U, 0x24U, 0x18U, 0x18U, 0x18U, 0x18U,
	};
	static const uint8_t setvcom[]  = { 0xB6U, 0x92U, 0x92U };
	static const uint8_t setgamma[] = {
		0xE0U, 0x00U, 0x0AU, 0x15U, 0x1BU, 0x1EU, 0x21U, 0x24U, 0x22U, 0x47U, 0x56U, 0x65U,
		0x66U, 0x6EU, 0x82U, 0x88U, 0x8BU, 0x9AU, 0x9DU, 0x98U, 0xA8U, 0xB9U, 0x5DU, 0x5CU,
		0x61U, 0x66U, 0x6AU, 0x6FU, 0x7FU, 0x7FU, 0x00U, 0x0AU, 0x15U, 0x1BU, 0x1EU, 0x21U,
		0x24U, 0x22U, 0x47U, 0x56U, 0x65U, 0x65U, 0x6EU, 0x81U, 0x87U, 0x8BU, 0x98U, 0x9DU,
		0x99U, 0xA8U, 0xBAU, 0x5DU, 0x5DU, 0x62U, 0x67U, 0x6BU, 0x72U, 0x7FU, 0x7FU,
	};
	static const uint8_t cmd_c0[]   = { 0xC0U, 0x1FU, 0x31U };
	static const uint8_t setpanel[] = { 0xCCU, 0x03U };
	static const uint8_t cmd_d4[]   = { 0xD4U, 0x02U };
	static const uint8_t bank_02[]  = { 0xBDU, 0x02U };
	static const uint8_t bank_d8[]  = {
		0xD8U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
	};
	static const uint8_t bank_00a[] = { 0xBDU, 0x00U };
	static const uint8_t bank_01[]  = { 0xBDU, 0x01U };
	static const uint8_t bank_b1[]  = { 0xB1U, 0x00U };
	static const uint8_t bank_00b[] = { 0xBDU, 0x00U };
	static const uint8_t cmd_bf[]   = {
		0xBFU, 0x40U, 0x81U, 0x50U, 0x00U, 0x1AU, 0xFCU, 0x01U,
	};
	static const uint8_t cmd_c6[]  = { 0xC6U, 0xEDU };
	static const uint8_t tear_on[] = { 0x35U, 0x00U };

	uint8_t  rddpm = 0xAAU;
	uint8_t  rddst[4];
	uint8_t  rddid[3];
	uint32_t int0 = 0;
	uint32_t int1 = 0;
	ssize_t  rc;
	bool     rddpm_ok;

	/* 1. Entry state. */
	rc = dcs_read_classified(dsi, 0x0AU, &rddpm, 1U, &int0, &int1);
	if (rc < 0) {
		printk("reinit: entry RDDPM read FAILED rc=%d -- state UNKNOWN\n", (int)rc);
	} else {
		printk("reinit: entry RDDPM=0x%02x sleep_out=%d display_on=%d\n",
		       rddpm,
		       (int)((rddpm & BIT(4)) != 0),
		       (int)((rddpm & BIT(2)) != 0));
	}

	memset(rddst, 0xAAU, sizeof(rddst));
	rc = dcs_read_classified(dsi, 0x09U, rddst, sizeof(rddst), &int0, &int1);
	if (rc < 0) {
		printk("reinit: entry RDDST read FAILED rc=%d -- state UNKNOWN\n", (int)rc);
	} else {
		printk("reinit: entry RDDST=%02x %02x %02x %02x\n", rddst[0], rddst[1], rddst[2], rddst[3]);
	}

	/* 2. Hardware reset with the VENDOR 180 ms dwell, not the driver's 50 ms. */
	if (!exp_set_resx(0)) {
		printk("reinit: ABORT -- RESX-low failed, refusing to reinit a panel whose "
		       "reset state is unknown\n");
		return;
	}
	k_msleep(20);
	if (!exp_set_resx(1)) {
		printk("reinit: ABORT -- RESX-high failed\n");
		return;
	}
	printk("reinit: RESX low 20ms then released high, now dwelling 180ms "
	       "(datasheet Fig.5.27, vendor value -- the driver's own Fig.5.28 floor is 50ms)\n");
	k_sleep(K_MSEC(180));

	/* 3. Re-send the full manufacturer init, driver's own order and bytes. */
#define REINIT_SEND(arr) \
	do { \
		ssize_t wrc = reinit_tx(dsi, (arr), sizeof(arr)); \
		printk("reinit: write cmd=0x%02x len=%u rc=%d\n", \
		       (arr)[0], \
		       (unsigned int)sizeof(arr), \
		       (int)wrc); \
	} while (0)

	REINIT_SEND(extc);
	REINIT_SEND(setmipi);
	REINIT_SEND(madctl);
	REINIT_SEND(setpower);
	REINIT_SEND(setdisp);
	REINIT_SEND(setcyc);
	REINIT_SEND(setgip0);
	REINIT_SEND(setgip1);
	REINIT_SEND(setgip2);
	/* Driver's own comment: without this pause the panel stops responding
	 * to further commands; reason undocumented in the datasheet. */
	k_msleep(1);
	REINIT_SEND(setvcom);
	REINIT_SEND(setgamma);
	REINIT_SEND(cmd_c0);
	REINIT_SEND(setpanel);
	REINIT_SEND(cmd_d4);
	REINIT_SEND(bank_02);
	REINIT_SEND(bank_d8);
	REINIT_SEND(bank_00a);
	REINIT_SEND(bank_01);
	REINIT_SEND(bank_b1);
	REINIT_SEND(bank_00b);
	REINIT_SEND(cmd_bf);
	REINIT_SEND(cmd_c6);
	REINIT_SEND(tear_on);

#undef REINIT_SEND

	/* 4. Exit sleep (vendor 120ms dwell), then display on. */
	{
		struct mipi_dsi_msg slpout_w = {
			.type  = MIPI_DSI_DCS_SHORT_WRITE,
			.flags = MIPI_DSI_MSG_USE_LPM,
			.cmd   = 0x11U, /* EXIT_SLEEP_MODE */
		};
		ssize_t slpout_rc = mipi_dsi_transfer(dsi, 0, &slpout_w);

		printk("reinit: EXIT_SLEEP_MODE (0x11) rc=%d\n", (int)slpout_rc);
		k_sleep(K_MSEC(120));

		struct mipi_dsi_msg dispon_w = {
			.type  = MIPI_DSI_DCS_SHORT_WRITE,
			.flags = MIPI_DSI_MSG_USE_LPM,
			.cmd   = 0x29U, /* SET_DISPLAY_ON */
		};
		ssize_t dispon_rc = mipi_dsi_transfer(dsi, 0, &dispon_w);

		printk("reinit: SET_DISPLAY_ON (0x29) rc=%d\n", (int)dispon_rc);
	}

	/* 5. Resulting state. */
	rc       = dcs_read_classified(dsi, 0x0AU, &rddpm, 1U, &int0, &int1);
	rddpm_ok = rc >= 0;
	if (!rddpm_ok) {
		printk("reinit: after RDDPM read FAILED rc=%d -- state UNKNOWN\n", (int)rc);
	} else {
		printk("reinit: after RDDPM=0x%02x sleep_out=%d display_on=%d\n",
		       rddpm,
		       (int)((rddpm & BIT(4)) != 0),
		       (int)((rddpm & BIT(2)) != 0));
	}

	memset(rddst, 0xAAU, sizeof(rddst));
	rc = dcs_read_classified(dsi, 0x09U, rddst, sizeof(rddst), &int0, &int1);
	if (rc < 0) {
		printk("reinit: after RDDST read FAILED rc=%d -- state UNKNOWN\n", (int)rc);
	} else {
		printk("reinit: after RDDST=%02x %02x %02x %02x\n", rddst[0], rddst[1], rddst[2], rddst[3]);
	}

	memset(rddid, 0xAAU, sizeof(rddid));
	rc = dcs_read_classified(dsi, 0x04U, rddid, sizeof(rddid), &int0, &int1);
	if (rc < 0) {
		printk("reinit: after RDDID read FAILED rc=%d -- state UNKNOWN\n", (int)rc);
	} else {
		printk("reinit: after RDDID=%02x %02x %02x\n", rddid[0], rddid[1], rddid[2]);
	}

	int sleep_out  = rddpm_ok && ((rddpm & BIT(4)) != 0);
	int display_on = rddpm_ok && ((rddpm & BIT(2)) != 0);

	if (!rddpm_ok) {
		printk("reinit: panel wake state UNKNOWN -- RDDPM read failed\n");
	} else if (sleep_out && display_on) {
		printk("reinit: panel came back sleep_out=1 display_on=1 after the 180ms reinit\n");
	} else {
		printk("reinit: panel did NOT come back sleep_out=1 display_on=1 "
		       "(sleep_out=%d display_on=%d)\n",
		       sleep_out,
		       display_on);
	}

	printk("reinit: END dwell=180ms full-init-resent\n");
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
 * BENCH DIAGNOSTIC -- probe_frm_vcom_sweep() (#2199), prefix `frmv:`.
 *
 * probe_bist_v2()'s Free Running Mode burn-in generator (DISP_BIST_EN, SETDISP
 * B2h parameter 11 bit 3, datasheet Sec5.17 p.113) was measured on a
 * confirmed-awake panel and produced NO change on the glass -- smaller than
 * camera noise against a 117-count backlight reference.  That result is
 * currently being read as "the glass cannot be driven".  It does not yet
 * establish that, because VCOM has never been tested across its real range.
 * If the common electrode sits where the liquid crystal sees no net field,
 * the glass shows nothing no matter how perfectly the source and gate
 * drivers work -- including under FRM.  An earlier sweep wrote SETVCOM (B6h)
 * with only TWO parameters, leaving VCMC[8] clear, so it covered only
 * -0.30V to -2.85V and never reached the codes that matter.
 *
 * This probe drives FRM and sweeps VCOM underneath it.  If any VCOM value
 * makes the FRM pattern appear, VCOM was the blocker and the panel is fine.
 *
 * Runs in COMMAND MODE, before any video-mode entry -- reads still work
 * there, so wakefulness is verifiable throughout.  Called immediately after
 * probe_bist_v2() returns (see the call site in main()), for the same reason
 * that probe runs where it does: probe_read_wedge() (run right after this
 * one) shows a read attempted once video mode has been touched can wedge the
 * read path even back in command mode, so this sweep's own RDDPM
 * self-certification would be unreliable if it ran any later.
 *
 * SETVCOM (B6h) 3-parameter form: parameter 1 = VCMC_F[7:0], parameter 2 =
 * VCMC_B[7:0], parameter 3 = VCOM_TIMES[7:5] | VCMC_B[8]<<1 | VCMC_F[8].  The
 * 0xE0 VCOM_TIMES base (bits 7:5, the OTP programmed-times counter,
 * documented default 111) is preserved on every write -- it is read-only
 * state from OTP, not a value this probe owns.
 *
 * All B6h writes here are volatile register writes, reversed by RESX or a
 * power cycle -- never SETOTP (BBh), the irreversible OTP burn command, which
 * is never sent by this app.  SETID (C3h) is likewise never sent.
 */
#define FRMV_STEP_COUNT 7

struct frmv_step {
	uint16_t    vcmc;
	const char *meaning;
};

static const struct frmv_step frmv_steps[FRMV_STEP_COUNT] = {
	{ 0x092U, "init value, control" },
	{ 0x000U, "-0.30V, top of range" },
	{ 0x0B8U, "~-2.14V, mid" },
	{ 0x172U, "-4.00V, bottom of normal range" },
	{ 0x1FEU, "VSSA" },
	{ 0x1FFU, "HZ, common electrode floating" },
	{ 0x092U, "restore" },
};

static void probe_frm_vcom_sweep(const struct device *dsi)
{
	static const uint8_t extc_cmd[] = { 0xB9U, 0xFFU, 0x83U, 0x94U };
	static const uint8_t frm_on[]   = { 0x00U, 0x80U, 0x64U, 0x0CU, 0x0DU, 0x2FU,
		                                0x00U, 0x00U, 0x00U, 0x00U, 0xC8U };
	static const uint8_t frm_off[]  = { 0x00U, 0x80U, 0x64U, 0x0CU, 0x0DU, 0x2FU };
	ssize_t              vcom_rc[FRMV_STEP_COUNT];
	bool                 all_awake = true;
	ssize_t              rc;

	/* 1. Confirm awake. */
	{
		uint8_t  rddpm = 0xAAU;
		uint32_t int0  = 0;
		uint32_t int1  = 0;

		rc = dcs_read_classified(dsi, 0x0AU, &rddpm, 1U, &int0, &int1);
		if (rc < 1) {
			printk("frmv: entry RDDPM read FAILED rc=%d -- panel state UNVERIFIED\n", (int)rc);
			return;
		}

		int sleep_out  = (int)((rddpm & BIT(4)) != 0);
		int display_on = (int)((rddpm & BIT(2)) != 0);

		printk(
		    "frmv: entry RDDPM=0x%02x sleep_out=%d display_on=%d\n", rddpm, sleep_out, display_on);
		all_awake = sleep_out && display_on;
	}

	/* 2. Enable FRM. */
	{
		struct mipi_dsi_msg w = {
			.type   = MIPI_DSI_GENERIC_LONG_WRITE,
			.flags  = MIPI_DSI_MSG_USE_LPM,
			.tx_buf = extc_cmd,
			.tx_len = sizeof(extc_cmd),
		};
		rc = mipi_dsi_transfer(dsi, 0, &w);
		printk("frmv: SETEXTC (B9h FF 83 94) rc=%d\n", (int)rc);
	}
	{
		struct mipi_dsi_msg w = {
			.type   = MIPI_DSI_DCS_LONG_WRITE,
			.flags  = MIPI_DSI_MSG_USE_LPM,
			.cmd    = 0xB2U,
			.tx_buf = frm_on,
			.tx_len = sizeof(frm_on),
		};
		rc = mipi_dsi_transfer(dsi, 0, &w);
		printk("frmv: SETDISP FRM-ON B2 00 80 64 0C 0D 2F 00 00 00 00 C8 rc=%d\n", (int)rc);
	}

	/* 3. Sweep VCOM with FRM running. */
	for (int step = 0; step < FRMV_STEP_COUNT; step++) {
		uint16_t vcmc = frmv_steps[step].vcmc;
		uint8_t  p1   = (uint8_t)(vcmc & 0xFFU);
		uint8_t  p2   = (uint8_t)(vcmc & 0xFFU);
		uint8_t  p3   = (uint8_t)(0xE0U | (((vcmc >> 8) & 1U) << 1) | ((vcmc >> 8) & 1U));
		ssize_t  extc_rc;

		{
			struct mipi_dsi_msg w = {
				.type   = MIPI_DSI_GENERIC_LONG_WRITE,
				.flags  = MIPI_DSI_MSG_USE_LPM,
				.tx_buf = extc_cmd,
				.tx_len = sizeof(extc_cmd),
			};
			extc_rc = mipi_dsi_transfer(dsi, 0, &w);
		}
		{
			uint8_t             vcom[3] = { p1, p2, p3 };
			struct mipi_dsi_msg w       = {
				.type   = MIPI_DSI_DCS_LONG_WRITE,
				.flags  = MIPI_DSI_MSG_USE_LPM,
				.cmd    = 0xB6U,
				.tx_buf = vcom,
				.tx_len = sizeof(vcom),
			};
			vcom_rc[step] = mipi_dsi_transfer(dsi, 0, &w);
			printk("frmv: step %d SETVCOM (B6h) %02x %02x %02x extc_rc=%d vcom_rc=%d "
			       "(%s)\n",
			       step,
			       vcom[0],
			       vcom[1],
			       vcom[2],
			       (int)extc_rc,
			       (int)vcom_rc[step],
			       frmv_steps[step].meaning);
		}

		k_sleep(K_MSEC(8000));
		printk("frmv: step %d VCMC=0x%03x param3=0x%02x extc_rc=%d vcom_rc=%d -- CAPTURE "
		       "NOW\n",
		       step,
		       vcmc,
		       p3,
		       (int)extc_rc,
		       (int)vcom_rc[step]);

		{
			uint8_t  rddpm = 0xAAU;
			uint32_t int0  = 0;
			uint32_t int1  = 0;

			rc = dcs_read_classified(dsi, 0x0AU, &rddpm, 1U, &int0, &int1);
			if (rc < 1) {
				printk("frmv: step %d after RDDPM read FAILED rc=%d -- panel "
				       "state UNVERIFIED\n",
				       step,
				       (int)rc);
				all_awake = false;
				continue;
			}

			int sleep_out  = (int)((rddpm & BIT(4)) != 0);
			int display_on = (int)((rddpm & BIT(2)) != 0);

			printk("frmv: step %d after RDDPM=0x%02x sleep_out=%d display_on=%d\n",
			       step,
			       rddpm,
			       sleep_out,
			       display_on);
			all_awake = all_awake && sleep_out && display_on;
		}
	}

	/* 4. Restore. */
	{
		struct mipi_dsi_msg w = {
			.type   = MIPI_DSI_GENERIC_LONG_WRITE,
			.flags  = MIPI_DSI_MSG_USE_LPM,
			.tx_buf = extc_cmd,
			.tx_len = sizeof(extc_cmd),
		};
		rc = mipi_dsi_transfer(dsi, 0, &w);
		printk("frmv: SETEXTC (B9h FF 83 94) rc=%d\n", (int)rc);
	}
	{
		struct mipi_dsi_msg w = {
			.type   = MIPI_DSI_DCS_LONG_WRITE,
			.flags  = MIPI_DSI_MSG_USE_LPM,
			.cmd    = 0xB2U,
			.tx_buf = frm_off,
			.tx_len = sizeof(frm_off),
		};
		rc = mipi_dsi_transfer(dsi, 0, &w);
		printk("frmv: SETDISP restore B2 00 80 64 0C 0D 2F rc=%d\n", (int)rc);
	}
	printk("frmv: END\n");

	/* 5. Verdict. */
	printk("frmv: VERDICT all_steps_awake=%d vcom_rc=[%d,%d,%d,%d,%d,%d,%d]\n",
	       (int)all_awake,
	       (int)vcom_rc[0],
	       (int)vcom_rc[1],
	       (int)vcom_rc[2],
	       (int)vcom_rc[3],
	       (int)vcom_rc[4],
	       (int)vcom_rc[5],
	       (int)vcom_rc[6]);
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

/*
 * Which mechanism, if either, can actually read B0h back on THIS board over
 * DSI.  probe_b0_readback() decides this; probe_stage_current() refuses to
 * run its read-modify-write sequence unless one of the two came back
 * B0_READ_NONE-free -- a blind write to B0h could disable a stage this app
 * could then never read back to restore.
 */
enum b0_read_mech {
	B0_READ_NONE    = 0,
	B0_READ_SPI     = 1, /* SETREADINDEX(FEh) + GETSPIREAD(FFh), Sec5.19.30/31 pp.250-251. */
	B0_READ_GENERIC = 2, /* the register's own command byte, direct DCS read. */
};

static ssize_t send_setextc(const struct device *dsi, const char *prefix)
{
	static const uint8_t extc_cmd[] = { 0xB9U, 0xFFU, 0x83U, 0x94U };
	struct mipi_dsi_msg  w          = {
		.type   = MIPI_DSI_GENERIC_LONG_WRITE,
		.flags  = MIPI_DSI_MSG_USE_LPM,
		.tx_buf = extc_cmd,
		.tx_len = sizeof(extc_cmd),
	};
	ssize_t rc = mipi_dsi_transfer(dsi, 0, &w);

	printk("%s: SETEXTC (B9h FF 83 94) rc=%d\n", prefix, (int)rc);
	return rc;
}

/*
 * SETREADINDEX (FEh) primes the target command byte, GETSPIREAD (FFh) reads
 * it back (Sec5.19.30 p.250, Sec5.19.31 p.251).  int0/int1 are captured
 * around the FFh read only, the same read-to-clear latches
 * dcs_read_classified() protects everywhere else in this file.
 */
static ssize_t feff_read(const struct device *dsi,
                         uint8_t              reg,
                         uint8_t             *buf,
                         size_t               len,
                         uint32_t            *int0,
                         uint32_t            *int1)
{
	uint8_t             idx   = reg;
	struct mipi_dsi_msg idx_w = {
		.type   = MIPI_DSI_DCS_LONG_WRITE,
		.flags  = MIPI_DSI_MSG_USE_LPM,
		.cmd    = 0xFEU,
		.tx_buf = &idx,
		.tx_len = 1U,
	};
	ssize_t idx_rc = mipi_dsi_transfer(dsi, 0, &idx_w);

	if (idx_rc < 0) {
		*int0 = 0;
		*int1 = 0;
		return idx_rc;
	}
	return dcs_read_classified(dsi, 0xFFU, buf, len, int0, int1);
}

static ssize_t
b0_read_via(const struct device *dsi, enum b0_read_mech mech, uint8_t *buf, size_t len)
{
	uint32_t int0;
	uint32_t int1;

	if (mech == B0_READ_SPI) {
		return feff_read(dsi, 0xB0U, buf, len, &int0, &int1);
	}
	if (mech == B0_READ_GENERIC) {
		return dcs_read_lpm(dsi, 0xB0U, buf, len);
	}
	return -ENOTSUP;
}

static void b0rd_print_hex(const char *tag, const uint8_t *buf, size_t len, ssize_t rc)
{
	printk("b0rd: %s raw=", tag);
	for (size_t i = 0; i < len; i++) {
		printk("%02x ", buf[i]);
	}
	printk("%s\n", (rc == (ssize_t)len) ? "" : "<- READ FAILED, sentinel");
}

/*
 * Reads one register BOTH ways and prints both raw results side by side.
 * The FEh/FFh path is documented for the SPI interface only -- the
 * datasheet never states whether it also works over DSI -- so a failure
 * there alone is inconclusive, not a fault; the plain generic read (the
 * register's own command byte, exactly like every other probe in this file)
 * is the cross-check.
 */
static void b0rd_dual_read(const struct device *dsi,
                           uint8_t              reg,
                           size_t               len,
                           const char          *name,
                           uint8_t             *spi_out,
                           ssize_t             *spi_rc,
                           uint8_t             *gen_out,
                           ssize_t             *gen_rc)
{
	uint32_t spi_int0 = 0;
	uint32_t spi_int1 = 0;
	uint32_t gen_int0 = 0;
	uint32_t gen_int1 = 0;

	memset(spi_out, 0xAAU, len);
	memset(gen_out, 0xAAU, len);

	*spi_rc = feff_read(dsi, reg, spi_out, len, &spi_int0, &spi_int1);
	printk(
	    "b0rd: %s FEh/FFh rc=%d int0=0x%08x int1=0x%08x\n", name, (int)*spi_rc, spi_int0, spi_int1);
	b0rd_print_hex(name, spi_out, len, *spi_rc);

	*gen_rc = dcs_read_classified(dsi, reg, gen_out, len, &gen_int0, &gen_int1);
	printk(
	    "b0rd: %s direct  rc=%d int0=0x%08x int1=0x%08x\n", name, (int)*gen_rc, gen_int0, gen_int1);
	b0rd_print_hex(name, gen_out, len, *gen_rc);
}

/*
 * BENCH DIAGNOSTIC -- probe_b0_readback() (#2199), prefix `b0rd:`.
 *
 * The one remaining way a software root cause could be hiding.  A root-cause
 * pass upstream of this probe already showed the dark-glass state needs only
 * VDD1 + HS_VCC + RESX + the DSI lanes, and does NOT need VDD3 / VSP / VSN /
 * VGH / VGL / VCL / VCOM -- exactly the set the glass needs and the logic
 * does not.  If the SLPOUT state machine never actually enabled the analog
 * stages, the IC is halting its own outputs and that is fixable in software.
 * If every enable reads back SET, the IC is trying to drive and the fault is
 * electrical, downstream of the IC.
 *
 * Reads the IC's real post-init power state through the datasheet's own
 * user-define read mechanism (SETREADINDEX/GETSPIREAD, see feff_read()
 * above) AND a plain generic read, side by side, via b0rd_dual_read().
 * SETEXTC (B9h FF 83 94) is required before every read below -- each
 * register's own Restrictions line says so.  B0h is READ-ONLY here: this
 * probe never writes it.
 *
 * Bit positions, transcribed from the datasheet tables, not assumed:
 *
 *   B0h SETSEQUENCE (Sec5.19.1 pp.184-185), 4 parameters:
 *     param2 D0 = OSC_EN
 *     param3 D6=VSN_EN D5=VSP_EN D4=VGL_EN D3=VGH_EN D2=VCL_EN D0=STB
 *     param4 D3=GON D2=DTE D1:D0=D[1:0]
 *
 *   B1h SETPOWER (Sec5.19.2 p.186) Bank 0 -- every init path in this file
 *   (the driver's own and probe_reinit_180ms()'s) ends on bank 00h, so a
 *   plain B1h read decodes against the Bank 0 table:
 *     param1 D6:D5=POWMOD[1:0] D4:D2=AP[2:0]
 *     param7 D7:D5=VGH_RATIO[2:0]
 *     param8 D7:D5=VGL_RATIO[2:0]
 *     param9  = VGHS[7:0]
 *     param10 = VGLS[7:0]
 *
 *   B2h SETDISP (Sec5.19.3 p.198) parameter 11 D3 = DISP_BIST_EN.
 *
 *   CCh SETPANEL (Sec5.19.17 p.223) parameter 1 -- this app's own init
 *   writes 0x03 to it, so a correct 0x03 readback is a positive control: it
 *   proves whichever mechanism produced it CAN read a register this app set
 *   to a known non-default value.
 *
 *   D2h SETOFFSET (Sec5.19.18 p.224) parameter 1 -- documented default 0x55,
 *   never written by this app or the driver, so a correct 0x55 readback is a
 *   second, independent positive control: it proves the mechanism can read
 *   even a register nobody touched.
 *
 * Returns which mechanism, if either, actually read B0h back reliably --
 * probe_stage_current() uses this to decide whether it may run at all.
 */
static enum b0_read_mech probe_b0_readback(const struct device *dsi)
{
	uint8_t b0_spi[4];
	uint8_t b0_gen[4];
	uint8_t b1_spi[10];
	uint8_t b1_gen[10];
	uint8_t b2_spi[11];
	uint8_t b2_gen[11];
	uint8_t cc_spi[1];
	uint8_t cc_gen[1];
	uint8_t d2_spi[1];
	uint8_t d2_gen[1];
	ssize_t b0_spi_rc, b0_gen_rc;
	ssize_t b1_spi_rc, b1_gen_rc;
	ssize_t b2_spi_rc, b2_gen_rc;
	ssize_t cc_spi_rc, cc_gen_rc;
	ssize_t d2_spi_rc, d2_gen_rc;

	send_setextc(dsi, "b0rd");
	b0rd_dual_read(dsi, 0xB0U, sizeof(b0_spi), "B0h", b0_spi, &b0_spi_rc, b0_gen, &b0_gen_rc);
	send_setextc(dsi, "b0rd");
	b0rd_dual_read(dsi, 0xB1U, sizeof(b1_spi), "B1h", b1_spi, &b1_spi_rc, b1_gen, &b1_gen_rc);
	send_setextc(dsi, "b0rd");
	b0rd_dual_read(dsi, 0xB2U, sizeof(b2_spi), "B2h", b2_spi, &b2_spi_rc, b2_gen, &b2_gen_rc);
	send_setextc(dsi, "b0rd");
	b0rd_dual_read(dsi, 0xCCU, sizeof(cc_spi), "CCh", cc_spi, &cc_spi_rc, cc_gen, &cc_gen_rc);
	send_setextc(dsi, "b0rd");
	b0rd_dual_read(dsi, 0xD2U, sizeof(d2_spi), "D2h", d2_spi, &d2_spi_rc, d2_gen, &d2_gen_rc);

	/* Decode B0h -- prefer whichever mechanism returned the full byte count. */
	bool           b0_spi_ok = (b0_spi_rc == (ssize_t)sizeof(b0_spi));
	bool           b0_gen_ok = (b0_gen_rc == (ssize_t)sizeof(b0_gen));
	const uint8_t *b0        = b0_spi_ok ? b0_spi : (b0_gen_ok ? b0_gen : NULL);
	const char    *b0_src    = b0_spi_ok ? "FEh/FFh" : (b0_gen_ok ? "direct" : "none");

	if (b0 != NULL) {
		printk("b0rd: B0h decode (via %s) OSC_EN=%d STB=%d VCL_EN=%d VGH_EN=%d VGL_EN=%d "
		       "VSP_EN=%d VSN_EN=%d GON=%d DTE=%d D[1:0]=%d\n",
		       b0_src,
		       (int)((b0[1] & BIT(0)) != 0),
		       (int)((b0[2] & BIT(0)) != 0),
		       (int)((b0[2] & BIT(2)) != 0),
		       (int)((b0[2] & BIT(3)) != 0),
		       (int)((b0[2] & BIT(4)) != 0),
		       (int)((b0[2] & BIT(5)) != 0),
		       (int)((b0[2] & BIT(6)) != 0),
		       (int)((b0[3] & BIT(3)) != 0),
		       (int)((b0[3] & BIT(2)) != 0),
		       (int)(b0[3] & 0x03U));
	} else {
		printk("b0rd: B0h decode SKIPPED -- both read mechanisms failed\n");
	}

	/* Decode B1h Bank 0. */
	bool           b1_spi_ok = (b1_spi_rc == (ssize_t)sizeof(b1_spi));
	bool           b1_gen_ok = (b1_gen_rc == (ssize_t)sizeof(b1_gen));
	const uint8_t *b1        = b1_spi_ok ? b1_spi : (b1_gen_ok ? b1_gen : NULL);
	const char    *b1_src    = b1_spi_ok ? "FEh/FFh" : (b1_gen_ok ? "direct" : "none");

	if (b1 != NULL) {
		printk("b0rd: B1h decode (via %s) POWMOD=%d AP=%d VGH_RATIO=%d VGL_RATIO=%d "
		       "VGHS=0x%02x VGLS=0x%02x\n",
		       b1_src,
		       (int)((b1[0] >> 5) & 0x03U),
		       (int)((b1[0] >> 2) & 0x07U),
		       (int)((b1[6] >> 5) & 0x07U),
		       (int)((b1[7] >> 5) & 0x07U),
		       b1[8],
		       b1[9]);
	} else {
		printk("b0rd: B1h decode SKIPPED -- both read mechanisms failed\n");
	}

	/* Decode B2h parameter 11: DISP_BIST_EN. */
	bool           b2_spi_ok = (b2_spi_rc == (ssize_t)sizeof(b2_spi));
	bool           b2_gen_ok = (b2_gen_rc == (ssize_t)sizeof(b2_gen));
	const uint8_t *b2        = b2_spi_ok ? b2_spi : (b2_gen_ok ? b2_gen : NULL);
	const char    *b2_src    = b2_spi_ok ? "FEh/FFh" : (b2_gen_ok ? "direct" : "none");

	if (b2 != NULL) {
		printk("b0rd: B2h decode (via %s) DISP_BIST_EN(param11)=%d\n",
		       b2_src,
		       (int)((b2[10] & BIT(3)) != 0));
	} else {
		printk("b0rd: B2h decode SKIPPED -- both read mechanisms failed\n");
	}

	/* Positive controls. */
	bool cc_spi_hit = (cc_spi_rc == (ssize_t)sizeof(cc_spi)) && (cc_spi[0] == 0x03U);
	bool cc_gen_hit = (cc_gen_rc == (ssize_t)sizeof(cc_gen)) && (cc_gen[0] == 0x03U);
	bool d2_spi_hit = (d2_spi_rc == (ssize_t)sizeof(d2_spi)) && (d2_spi[0] == 0x55U);
	bool d2_gen_hit = (d2_gen_rc == (ssize_t)sizeof(d2_gen)) && (d2_gen[0] == 0x55U);

	printk("b0rd: controls CCh==0x03 FEh/FFh=%d direct=%d | D2h==0x55 FEh/FFh=%d direct=%d\n",
	       (int)cc_spi_hit,
	       (int)cc_gen_hit,
	       (int)d2_spi_hit,
	       (int)d2_gen_hit);

	/* Verdict. */
	bool enables_set    = (b0 != NULL) &&
	                      ((b0[2] & (BIT(6) | BIT(5) | BIT(4) | BIT(3) | BIT(2))) ==
	                       (BIT(6) | BIT(5) | BIT(4) | BIT(3) | BIT(2))) &&
	                      ((b0[3] & (BIT(3) | BIT(2))) == (BIT(3) | BIT(2)));
	bool any_control_ok = cc_spi_hit || cc_gen_hit || d2_spi_hit || d2_gen_hit;

	/*
	 * The verdict MUST gate on the positive controls first.  Bytes coming back
	 * is not the same as the read path working: this panel returns rc=4 with
	 * an all-zero payload for gated manufacturer reads, and an all-zero B0h
	 * decodes to "every enable clear", which reads as a software-fixable root
	 * cause.  That is a dead read path being decoded as data -- the exact
	 * failure this probe's controls exist to catch, and the first version of
	 * this ladder printed the false conclusion anyway because it never
	 * consulted them.
	 *
	 * CCh is written to 0x03 by the panel init and D2h has a documented 0x55
	 * default that no init touches.  If neither comes back on either mechanism,
	 * nothing read here is evidence about the IC.
	 */
	printk("b0rd: VERDICT B0h_source=%s enables_all_set=%d control_confirmed=%d -- %s\n",
	       b0_src,
	       (int)enables_set,
	       (int)any_control_ok,
	       !any_control_ok ? "READ PATH UNPROVEN: neither CCh==0x03 nor D2h==0x55 came back "
	                         "on either mechanism, so the B0h bytes above are NOT evidence "
	                         "about the SLPOUT state machine -- an all-zero B0h is what a "
	                         "dead gated read returns, not what a halted IC looks like"
	       : (b0 == NULL) ? "B0h unreadable, cannot speak to the SLPOUT state machine"
	       : enables_set  ? "every enable reads SET -- IC is trying to drive the analog "
	                        "stages, fault is downstream (electrical)"
	                      : "at least one enable reads CLEAR -- SLPOUT never actually "
	                        "enabled the analog stage, fixable in software");

	/*
	 * Report the read path as unusable unless a control confirmed it, so
	 * probe_stage_current() refuses rather than doing read-modify-write on
	 * bytes that may be a failed read.  Writing a B0h derived from 0x00000000
	 * clears OSC_EN and every analog enable -- recoverable by RESX plus
	 * re-init, but it halts the IC for the rest of the run and voids every
	 * measurement after it.
	 */
	if (!any_control_ok) {
		return B0_READ_NONE;
	}

	return b0_spi_ok ? B0_READ_SPI : (b0_gen_ok ? B0_READ_GENERIC : B0_READ_NONE);
}

#define STGI_PHASE_HOLD_MS 8000

/*
 * Read-modify-write of ONE named bit-group inside B0h off the CURRENT
 * register value -- never composed from scratch.  clear_mask bits are
 * cleared, set_mask bits are set; every other bit (and all 3 other bytes)
 * passes through unchanged.  Uses whichever mechanism probe_b0_readback()
 * showed works.
 */
static bool stgi_rmw_b0(const struct device *dsi,
                        enum b0_read_mech    mech,
                        uint8_t              byte_idx,
                        uint8_t              clear_mask,
                        uint8_t              set_mask,
                        const char          *tag)
{
	uint8_t buf[4];
	ssize_t rrc = b0_read_via(dsi, mech, buf, sizeof(buf));

	if (rrc != (ssize_t)sizeof(buf)) {
		printk("stgi: %s B0h read FAILED rc=%d -- refusing blind write\n", tag, (int)rrc);
		return false;
	}

	buf[byte_idx] = (uint8_t)((buf[byte_idx] & (uint8_t)~clear_mask) | set_mask);

	struct mipi_dsi_msg w = {
		.type   = MIPI_DSI_DCS_LONG_WRITE,
		.flags  = MIPI_DSI_MSG_USE_LPM,
		.cmd    = 0xB0U,
		.tx_buf = buf,
		.tx_len = sizeof(buf),
	};
	ssize_t wrc = mipi_dsi_transfer(dsi, 0, &w);

	printk("stgi: %s B0h write %02x %02x %02x %02x rc=%d\n",
	       tag,
	       buf[0],
	       buf[1],
	       buf[2],
	       buf[3],
	       (int)wrc);
	return wrc == (ssize_t)sizeof(buf);
}

/*
 * BENCH DIAGNOSTIC -- probe_stage_current() (#2199), prefix `stgi:`.
 *
 * A current-signature probe: each B0h analog-stage enable that really
 * switches a load should move the board's 16V input current when toggled.
 * An external DPS sampler buckets its readings against the marker lines
 * below. Panel state matters and is the point -- probe_bist_v2() and
 * probe_frm_vcom_sweep() already ran with FRM active and saw nothing on the
 * glass, so this probe reproduces that exact state (Sleep Out, Display On,
 * FRM running via SETDISP with parameter 11 bit3 set -- the same byte string
 * probe_bist_v2() uses) and toggles the enables underneath it.
 *
 * B0h is documented "for DCS command auto sequence and manual mode debug
 * use, please don't access this command in initial code" (Sec5.19.1 p.184)
 * -- that warns against writing it from INIT code, not from a debug probe,
 * which is what this is.  Still handled carefully: every write is a
 * read-modify-write off the value stgi_rmw_b0() just read (never composed
 * from scratch), the exact original bytes captured at entry are
 * re-asserted at the end, and if anything goes wrong a RESX pulse followed
 * by re-init (see panel_wake_after_resx() / probe_reinit_180ms()) recovers
 * the IC.
 *
 * Refuses to run at all if probe_b0_readback() found neither read mechanism
 * reliable for B0h -- a blind write here could disable a stage this probe
 * could then never read back to restore.
 */
static void probe_stage_current(const struct device *dsi, enum b0_read_mech b0_mech)
{
	static const uint8_t frm_on[] = { 0x00U, 0x80U, 0x64U, 0x0CU, 0x0DU, 0x2FU,
		                              0x00U, 0x00U, 0x00U, 0x00U, 0xC8U };
	uint8_t              baseline[4];
	ssize_t              rc;

	if (b0_mech == B0_READ_NONE) {
		printk("stgi: SKIPPED -- B0h not readable, refusing blind read-modify-write\n");
		return;
	}

	/* 1. Confirm awake before touching anything. */
	{
		uint8_t  rddpm = 0xAAU;
		uint32_t int0  = 0;
		uint32_t int1  = 0;

		rc = dcs_read_classified(dsi, 0x0AU, &rddpm, 1U, &int0, &int1);
		printk("stgi: entry RDDPM rc=%d data=0x%02x%s\n",
		       (int)rc,
		       rddpm,
		       (rc < 1) ? "  <- READ FAILED, sentinel" : "");
	}

	/* 2. Sleep Out + Display On + FRM, reusing probe_bist_v2()'s own byte string. */
	(void)send_setextc(dsi, "stgi");
	{
		struct mipi_dsi_msg slpout_w = {
			.type  = MIPI_DSI_DCS_SHORT_WRITE,
			.flags = MIPI_DSI_MSG_USE_LPM,
			.cmd   = 0x11U, /* EXIT_SLEEP_MODE */
		};
		ssize_t slpout_rc = mipi_dsi_transfer(dsi, 0, &slpout_w);

		printk("stgi: EXIT_SLEEP_MODE rc=%d\n", (int)slpout_rc);
		k_msleep(120);

		struct mipi_dsi_msg dispon_w = {
			.type  = MIPI_DSI_DCS_SHORT_WRITE,
			.flags = MIPI_DSI_MSG_USE_LPM,
			.cmd   = 0x29U, /* SET_DISPLAY_ON */
		};
		ssize_t dispon_rc = mipi_dsi_transfer(dsi, 0, &dispon_w);

		printk("stgi: SET_DISPLAY_ON rc=%d\n", (int)dispon_rc);
	}
	(void)send_setextc(dsi, "stgi");
	{
		struct mipi_dsi_msg w = {
			.type   = MIPI_DSI_DCS_LONG_WRITE,
			.flags  = MIPI_DSI_MSG_USE_LPM,
			.cmd    = 0xB2U,
			.tx_buf = frm_on,
			.tx_len = sizeof(frm_on),
		};
		rc = mipi_dsi_transfer(dsi, 0, &w);
		printk("stgi: SETDISP FRM-ON B2 00 80 64 0C 0D 2F 00 00 00 00 C8 rc=%d\n", (int)rc);
	}

	/* 3. Capture the ORIGINAL B0h value -- restored verbatim at the end. */
	rc = b0_read_via(dsi, b0_mech, baseline, sizeof(baseline));
	if (rc != (ssize_t)sizeof(baseline)) {
		printk("stgi: ABORT -- baseline B0h read FAILED rc=%d, refusing to proceed\n", (int)rc);
		return;
	}
	printk("stgi: baseline B0h=%02x %02x %02x %02x\n",
	       baseline[0],
	       baseline[1],
	       baseline[2],
	       baseline[3]);

#define STGI_PHASE(n, desc) \
	do { \
		uint8_t  pm = 0xAAU; \
		uint32_t i0 = 0; \
		uint32_t i1 = 0; \
		ssize_t  prc; \
		printk("stgi: phase %d %s -- SAMPLE NOW\n", (n), (desc)); \
		k_msleep(STGI_PHASE_HOLD_MS); \
		prc = dcs_read_classified(dsi, 0x0AU, &pm, 1U, &i0, &i1); \
		printk("stgi: phase %d RDDPM rc=%d data=0x%02x%s\n", \
		       (n), \
		       (int)prc, \
		       pm, \
		       (prc < 1) ? "  <- READ FAILED, sentinel" : ""); \
	} while (0)

	/* Phase 1: baseline, FRM running, nothing changed. */
	STGI_PHASE(1, "baseline, FRM running, nothing changed");

	/* Phase 2/3: VSP_EN + VSN_EN cleared, then restored. */
	stgi_rmw_b0(dsi, b0_mech, 2U, BIT(5) | BIT(6), 0U, "phase2 clear VSP_EN+VSN_EN");
	STGI_PHASE(2, "VSP_EN+VSN_EN CLEARED");
	stgi_rmw_b0(dsi, b0_mech, 2U, 0U, BIT(5) | BIT(6), "phase3 restore VSP_EN+VSN_EN");
	STGI_PHASE(3, "VSP_EN+VSN_EN restored");

	/* Phase 4/5: VGH_EN + VGL_EN cleared, then restored. */
	stgi_rmw_b0(dsi, b0_mech, 2U, BIT(3) | BIT(4), 0U, "phase4 clear VGH_EN+VGL_EN");
	STGI_PHASE(4, "VGH_EN+VGL_EN CLEARED");
	stgi_rmw_b0(dsi, b0_mech, 2U, 0U, BIT(3) | BIT(4), "phase5 restore VGH_EN+VGL_EN");
	STGI_PHASE(5, "VGH_EN+VGL_EN restored");

	/* Phase 6/7: GON cleared, then restored. */
	stgi_rmw_b0(dsi, b0_mech, 3U, BIT(3), 0U, "phase6 clear GON");
	STGI_PHASE(6, "GON CLEARED");
	stgi_rmw_b0(dsi, b0_mech, 3U, 0U, BIT(3), "phase7 restore GON");
	STGI_PHASE(7, "GON restored");

	/* Phase 8/9: DTE cleared, then restored. */
	stgi_rmw_b0(dsi, b0_mech, 3U, BIT(2), 0U, "phase8 clear DTE");
	STGI_PHASE(8, "DTE CLEARED");
	stgi_rmw_b0(dsi, b0_mech, 3U, 0U, BIT(2), "phase9 restore DTE");
	STGI_PHASE(9, "DTE restored");

#undef STGI_PHASE

	/* Final safety restore: re-assert the exact original B0h bytes captured at entry. */
	{
		struct mipi_dsi_msg w = {
			.type   = MIPI_DSI_DCS_LONG_WRITE,
			.flags  = MIPI_DSI_MSG_USE_LPM,
			.cmd    = 0xB0U,
			.tx_buf = baseline,
			.tx_len = sizeof(baseline),
		};
		ssize_t wrc = mipi_dsi_transfer(dsi, 0, &w);

		printk("stgi: final restore B0h=%02x %02x %02x %02x rc=%d\n",
		       baseline[0],
		       baseline[1],
		       baseline[2],
		       baseline[3],
		       (int)wrc);
	}

	printk("stgi: END\n");
}

/*
 * BENCH DIAGNOSTIC -- probe_pwr_current() (#2199), prefix `pwrc:`.
 *
 * Every OPTICAL avenue is exhausted: probe_bist_v2()'s Free Running Mode
 * burn-in generator and probe_frm_vcom_sweep()'s VCOM sweep across the
 * panel's entire range -- including VSSA and a floating common electrode --
 * both came back flat, below camera noise.  The open question left is
 * whether the panel MODULE is receiving power at all: the suspicion is that
 * the FFC at J6 is skewed or mis-seated, contacting the low/middle pins
 * (backlight 1/2, DSI lanes 5-18, LCD_RST_L 21 -- all demonstrably working)
 * but not the high-numbered end (DSI_I2C 26/27, CTP_RST_L/CTP_INT_L 28/29
 * dead every run; +3V3 30, LCD_PWR_EN_L 32, +5V 39/40 unverifiable from
 * firmware alone).
 *
 * This is an ELECTRICAL test, not an optical one: asserting LCD_PWR_EN
 * (expander U35 P0) should bring up the module's analog rails and draw
 * measurable current on the board's 16V input.  The bench DPS resolves
 * module-level steps easily -- a backlight toggle previously gave a clean
 * 0.146A -> 0.074A -> 0.146A step, about 1.16W -- so this probe repeats that
 * exact calibration (phases 3-4) alongside the LCD_PWR_EN toggle (phases
 * 1-2) an external power sampler can bucket against.  If phases 1-2 show no
 * step while 3-4 do, the sampler was working and the module rail genuinely
 * did not move.
 *
 * Runs LAST in main(), after every other probe, so nothing downstream
 * depends on the state it leaves.  Each phase holds 10s and is announced
 * with a "-- SAMPLE NOW" marker line so an external sampler can bucket its
 * readings; the expander's four registers (in/out/pol/cfg, via
 * dump_lcd_exp_regs()) are printed at every phase so the log shows what was
 * actually driven.
 *
 * Guarded read-modify-write on the output register (0x01), exactly like
 * exp_set_resx() above: LCD_RST is P1 and the touch reset is P3, and a
 * failed read would leave `out` at 0, driving both low on write -- so a
 * failed read aborts the toggle phases (1-2) rather than risk that.  The
 * backlight phases (3-4) do not touch the expander output register and
 * always run.
 *
 * FIX (#2199): the +0.0004A LCD_PWR_EN measurement below was originally
 * taken with the panel in whatever state main() had left it in -- and if
 * LCD_PWR_EN gates a regulator feeding the module's analog input, a panel
 * left in Sleep In draws near-zero on that rail regardless of whether the
 * regulator works, making a near-zero delta the CORRECT reading rather than
 * evidence the rail is dead.  This probe now puts the panel in Sleep Out +
 * Display On + FRM before phase 0, so the measurement is taken with the
 * analog domain actually demanded.
 */
#define EXP_CFG_REG        0x03U
#define EXP_PWR_EN_BIT     BIT(0) /* LCD_PWR_EN, expander P0. */
#define PWRC_PHASE_HOLD_MS 10000

static void probe_pwr_current(const struct device *dsi)
{
	/* 0. Wake the panel (Sleep Out + Display On + FRM) before measuring anything. */
	(void)send_setextc(dsi, "pwrc");
	{
		struct mipi_dsi_msg slpout_w = {
			.type  = MIPI_DSI_DCS_SHORT_WRITE,
			.flags = MIPI_DSI_MSG_USE_LPM,
			.cmd   = 0x11U, /* EXIT_SLEEP_MODE */
		};
		ssize_t slpout_rc = mipi_dsi_transfer(dsi, 0, &slpout_w);

		printk("pwrc: EXIT_SLEEP_MODE rc=%d\n", (int)slpout_rc);
		k_msleep(120);

		struct mipi_dsi_msg dispon_w = {
			.type  = MIPI_DSI_DCS_SHORT_WRITE,
			.flags = MIPI_DSI_MSG_USE_LPM,
			.cmd   = 0x29U, /* SET_DISPLAY_ON */
		};
		ssize_t dispon_rc = mipi_dsi_transfer(dsi, 0, &dispon_w);

		printk("pwrc: SET_DISPLAY_ON rc=%d\n", (int)dispon_rc);
	}
	(void)send_setextc(dsi, "pwrc");
	{
		static const uint8_t frm_on[] = { 0x00U, 0x80U, 0x64U, 0x0CU, 0x0DU, 0x2FU,
			                              0x00U, 0x00U, 0x00U, 0x00U, 0xC8U };
		struct mipi_dsi_msg  w        = {
			.type   = MIPI_DSI_DCS_LONG_WRITE,
			.flags  = MIPI_DSI_MSG_USE_LPM,
			.cmd    = 0xB2U,
			.tx_buf = frm_on,
			.tx_len = sizeof(frm_on),
		};
		ssize_t rc = mipi_dsi_transfer(dsi, 0, &w);

		printk("pwrc: SETDISP FRM-ON B2 00 80 64 0C 0D 2F 00 00 00 00 C8 rc=%d\n", (int)rc);
	}
	{
		uint8_t  rddpm = 0xAAU;
		uint32_t int0  = 0;
		uint32_t int1  = 0;
		ssize_t  rc    = dcs_read_classified(dsi, 0x0AU, &rddpm, 1U, &int0, &int1);

		printk("pwrc: post-wake RDDPM rc=%d data=0x%02x%s\n",
		       (int)rc,
		       rddpm,
		       (rc < 1) ? "  <- READ FAILED, sentinel" : "");
	}

	uint8_t cfg          = 0;
	int     cfg_rc       = i2c_reg_read_byte_dt(&lcd_exp_i2c, EXP_CFG_REG, &cfg);
	bool    p0_is_output = (cfg_rc == 0) && ((cfg & EXP_PWR_EN_BIT) == 0);

	printk("pwrc: P0 config cfg=0x%02x(rc%d) -- P0 configured as %s\n",
	       cfg,
	       cfg_rc,
	       p0_is_output ? "OUTPUT" : "NOT OUTPUT or read failed");

	uint8_t out      = 0;
	int     out_rc   = i2c_reg_read_byte_dt(&lcd_exp_i2c, EXP_OUTPUT_REG, &out);
	bool    guard_ok = (out_rc == 0);
	int     booted   = guard_ok ? (int)((out & EXP_PWR_EN_BIT) != 0) : -1;

	printk("pwrc: phase 0 baseline P0=%d -- SAMPLE NOW\n", booted);
	dump_lcd_exp_regs("pwrc-phase0");
	k_msleep(PWRC_PHASE_HOLD_MS);

	if (!guard_ok) {
		printk("pwrc: P0 ABORT -- output-register read failed rc=%d, refusing RMW\n", out_rc);
	} else {
		uint8_t want1 = booted ? (uint8_t)(out & ~EXP_PWR_EN_BIT) : (out | EXP_PWR_EN_BIT);
		int     wr1   = i2c_reg_write_byte_dt(&lcd_exp_i2c, EXP_OUTPUT_REG, want1);

		printk(
		    "pwrc: phase 1 P0=%d -- SAMPLE NOW (wrote=0x%02x write-rc=%d)\n", !booted, want1, wr1);
		dump_lcd_exp_regs("pwrc-phase1");
		k_msleep(PWRC_PHASE_HOLD_MS);

		uint8_t want2 = booted ? (want1 | EXP_PWR_EN_BIT) : (uint8_t)(want1 & ~EXP_PWR_EN_BIT);
		int     wr2   = i2c_reg_write_byte_dt(&lcd_exp_i2c, EXP_OUTPUT_REG, want2);

		printk("pwrc: phase 2 P0=%d restored -- SAMPLE NOW (wrote=0x%02x write-rc=%d)\n",
		       booted,
		       want2,
		       wr2);
		dump_lcd_exp_regs("pwrc-phase2");
		k_msleep(PWRC_PHASE_HOLD_MS);
	}

	int bl_off_rc = gpio_pin_set_dt(&bl_gpio, 0);

	printk("pwrc: phase 3 backlight OFF -- SAMPLE NOW (rc=%d)\n", bl_off_rc);
	dump_lcd_exp_regs("pwrc-phase3");
	k_msleep(PWRC_PHASE_HOLD_MS);

	int bl_on_rc = gpio_pin_set_dt(&bl_gpio, 1);

	printk("pwrc: phase 4 backlight ON -- SAMPLE NOW (rc=%d)\n", bl_on_rc);
	dump_lcd_exp_regs("pwrc-phase4");
	k_msleep(PWRC_PHASE_HOLD_MS);

	printk("pwrc: END P0 restored to booted level, backlight ON\n");
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

		/*
		 * BENCH DIAGNOSTIC (#2199): probe_reinit_180ms() must run here --
		 * immediately after probe_setextc_gate_v2(), whose own final RESX
		 * pulse already left the panel awake via panel_wake_after_resx(),
		 * so this probe starts from a known state -- and before every
		 * probe below, so probe_panel_reads() onward all measure a panel
		 * brought up with the vendor 180ms dwell instead of the driver's
		 * 50ms floor.  See the probe's own banner.
		 */
		probe_reinit_180ms(dsi);

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

		/*
		 * BENCH DIAGNOSTIC (#2199): probe_b0_readback() must run here --
		 * still command mode, panel awake via probe_reinit_180ms() above,
		 * and before anything below ever enters video mode.
		 * probe_stage_current() runs immediately after it and depends on
		 * its return (which B0h read mechanism, if any, actually works)
		 * to decide whether its read-modify-write toggle sequence may run
		 * at all.
		 */
		enum b0_read_mech b0_mech = probe_b0_readback(dsi);

		probe_stage_current(dsi, b0_mech);
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
			 * BENCH DIAGNOSTIC (#2199): probe_frm_vcom_sweep() runs HERE,
			 * immediately after probe_bist_v2() returns -- still command
			 * mode, still before probe_read_wedge() below ever touches
			 * video mode, so DCS reads (and this sweep's own RDDPM
			 * self-certification) still work.  See the probe's own banner
			 * for why the earlier FRM-only result does not yet prove the
			 * glass cannot be driven.
			 */
			if (dsi_ok) {
				probe_frm_vcom_sweep(dsi);
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

	/*
	 * This line reports the CHAIN's state, and nothing about the glass.  It
	 * said PASS on every run for two days while the panel stayed blank, and
	 * three separate bench reports had to append the caveat by hand -- so it
	 * says so itself now.  Every stage below is a host-side or DSI-side
	 * readback: no signal in `pass` comes from an optical measurement, and
	 * the app has no way to see the glass.  Photometry decides pixels.
	 */
	if (pass) {
		printk("RESULT CHAIN-UP: RK055HDMIPI4MA0 chain reports ready -- hx8394 panel "
		       "+ mipi-dsi + cdc200 all bound, full-screen green frame written, "
		       "scanout clean. THIS IS NOT A PIXELS-ON-GLASS RESULT: every check "
		       "behind it is a host/DSI readback, none is optical. The glass may be "
		       "blank with all of them passing -- measure it with the camera.\n");
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

	/*
	 * Step 7 (BENCH DIAGNOSTIC, #2199): probe_pwr_current() runs absolutely
	 * last -- after RESULT PASS/FAIL is already decided and printed -- so
	 * nothing above or below depends on the state it leaves (see its own
	 * banner).
	 */
	probe_pwr_current(dsi);

	return 0;
}
