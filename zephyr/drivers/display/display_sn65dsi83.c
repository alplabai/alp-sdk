/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR-0017-ADJACENT (authored from the TI datasheet, no vendor SDK) ======
 * TI SN65DSI83 single-channel MIPI DSI receiver to single-link FlatLink(TM)
 * LVDS bridge.  No upstream Zephyr driver, no hal_alif library, and no
 * sdk-alif fork driver exists for this part class, so the whole driver is
 * authored clean-room against the public datasheet (Texas Instruments
 * SLLSEC1, "SN65DSI83 MIPI DSI Bridge to FlatLink LVDS"): Table 7-2 (the
 * initialization sequence) and Tables 7-4..7-9 (the CSR bit-field maps).
 * NOT ported from Linux's ti-sn65dsi83.c (GPL-2.0) -- see
 * docs/adr/0017-alp-sdk-over-the-vendor-sdk.md.
 *
 * BENCH-UNVERIFIED: written for a maintainer-built adapter PCB with no
 * hardware on hand for this change.  Every register value below is computed
 * from the datasheet's documented formulas and cross-checked against the
 * worked example in this driver's own devicetree overlay comment; none of it
 * has been read back from a real SN65DSI83.  Re-verify CSR 0x00..0x08 (the ID
 * check), the PLL_EN_STAT poll and CSR 0xE5 on the bench before shipping.
 * ================================================================================
 *
 * WHAT THIS DRIVER DOES NOT DO: it implements no Zephyr display-class API.
 * Once its CSR bank is programmed the bridge is a transparent DSI-to-LVDS
 * converter -- the CDC200 (tes,cdc-2.1) stays the `zephyr,display` chosen
 * node and owns the framebuffer, layers and blanking.  This device exists
 * only to get itself into that transparent state once, at init, which is why
 * `DEVICE_DT_INST_DEFINE()` below passes no display_driver_api.
 *
 * INIT ORDER (why this runs at CONFIG_APPLICATION_INIT_PRIORITY, matching
 * upstream Zephyr's himax,hx8394 panel driver): by the time this device
 * initializes, the I2C bus, the carrier's PCA9538 GPIO expander (this
 * bridge's EN pin lives behind it on this shield) and the DesignWare MIPI-DSI
 * host all need to be ready.  APPLICATION (90) is after the expander (I2C
 * bus priority, 50), the fixed regulators (75) and the DSI host + CDC200
 * (85) -- see e1m_evk_rvt121hvdfwca0/Kconfig.defconfig.
 *
 * THE EN-PIN / CLOCK-LANE ORDERING (the one sequencing rule the whole driver
 * exists to get right): the datasheet's Table 7-2 init sequence 1-4 requires
 * the DSI clock lane to be in the HS state -- continuously, before the
 * bridge's EN pin is ever raised.  The bridge answers NO I2C transaction
 * at all with EN low, so this cannot be sequenced the other way around.  On
 * this SoC, "clock lane HS" is a MODE the DesignWare DSI host driver tracks
 * (dsi_dw_set_mode(), <zephyr/drivers/mipi_dsi/dsi_dw.h>) and drives via
 * DSI_LPCLK_CTRL.PHY_TXREQUESTCLKHS -- separate from the CDC200 actually
 * feeding it pixels (CDC_EN, toggled later by cdc200's blanking_on/off).  So:
 *
 *   1. EN low (>=10 ms)                            -- datasheet init seq 3
 *   2. mipi_dsi_attach()                            -- configures the host,
 *                                                       leaves it in COMMAND
 *                                                       mode (clock lane LP)
 *   3. dsi_dw_set_mode(DSI_DW_VIDEO_MODE)           -- switches the host to
 *                                                       VIDEO mode: the clock
 *                                                       lane goes HS and
 *                                                       stays there
 *                                                       (continuous -- see
 *                                                       below), with the CDC
 *                                                       not yet enabled, so
 *                                                       no pixel data is on
 *                                                       the wire yet
 *   4. EN high, wait 10 ms                          -- datasheet init seq 4
 *   5. read + verify CSR 0x00..0x08 (the ID check)  -- the bridge only
 *                                                       answers I2C now
 *   6. program the CSR bank from DT                 -- datasheet init seq 5
 *   7. PLL_EN, poll PLL_EN_STAT, SOFT_RESET, clear   -- datasheet init seq
 *      CSR 0xE5                                        6-10
 *
 * When the app later calls display_blanking_off(cdc200), cdc200's
 * blanking_off calls dsi_dw_set_mode(VIDEO_MODE) again; the host is already
 * there, so it early-returns 0 without touching the link (dsi_dw.c,
 * dsi_dw_set_mode_locked(): `if (mode == data->curr_mode) return 0;`) -- this
 * driver's own mode switch does not have to be undone or special-cased for
 * that later call to still work.
 *
 * MIPI_DSI_MODE_VIDEO_BURST, no MIPI_DSI_CLOCK_NON_CONTINUOUS: the LVDS pixel
 * clock this bridge outputs is DERIVED from the incoming DSI clock lane
 * (HS_CLK_SRC = 1, CSR 0x0A.0 below) -- if the host ever stopped that clock
 * between packets (the non-continuous-clock mode some panels use to save
 * power), the bridge's LVDS clock would stop with it and the panel would
 * lose sync every time.  Burst mode is requested because it is what the
 * cdc200/dsi_dw pairing on this SoC is proven with (the e1m_evk_rk055hdmipi4ma0
 * shield); non-burst was not tried against this bridge.
 *
 * ponytail: blanking_on() (cdc200_blanking_on -> dsi_dw_set_mode(COMMAND))
 * stops the HS clock lane, and this bridge's LVDS PLL is sourced from it
 * (HS_CLK_SRC=1) -- so a blanking_on/blanking_off cycle after this driver's
 * one-shot init almost certainly drops the bridge's PLL lock, and nothing
 * here re-runs PLL_EN/SOFT_RESET on the way back to video mode.  Not
 * implemented: no re-init hook is wired to cdc200's blanking_off.  Upgrade
 * path if a real app needs blanking: have it call
 * sn65dsi83's own re-lock helper (CSR 0x0D=0x01, poll 0x0A.7, CSR
 * 0x09=0x01, 3 ms) itself before display_blanking_off(), or wire a
 * blanking-aware hook once this is bench-verified.
 */

#define DT_DRV_COMPAT ti_sn65dsi83

#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display/sn65dsi83.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/mipi_dsi.h>
#include <zephyr/drivers/mipi_dsi/dsi_dw.h>
#include <zephyr/dt-bindings/mipi_dsi/mipi_dsi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(sn65dsi83, CONFIG_DISPLAY_LOG_LEVEL);

/* This driver only ever attaches as the DSI host's sole peripheral. */
#define SN65DSI83_DSI_CHANNEL 0U

/*
 * ponytail: the DT-derived register-value macros below (SN65_PCLK_HZ etc.)
 * all hardcode instance 0 (DT_INST_PHANDLE(0, ...)) rather than being
 * parameterized per-instance, matching display_cdc200.c's own
 * single-DSI-host assumption on this SoC.  A second ti,sn65dsi83 node would
 * silently share instance 0's clock/timing derivation.  Upgrade path if a
 * board ever needs two: parameterize every SN65_* macro on `inst` and move
 * them inside SN65DSI83_INIT().
 */
BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(ti_sn65dsi83) <= 1,
	     "display_sn65dsi83.c assumes a single ti,sn65dsi83 instance");

/* CSR addresses (datasheet Tables 7-4..7-9). */
#define SN65_REG_ID_BASE      0x00U /* 9-byte burst: CSR 0x00..0x08. */
#define SN65_REG_ID_LEN       9U
#define SN65_REG_SOFT_RESET   0x09U
#define SN65_REG_CLK_SRC      0x0AU /* LVDS_CLK_RANGE[3:1], HS_CLK_SRC[0]; PLL_EN_STAT[7] (R/O). */
#define SN65_REG_CLK_DIV      0x0BU /* DSI_CLK_DIVIDER[7:3], REFCLK_MULTIPLIER[1:0]. */
#define SN65_REG_PLL_EN       0x0DU
#define SN65_REG_DSI_LANE     0x10U /* reserved[7:5]=0b001 default, CHA_DSI_LANES[4:3]. */
#define SN65_REG_DSI_CLK_RANGE 0x12U
#define SN65_REG_LVDS_FMT     0x18U
#define SN65_REG_LINE_LEN_LOW  0x20U
#define SN65_REG_LINE_LEN_HIGH 0x21U
#define SN65_REG_VDISP_LOW    0x24U /* test-pattern only. */
#define SN65_REG_VDISP_HIGH   0x25U /* test-pattern only. */
#define SN65_REG_SYNC_DLY_LOW  0x28U
#define SN65_REG_SYNC_DLY_HIGH 0x29U
#define SN65_REG_HSYNC_PW_LOW  0x2CU
#define SN65_REG_HSYNC_PW_HIGH 0x2DU
#define SN65_REG_VSYNC_PW_LOW  0x30U
#define SN65_REG_VSYNC_PW_HIGH 0x31U
#define SN65_REG_HBP           0x34U
#define SN65_REG_VBP           0x36U /* test-pattern only. */
#define SN65_REG_HFP           0x38U /* test-pattern only. */
#define SN65_REG_VFP           0x3AU /* test-pattern only. */
#define SN65_REG_TEST_PATTERN  0x3CU
#define SN65_REG_IRQ_EN        0xE0U
#define SN65_REG_ERR_STAT      0xE5U

/* CSR 0x0A: PLL_EN_STAT (datasheet calls it "PLL locked" once set + 3 ms). */
#define SN65_CLK_SRC_PLL_EN_STAT BIT(7)

/* CSR 0x0D / 0x09. */
#define SN65_PLL_EN_BIT     BIT(0)
#define SN65_SOFT_RESET_BIT BIT(0)

/* CSR 0x18 (datasheet Table 7-7). */
#define SN65_LVDS_FMT_DE_POS       0U       /* bit7=0: DE positive (default). */
#define SN65_LVDS_FMT_HS_VS_NEG    (BIT(6) | BIT(5)) /* HS_NEG_POLARITY, VS_NEG_POLARITY (default). */
#define SN65_LVDS_FMT_RESERVED_BIT4 BIT(4)  /* must stay at its default of 1. */
#define SN65_LVDS_FMT_24BPP_MODE   BIT(3)
#define SN65_LVDS_FMT_24BPP_FORMAT1 BIT(1)

/* CSR 0x3C. */
#define SN65_TEST_PATTERN_EN BIT(4)

/*
 * DT-derived clock/timing constants.  These come from the panel's DSI
 * neighbours -- the cdc-if (CDC200) node's pixel clock and timings, and the
 * DSI host's declared lane bandwidth -- not from any property of THIS node,
 * because the datasheet's register values are all functions of the link the
 * bridge sits on, and that link is described once, in the DSI host subtree.
 * DT stays the single source of truth: change the panel timings or the lane
 * bandwidth in the overlay and every CSR value below is recomputed.
 */
#define SN65_MIPI_NODE DT_INST_PHANDLE(0, mipi_dsi)
#define SN65_CDC_NODE  DT_PHANDLE(SN65_MIPI_NODE, cdc_if)

#define SN65_PCLK_HZ     DT_PROP(SN65_CDC_NODE, clock_frequency)
#define SN65_LANE_BW_BPS DT_PROP(SN65_MIPI_NODE, panel_max_lane_bandwidth)

/*
 * The DSI clock LANE toggles at half the D-PHY bit rate (DDR): dsi_dw.c's
 * dw_calc_clocks() sets `phy->pll_fout = hs_bit_clk >> 1`, and clamps
 * hs_bit_clk to panel-max-lane-bandwidth when the burst-mode formula would
 * exceed it.  This shield's overlay sets panel-max-lane-bandwidth to the
 * actual achieved rate (see its header comment), so the clamp is exact:
 * the DSI clock lane the bridge receives is this bandwidth / 2.
 */
#define SN65_DSI_HS_CLK_HZ (SN65_LANE_BW_BPS / 2U)

/* CSR 0x0B[7:3] DSI_CLK_DIVIDER: register value v means "divide by v + 1". */
#define SN65_DSI_CLK_DIV_VAL   (DIV_ROUND_CLOSEST(SN65_DSI_HS_CLK_HZ, SN65_PCLK_HZ))
#define SN65_DSI_CLK_DIV_FIELD (SN65_DSI_CLK_DIV_VAL - 1U)

BUILD_ASSERT(SN65_DSI_CLK_DIV_VAL >= 1U && SN65_DSI_CLK_DIV_VAL <= 25U,
	     "sn65dsi83: panel-max-lane-bandwidth/2 vs the cdc-if clock-frequency do not form a "
	     "DSI_CLK_DIVIDER the bridge can express (CSR 0x0B, divide-by 1..25)");

/* The LVDS clock the divider actually reaches, for the range lookup + tolerance check below. */
#define SN65_LVDS_CLK_HZ (SN65_DSI_HS_CLK_HZ / SN65_DSI_CLK_DIV_VAL)

#define SN65_LVDS_CLK_ERR_HZ                                                                      \
	(SN65_LVDS_CLK_HZ > SN65_PCLK_HZ ? (SN65_LVDS_CLK_HZ - SN65_PCLK_HZ)                        \
					  : (SN65_PCLK_HZ - SN65_LVDS_CLK_HZ))

/*
 * The integer divider cannot always land the LVDS clock exactly on the CDC
 * pixel clock; the PLL itself only resolves to ~0.4 MHz steps in any case
 * (datasheet section 7.3.2), so require the divider to get within that
 * margin rather than demanding an exact match.
 */
BUILD_ASSERT(SN65_LVDS_CLK_ERR_HZ <= 400000U,
	     "sn65dsi83: DSI_CLK_DIVIDER cannot reach the cdc-if pixel clock within the PLL's "
	     "~0.4 MHz step -- re-check panel-max-lane-bandwidth vs clock-frequency in the "
	     "overlay");

/* CSR 0x0A[3:1] LVDS_CLK_RANGE (datasheet Table 7-5): 6 half-open 25 MHz-ish bins, 25..154 MHz. */
BUILD_ASSERT(SN65_LVDS_CLK_HZ >= 25000000U && SN65_LVDS_CLK_HZ <= 154000000U,
	     "sn65dsi83: LVDS output clock must fall inside 25..154 MHz (CSR 0x0A LVDS_CLK_RANGE)");

#define SN65_LVDS_CLK_RANGE                                                                        \
	(SN65_LVDS_CLK_HZ < 37500000U    ? 0U                                                       \
	 : SN65_LVDS_CLK_HZ < 62500000U  ? 1U                                                        \
	 : SN65_LVDS_CLK_HZ < 87500000U  ? 2U                                                        \
	 : SN65_LVDS_CLK_HZ < 112500000U ? 3U                                                        \
	 : SN65_LVDS_CLK_HZ < 137500000U ? 4U                                                        \
					  : 5U)

/*
 * CSR 0x12 CHA_DSI_CLK_RANGE: 5 MHz bins starting at 0x08=40 MHz, i.e. the
 * register value is simply the clock in whole 5 MHz units (integer division
 * truncates toward the bin's low edge, matching "0x08 - 40<=f<45 MHz").
 */
#define SN65_DSI_CLK_RANGE (SN65_DSI_HS_CLK_HZ / 5000000U)

BUILD_ASSERT(SN65_DSI_CLK_RANGE >= 8U && SN65_DSI_CLK_RANGE <= 100U,
	     "sn65dsi83: DSI clock lane must fall inside 40..500 MHz (CSR 0x12 CHA_DSI_CLK_RANGE)");

/* CSR 0x10[4:3] CHA_DSI_LANES: 00=4, 01=3, 10=2, 11=1 (i.e. field = 4 - lane count). */
#define SN65_DATA_LANES DT_INST_PROP(0, data_lanes)
#define SN65_LANE_FIELD (4U - SN65_DATA_LANES)

/* Reserved bits[6:5] of CSR 0x10 default to 0b01; keep them there per the datasheet note. */
#define SN65_DSI_LANE_REG (BIT(5) | (SN65_LANE_FIELD << 3))

#define SN65_PIXFMT DT_INST_PROP(0, pixel_format)

BUILD_ASSERT(SN65_PIXFMT == MIPI_DSI_PIXFMT_RGB666_PACKED || SN65_PIXFMT == MIPI_DSI_PIXFMT_RGB888,
	     "sn65dsi83: pixel-format must be MIPI_DSI_PIXFMT_RGB666_PACKED (18 bpp) or "
	     "MIPI_DSI_PIXFMT_RGB888 (24 bpp) -- the only two formats the bridge decodes");

#if SN65_PIXFMT == MIPI_DSI_PIXFMT_RGB888
/* Plan B (24 bpp, format 1: the 2 LSB per colour on the Y3 LVDS lane). */
#define SN65_LVDS_FMT_BPP_BITS (SN65_LVDS_FMT_24BPP_MODE | SN65_LVDS_FMT_24BPP_FORMAT1)
#else
/* 18 bpp: LVDS channel A lane 4 (Y3) disabled, format field don't-care. */
#define SN65_LVDS_FMT_BPP_BITS 0U
#endif

#define SN65_LVDS_FMT_REG                                                                          \
	(SN65_LVDS_FMT_DE_POS | SN65_LVDS_FMT_HS_VS_NEG | SN65_LVDS_FMT_RESERVED_BIT4 |             \
	 SN65_LVDS_FMT_BPP_BITS)

/*
 * The panel timing fields (datasheet Table 7-8): line length and the
 * back-porch/sync-pulse-width registers come straight off the cdc-if node
 * that already describes this exact panel timing to the CDC200 and the DSI
 * host.  CHA_VERTICAL_DISPLAY_SIZE / VBP / HFP / VFP are documented
 * "TEST PATTERN GENERATION PURPOSE ONLY" -- normal passthrough video derives
 * its own vertical/blanking timing from the incoming DSI stream -- but they
 * are still programmed from the same DT facts so the built-in test pattern
 * (CONFIG_SN65DSI83_TEST_PATTERN) reproduces the real panel timing.
 */
/* The generic display-controller.yaml binding names these width/height, not
 * hactive/vactive -- keep this file's constant names close to the datasheet
 * field they feed (CHA_ACTIVE_LINE_LENGTH / CHA_VERTICAL_DISPLAY_SIZE). */
#define SN65_HACTIVE DT_PROP(SN65_CDC_NODE, width)
#define SN65_VACTIVE DT_PROP(SN65_CDC_NODE, height)
#define SN65_HSYNC_LEN DT_PROP(SN65_CDC_NODE, hsync_len)
#define SN65_VSYNC_LEN DT_PROP(SN65_CDC_NODE, vsync_len)
#define SN65_HBACK_PORCH DT_PROP(SN65_CDC_NODE, hback_porch)
#define SN65_VBACK_PORCH DT_PROP(SN65_CDC_NODE, vback_porch)
#define SN65_HFRONT_PORCH DT_PROP(SN65_CDC_NODE, hfront_porch)
#define SN65_VFRONT_PORCH DT_PROP(SN65_CDC_NODE, vfront_porch)

BUILD_ASSERT(SN65_HACTIVE <= 0xFFFU, "sn65dsi83: CHA_ACTIVE_LINE_LENGTH is a 12-bit field");
BUILD_ASSERT(SN65_VACTIVE <= 0xFFFU, "sn65dsi83: CHA_VERTICAL_DISPLAY_SIZE is a 12-bit field");
BUILD_ASSERT(SN65_HSYNC_LEN <= 0x3FFU, "sn65dsi83: CHA_HSYNC_PULSE_WIDTH is a 10-bit field");
BUILD_ASSERT(SN65_VSYNC_LEN <= 0x3FFU, "sn65dsi83: CHA_VSYNC_PULSE_WIDTH is a 10-bit field");
BUILD_ASSERT(SN65_HBACK_PORCH <= 0xFFU, "sn65dsi83: CHA_HORIZONTAL_BACK_PORCH is an 8-bit field");
BUILD_ASSERT(SN65_VBACK_PORCH <= 0xFFU, "sn65dsi83: CHA_VERTICAL_BACK_PORCH is an 8-bit field");
BUILD_ASSERT(SN65_HFRONT_PORCH <= 0xFFU, "sn65dsi83: CHA_HORIZONTAL_FRONT_PORCH is an 8-bit field");
BUILD_ASSERT(SN65_VFRONT_PORCH <= 0xFFU, "sn65dsi83: CHA_VERTICAL_FRONT_PORCH is an 8-bit field");

/*
 * CHA_SYNC_DELAY: the datasheet requires >= 32 pixel clocks (Table 7-8) and
 * notes the bridge's own pipeline already adds ~10; there is no DT property
 * for it (it is a bridge-internal margin, not a panel fact), so use a fixed
 * value with a little headroom over the minimum.
 */
#define SN65_SYNC_DELAY 33U
BUILD_ASSERT(SN65_SYNC_DELAY >= 32U && SN65_SYNC_DELAY <= 0xFFFU,
	     "sn65dsi83: CHA_SYNC_DELAY must be >= 32 pixel clocks (datasheet Table 7-8) and fit "
	     "the 12-bit field");

struct sn65dsi83_config {
	struct i2c_dt_spec i2c;
	const struct device *mipi_dsi;
	struct gpio_dt_spec enable_gpio;
};

/*
 * Datasheet Table 7-4: "Addresses 0x08 - 0x00 = {0x01, 0x20, 0x20, 0x20,
 * 0x44, 0x53, 0x49, 0x38, 0x35}" -- worded high-address-first.  Read as a
 * 9-byte burst starting at 0x00 (ascending), the expected buffer is this
 * array.  BENCH-UNVERIFIED: the datasheet's descending phrasing versus an
 * ascending I2C burst read is exactly the kind of off-by-reversal that only
 * a real read-back settles -- treat a byte-reversed match on the bench as
 * "found the bridge, fix the constant" rather than "wrong bridge".
 */
static const uint8_t sn65dsi83_expected_id[SN65_REG_ID_LEN] = {
	0x01U, 0x20U, 0x20U, 0x20U, 0x44U, 0x53U, 0x49U, 0x38U, 0x35U,
};

static int sn65dsi83_check_id(const struct device *dev)
{
	const struct sn65dsi83_config *config = dev->config;
	uint8_t id[SN65_REG_ID_LEN];
	int ret;

	ret = i2c_burst_read_dt(&config->i2c, SN65_REG_ID_BASE, id, sizeof(id));
	if (ret != 0) {
		LOG_ERR("ID read failed (%d) -- EN high but the bridge did not answer I2C", ret);
		return ret;
	}

	if (memcmp(id, sn65dsi83_expected_id, sizeof(id)) != 0) {
		LOG_ERR("Unexpected ID: got %02x %02x %02x %02x %02x %02x %02x %02x %02x, "
			"want %02x %02x %02x %02x %02x %02x %02x %02x %02x",
			id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7], id[8],
			sn65dsi83_expected_id[0], sn65dsi83_expected_id[1], sn65dsi83_expected_id[2],
			sn65dsi83_expected_id[3], sn65dsi83_expected_id[4], sn65dsi83_expected_id[5],
			sn65dsi83_expected_id[6], sn65dsi83_expected_id[7],
			sn65dsi83_expected_id[8]);
		return -ENODEV;
	}

	return 0;
}

static int sn65dsi83_program_csr(const struct device *dev)
{
	const struct sn65dsi83_config *config = dev->config;
	const struct i2c_dt_spec *i2c = &config->i2c;
	int ret;

	/*
	 * Every one of these is a single-byte write; a table would save lines
	 * but would also hide which value is a fixed datasheet constant
	 * (SOFT_RESET-adjacent, format polarities) versus a DT-derived one
	 * (the clock/lane/timing values above) -- worth the repetition.
	 */
	static const struct {
		uint8_t reg;
		uint32_t val; /* uint32_t: several are compile-time expressions, not all fit uint8_t
				 statically, though every one is masked to a byte before use. */
	} csr[] = {
		{ SN65_REG_CLK_SRC, (SN65_LVDS_CLK_RANGE << 1) | 1U /* HS_CLK_SRC */ },
		{ SN65_REG_CLK_DIV, (SN65_DSI_CLK_DIV_FIELD << 3) /* REFCLK_MULTIPLIER=0 */ },
		{ SN65_REG_DSI_LANE, SN65_DSI_LANE_REG },
		{ 0x11U, 0x00U },                            /* CHA_DSI_DATA_EQ/CLK_EQ: no equalization */
		{ SN65_REG_DSI_CLK_RANGE, SN65_DSI_CLK_RANGE },
		{ SN65_REG_LVDS_FMT, SN65_LVDS_FMT_REG },
		{ SN65_REG_LINE_LEN_LOW, SN65_HACTIVE & 0xFFU },
		{ SN65_REG_LINE_LEN_HIGH, (SN65_HACTIVE >> 8) & 0x0FU },
		{ SN65_REG_VDISP_LOW, SN65_VACTIVE & 0xFFU },
		{ SN65_REG_VDISP_HIGH, (SN65_VACTIVE >> 8) & 0x0FU },
		{ SN65_REG_SYNC_DLY_LOW, SN65_SYNC_DELAY & 0xFFU },
		{ SN65_REG_SYNC_DLY_HIGH, (SN65_SYNC_DELAY >> 8) & 0x0FU },
		{ SN65_REG_HSYNC_PW_LOW, SN65_HSYNC_LEN & 0xFFU },
		{ SN65_REG_HSYNC_PW_HIGH, (SN65_HSYNC_LEN >> 8) & 0x03U },
		{ SN65_REG_VSYNC_PW_LOW, SN65_VSYNC_LEN & 0xFFU },
		{ SN65_REG_VSYNC_PW_HIGH, (SN65_VSYNC_LEN >> 8) & 0x03U },
		{ SN65_REG_HBP, SN65_HBACK_PORCH & 0xFFU },
		{ SN65_REG_VBP, SN65_VBACK_PORCH & 0xFFU },
		{ SN65_REG_HFP, SN65_HFRONT_PORCH & 0xFFU },
		{ SN65_REG_VFP, SN65_VFRONT_PORCH & 0xFFU },
		{ SN65_REG_TEST_PATTERN,
		  IS_ENABLED(CONFIG_SN65DSI83_TEST_PATTERN) ? SN65_TEST_PATTERN_EN : 0x00U },
	};

	for (size_t i = 0; i < ARRAY_SIZE(csr); i++) {
		ret = i2c_reg_write_byte_dt(i2c, csr[i].reg, (uint8_t)csr[i].val);
		if (ret != 0) {
			LOG_ERR("CSR 0x%02x write failed (%d)", csr[i].reg, ret);
			return ret;
		}
	}

	return 0;
}

static int sn65dsi83_pll_start(const struct device *dev)
{
	const struct sn65dsi83_config *config = dev->config;
	const struct i2c_dt_spec *i2c = &config->i2c;
	int ret;

	/* Datasheet init seq 6: set PLL_EN.  The input clock (the DSI clock lane,
	 * already HS since step 3 of sn65dsi83_init()) must already be stable. */
	ret = i2c_reg_write_byte_dt(i2c, SN65_REG_PLL_EN, SN65_PLL_EN_BIT);
	if (ret != 0) {
		LOG_ERR("PLL_EN write failed (%d)", ret);
		return ret;
	}

	/* Datasheet: "After PLL_EN_STAT = 1, wait at least 3 ms for PLL to lock."
	 * Poll instead of a fixed sleep so a genuinely unlocked PLL is reported. */
	for (int i = 0; i < 20; i++) {
		uint8_t clk_src;

		ret = i2c_reg_read_byte_dt(i2c, SN65_REG_CLK_SRC, &clk_src);
		if (ret != 0) {
			LOG_ERR("PLL_EN_STAT poll read failed (%d)", ret);
			return ret;
		}
		if (clk_src & SN65_CLK_SRC_PLL_EN_STAT) {
			break;
		}
		k_msleep(1);
		if (i == 19) {
			LOG_ERR("PLL did not lock within 20 ms (CSR 0x0A=0x%02x)", clk_src);
			return -ETIMEDOUT;
		}
	}
	/* The extra margin the datasheet's own "wait at least 3 ms" asks for. */
	k_msleep(3);

	/* Datasheet init seq 7: SOFT_RESET, then wait 10 ms. */
	ret = i2c_reg_write_byte_dt(i2c, SN65_REG_SOFT_RESET, SN65_SOFT_RESET_BIT);
	if (ret != 0) {
		LOG_ERR("SOFT_RESET write failed (%d)", ret);
		return ret;
	}
	k_msleep(10);

	/* Datasheet init seq 10: no IRQ pin wired on this adapter, so IRQ_EN stays
	 * off; clear whatever the reset/PLL-lock sequence latched into 0xE5. */
	ret = i2c_reg_write_byte_dt(i2c, SN65_REG_IRQ_EN, 0x00U);
	if (ret != 0) {
		LOG_ERR("IRQ_EN write failed (%d)", ret);
		return ret;
	}
	ret = i2c_reg_write_byte_dt(i2c, SN65_REG_ERR_STAT, 0xFFU);
	if (ret != 0) {
		LOG_ERR("Error-register clear failed (%d)", ret);
		return ret;
	}

	return 0;
}

static int sn65dsi83_init(const struct device *dev)
{
	const struct sn65dsi83_config *config = dev->config;
	struct mipi_dsi_device mdev = { 0 };
	int ret;

	if (!device_is_ready(config->i2c.bus)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}
	if (!device_is_ready(config->enable_gpio.port)) {
		LOG_ERR("EN GPIO controller not ready");
		return -ENODEV;
	}
	if (!device_is_ready(config->mipi_dsi)) {
		LOG_ERR("MIPI-DSI host not ready");
		return -ENODEV;
	}

	/* Step 1: EN low for >= 10 ms (datasheet init seq 3). */
	ret = gpio_pin_configure_dt(&config->enable_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		LOG_ERR("EN GPIO configure failed (%d)", ret);
		return ret;
	}
	k_msleep(10);

	/* Step 2: attach to the DSI host (configures it, leaves the clock lane LP). */
	mdev.data_lanes = SN65_DATA_LANES;
	mdev.pixfmt = SN65_PIXFMT;
	mdev.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST;
	/* mdev.timings left zeroed: dsi_dw_attach_locked() always overrides them
	 * from the cdc-if node -- see dsi_dw.c's own comment on why a panel/bridge
	 * driver need not (and on the real hx8394 driver, does not) fill this in. */

	ret = mipi_dsi_attach(config->mipi_dsi, SN65DSI83_DSI_CHANNEL, &mdev);
	if (ret != 0) {
		LOG_ERR("mipi_dsi_attach failed (%d)", ret);
		return ret;
	}

	/* Step 3: clock lane HS, continuous -- CDC200 is not enabled yet, so no
	 * pixel data reaches the wire; only the clock the bridge needs to see
	 * before EN goes high. */
	ret = dsi_dw_set_mode(config->mipi_dsi, DSI_DW_VIDEO_MODE);
	if (ret != 0) {
		LOG_ERR("dsi_dw_set_mode(VIDEO) failed (%d)", ret);
		return ret;
	}

	/* Step 4: EN high, wait 10 ms (datasheet init seq 4). */
	ret = gpio_pin_set_dt(&config->enable_gpio, 1);
	if (ret != 0) {
		LOG_ERR("EN GPIO set failed (%d)", ret);
		return ret;
	}
	k_msleep(10);

	/* Step 5: the bridge answers I2C for the first time -- confirm it is
	 * actually there before writing a single CSR into the dark. */
	ret = sn65dsi83_check_id(dev);
	if (ret != 0) {
		return ret;
	}

	/* Step 6: program the CSR bank (datasheet init seq 5). */
	ret = sn65dsi83_program_csr(dev);
	if (ret != 0) {
		return ret;
	}

	/* Step 7: PLL_EN, poll lock, SOFT_RESET, clear errors (init seq 6-10). */
	return sn65dsi83_pll_start(dev);
}

int sn65dsi83_read_errors(const struct device *dev, uint8_t *e5)
{
	const struct sn65dsi83_config *config;

	if (dev == NULL || e5 == NULL) {
		return -EINVAL;
	}

	config = dev->config;
	return i2c_reg_read_byte_dt(&config->i2c, SN65_REG_ERR_STAT, e5);
}

#define SN65DSI83_INIT(inst)                                                                        \
	static const struct sn65dsi83_config sn65dsi83_config_##inst = {                            \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                   \
		.mipi_dsi = DEVICE_DT_GET(DT_INST_PHANDLE(inst, mipi_dsi)),                          \
		.enable_gpio = GPIO_DT_SPEC_INST_GET(inst, enable_gpios),                            \
	};                                                                                            \
	DEVICE_DT_INST_DEFINE(inst, sn65dsi83_init, NULL, NULL, &sn65dsi83_config_##inst,           \
			       POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(SN65DSI83_INIT)
