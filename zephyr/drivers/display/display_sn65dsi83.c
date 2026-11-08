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
 * only to get itself into that transparent state at init -- and again, through
 * the same init sequence, if the bridge later resets itself to its defaults
 * (CONFIG_SN65DSI83_RECOVERY, sn65dsi83_recovery.h) -- which is why
 * `DEVICE_DT_INST_DEFINE()` below passes no display_driver_api.
 *
 * INIT ORDER (why this runs at POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, matching
 * upstream Zephyr's himax,hx8394 panel driver): by the time this device
 * initializes, the I2C bus, the EN pin's GPIO controller (a plain SoC GPIO on
 * this shield: P13_4, the mikroBUS INT pin, gpio13) and the DesignWare
 * MIPI-DSI host all need to be ready.  CONFIG_APPLICATION_INIT_PRIORITY (90, kernel/Kconfig.device) is
 * after the GPIO and I2C controllers (I2C bus priority, KERNEL_INIT_PRIORITY_DEVICE = 50,
 * drivers/i2c/Kconfig), the fixed regulators (REGULATOR_FIXED_INIT_PRIORITY
 * = 75, drivers/regulator/Kconfig.fixed) and the DSI host + CDC200
 * (MIPI_DSI_INIT_PRIORITY / DISPLAY_INIT_PRIORITY = 85, drivers/mipi_dsi/
 * Kconfig and drivers/display/Kconfig) -- all upstream Zephyr defaults, not
 * anything this shield's own Kconfig.defconfig sets.
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
 * NON-BURST, no MIPI_DSI_CLOCK_NON_CONTINUOUS: the LVDS pixel clock this
 * bridge outputs is DERIVED from the incoming DSI clock lane (HS_CLK_SRC = 1,
 * CSR 0x0A.0 below) -- if the host ever stopped that clock between packets
 * (the non-continuous-clock mode some panels use to save power), the
 * bridge's LVDS clock would stop with it and the panel would lose sync every
 * time.  Non-burst-sync-events is requested (not burst) because burst does
 * not fit this shield's link at all: burst RGB888 at this shield's 36.36 MHz
 * pixel clock needs 36.36e6 * 24 / 2 * 4 / 3 = 581.8 Mbps/lane (see
 * SN65_BURST_HS_CLK_HZ below), over the Ensemble E8 two-lane application-
 * note ceiling of 500 Mbps/lane -- non-burst is the only mode that fits.
 * This driver does not hardcode the choice: it requests plain
 * MIPI_DSI_MODE_VIDEO and lets the DSI host's own DT-declared
 * `dpi-video-mode` (see snps,designware-dsi.yaml) OR the burst/sync-pulse
 * flag in -- the same mechanism upstream's himax,hx8394 driver relies on
 * (zephyr/drivers/display/display_hx8394.c only ever sets
 * MIPI_DSI_MODE_VIDEO too).  Hardcoding MIPI_DSI_MODE_VIDEO_BURST here would
 * silently override a shield that asks for non-burst, because dsi_dw.c's
 * dsi_dw_attach() only ever ORs the host's flags onto whatever the
 * peripheral already set -- it never clears one.
 *
 * WARM BOOT (this core alone reset while the core that owns the bus runs; needs the
 * alp,i2c-handover alive-address, see sn65dsi83_boot_plan_for()): the sequence above is NOT
 * run.  EN stays high (low would reset a bridge the owner is watching), the I2C controller
 * is neither initialised nor used (the owner may be mid-transfer), the recipe magic and the
 * owner's counters are kept and the recipe is re-published unchanged.  Steps 2-3 still run:
 * the DSI host and the CDC200 need them.  Restarting the host's clock lane can cost the
 * bridge its PLL lock; the bus owner's recovery agent notices and replays the CSR table.
 *
 * BLANKING IS NOT SUPPORTED: an app must not call display_blanking_on() on
 * the CDC200 behind this bridge (the first blanking_off(), which starts
 * video, is the supported use).
 * ponytail: blanking_on() (cdc200_blanking_on -> dsi_dw_set_mode(COMMAND))
 * stops the HS clock lane, and this bridge's LVDS PLL is sourced from it
 * (HS_CLK_SRC=1) -- so a blanking_on/blanking_off cycle after this driver's
 * one-shot init almost certainly drops the bridge's PLL lock, and nothing
 * here re-runs PLL_EN/SOFT_RESET on the way back to video mode.  Not
 * implemented: no re-lock helper exists in this driver, and no re-init hook
 * is wired to cdc200's blanking_off.  Upgrade path if a real app needs
 * blanking: add one, following the datasheet's Section 8.1.1 video STOP/
 * restart sequence (PLL_EN 0 -> 1, wait >= 3 ms, then SOFT_RESET), and call
 * it before display_blanking_off(), or wire a blanking-aware hook once this
 * is bench-verified.
 */

#define DT_DRV_COMPAT ti_sn65dsi83

#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display/sn65dsi83.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/mipi_dsi.h>
#include <zephyr/init.h>
#include <zephyr/drivers/mipi_dsi/dsi_dw.h>
#include <zephyr/dt-bindings/mipi_dsi/mipi_dsi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "sn65dsi83_recovery.h"

#ifdef CONFIG_ALIF_I2C_HANDOVER
#include "../../soc-bridge/alif/i2c_handover.h"
#define SN65_BOOT_IS_WARM() alp_i2c_handover_warm_boot()
#else
#define SN65_BOOT_IS_WARM() false
#endif

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

/*
 * A core that releases this bridge's I2C bus (alp,i2c-handover) may not touch the bridge
 * afterwards, so its recovery has to run on the core that gets the bus: the recipe address
 * says where this driver publishes the table that core replays.  A handover without it
 * would leave the bridge unwatched.
 */
#define SN65_HANDOVER_NEEDS_RECIPE(node) \
	BUILD_ASSERT(!(DT_ENUM_HAS_VALUE(node, role, release) && \
	               COND_CODE_1(DT_NODE_HAS_PROP(node, bus), \
	                           (DT_SAME_NODE(DT_PROP(node, bus), DT_INST_BUS(0))), \
	                           (0))) || \
	                 DT_INST_NODE_HAS_PROP(0, recovery_recipe_address), \
	             "ti,sn65dsi83: an alp,i2c-handover release node hands this bridge's I2C bus to " \
	             "another core, so recovery-recipe-address is required");
DT_FOREACH_STATUS_OKAY(alp_i2c_handover, SN65_HANDOVER_NEEDS_RECIPE)

/* CSR addresses (datasheet Tables 7-4..7-9). */
#define SN65_REG_CLK_DIV 0x0BU /* DSI_CLK_DIVIDER[7:3], REFCLK_MULTIPLIER[1:0]. */
#define SN65_REG_DSI_LANE \
	0x10U /* bit7/6:5 reserved (default 0/01), CHA_DSI_LANES[4:3] (Table 7-6). */
#define SN65_REG_DSI_CLK_RANGE 0x12U
#define SN65_REG_LVDS_FMT      0x18U
#define SN65_REG_LINE_LEN_LOW  0x20U
#define SN65_REG_LINE_LEN_HIGH 0x21U
#define SN65_REG_VDISP_LOW     0x24U /* test-pattern only. */
#define SN65_REG_VDISP_HIGH    0x25U /* test-pattern only. */
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

/* CSR 0x18 (datasheet Table 7-7). */
#define SN65_LVDS_FMT_DE_POS    0U                /* bit7=0: DE positive (default). */
#define SN65_LVDS_FMT_HS_VS_NEG (BIT(6) | BIT(5)) /* HS_NEG_POLARITY, VS_NEG_POLARITY (default). */
/*
 * bit4: Table 7-7 documents this as reserved, default 1, "must remain at
 * default"; the datasheet's own Section 8.2.2.1 example script names the
 * same bit LVDS_LINK_CFG (1 = single link) instead.  This bridge only ever
 * does single-link (it has no channel B), so the two descriptions agree on
 * the value that must be written here either way: 1.
 */
#define SN65_LVDS_FMT_LINK_CFG_SINGLE BIT(4)
#define SN65_LVDS_FMT_24BPP_MODE \
	BIT(3) /* 1 = 24 bpp (Y3 lane enabled), 0 = 18 bpp (Y3 disabled). */
/*
 * CHA_24BPP_FORMAT1: which half of each 8-bit colour rides the Y3 lane when
 * 24BPP_MODE=1. 0 (Format 2, the default) = the 2 MSB per colour on Y3 --
 * this is VESA-24 (datasheet Figure 7-5), what a VESA-24 panel expects.
 * 1 (Format 1) = the 2 LSB per colour on Y3 -- JEIDA, a DIFFERENT wire
 * format; using it against a VESA-24 panel puts the wrong 2 bits on Y3
 * (CSR 0x18 = 0x7A instead of the correct 0x78 for this panel).
 * Not used for this shield's panel (Format 1 stays 0 below); kept named for
 * the day a JEIDA panel needs it.
 */
#define SN65_LVDS_FMT_24BPP_FORMAT1_JEIDA BIT(1)

/* CSR 0x3C. */
#define SN65_TEST_PATTERN_EN BIT(4)

/*
 * DT-derived clock/timing constants.  These come from the panel's DSI
 * neighbours -- the cdc-if (CDC200) node's pixel clock and timings, and the
 * DSI host's declared lane bandwidth/lane count/video mode -- not from any
 * property of THIS node, because the datasheet's register values are all
 * functions of the link the bridge sits on, and that link is described
 * once, in the DSI host subtree.  DT stays the single source of truth:
 * change the panel timings, lane count or dpi-video-mode in the overlay and
 * every CSR value below is recomputed.
 */
#define SN65_MIPI_NODE DT_INST_PHANDLE(0, mipi_dsi)
#define SN65_CDC_NODE  DT_PHANDLE(SN65_MIPI_NODE, cdc_if)

#define SN65_PCLK_HZ     DT_PROP(SN65_CDC_NODE, clock_frequency)
#define SN65_LANE_BW_BPS DT_PROP(SN65_MIPI_NODE, panel_max_lane_bandwidth)

/* CSR 0x10[4:3] CHA_DSI_LANES: 00=4, 01=3, 10=2, 11=1 (i.e. field = 4 - lane count). */
#define SN65_DATA_LANES DT_INST_PROP(0, data_lanes)
#define SN65_LANE_FIELD (4U - SN65_DATA_LANES)

/* CSR 0x10 bits6:5 reserved, default 01 (bit7 reserved default 0 is simply
 * left unset below) -- see SN65_REG_DSI_LANE's comment for the datasheet
 * table this comes from. */
#define SN65_DSI_LANE_REG (BIT(5) | (SN65_LANE_FIELD << 3))

#define SN65_PIXFMT DT_INST_PROP(0, pixel_format)

BUILD_ASSERT(SN65_PIXFMT == MIPI_DSI_PIXFMT_RGB666_PACKED || SN65_PIXFMT == MIPI_DSI_PIXFMT_RGB888,
             "sn65dsi83: pixel-format must be MIPI_DSI_PIXFMT_RGB666_PACKED (18 bpp) or "
             "MIPI_DSI_PIXFMT_RGB888 (24 bpp) -- the only two formats the bridge decodes");

/* Bits per pixel the DSI LINK carries -- a property of the link format
 * above, not of the CDC's framebuffer (pixel-fmt-l1 in the overlay). */
#define SN65_BPP (SN65_PIXFMT == MIPI_DSI_PIXFMT_RGB888 ? 24U : 18U)

#if SN65_PIXFMT == MIPI_DSI_PIXFMT_RGB888
/*
 * 24 bpp, Format 2 (FORMAT1 left at its 0 default) -- VESA-24, this
 * shield's panel wiring (Figure 7-5).  NOT FORMAT1=1: that is JEIDA, a
 * different bit assignment on the Y3 lane (Figure 7-6) that this panel does
 * not use -- see SN65_LVDS_FMT_24BPP_FORMAT1_JEIDA's comment above.
 */
#define SN65_LVDS_FMT_BPP_BITS (SN65_LVDS_FMT_24BPP_MODE)
#else
/* 18 bpp (Figure 7-4): LVDS channel A lane 4 (Y3) disabled, FORMAT1 don't-care. */
#define SN65_LVDS_FMT_BPP_BITS 0U
#endif

#define SN65_LVDS_FMT_REG \
	(SN65_LVDS_FMT_DE_POS | SN65_LVDS_FMT_HS_VS_NEG | SN65_LVDS_FMT_LINK_CFG_SINGLE | \
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
#define SN65_HACTIVE      DT_PROP(SN65_CDC_NODE, width)
#define SN65_VACTIVE      DT_PROP(SN65_CDC_NODE, height)
#define SN65_HSYNC_LEN    DT_PROP(SN65_CDC_NODE, hsync_len)
#define SN65_VSYNC_LEN    DT_PROP(SN65_CDC_NODE, vsync_len)
#define SN65_HBACK_PORCH  DT_PROP(SN65_CDC_NODE, hback_porch)
#define SN65_VBACK_PORCH  DT_PROP(SN65_CDC_NODE, vback_porch)
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
 * The DSI clock LANE (what this bridge's HS_CLK_SRC=1 actually measures,
 * CSR 0x0A.0) the CSR bank below is tuned against.  dsi_dw.c's
 * dw_calc_clocks() computes the D-PHY bit rate two DIFFERENT ways depending
 * on the shield's dpi-video-mode, then clamps the result to
 * panel-max-lane-bandwidth either way -- panel-max-lane-bandwidth/2 equals
 * the actual rate ONLY for a burst config deliberately tuned to land exactly
 * on that clamp; it is not the rate source in general.  For non-burst (this
 * shield -- see &mipi_dsi's dpi-video-mode in the overlay) the real rate is
 * whatever the non-burst formula computes; the clamp is a ceiling that must
 * sit ABOVE it, not the rate source -- a clamp below the non-burst need
 * starves the DPI payload FIFO every line (dsi_dw.c's own comment: INT_ST1
 * DPI_PLD_WR_ERR), it does not just fail to reach a target.
 *
 * Both formulas are dsi_dw.c's own (dw_calc_clocks()), replicated here in
 * integer arithmetic -- the driver's own math is double-precision float, at
 * runtime; this is preprocessor-evaluated, so being ~1 part in a million off
 * is expected and is well inside the ~0.4 MHz D-PHY PLL step tolerance
 * already built into the BUILD_ASSERTs below:
 *
 *   burst:     pclk * bpp * 4/3 / lanes, clamped to panel-max-lane-bandwidth
 *   non-burst: (pkt_size*bpp/8 + 12) * pclk * 8 / (pkt_size * lanes)
 *              + (sync_pulse ? 64 : 32) * pclk / (lanes * hactive)
 *
 * dpi-video-mode's enum order matches dsi_dw.c's own DSI_DW_MODE_FLAGS_OR:
 * 0 non-burst-sync-events, 1 non-burst-sync-pulses, 2 burst.
 */
/* vid-pkt-size defaults to hactive when unset in DT, matching dsi_dw.c's own
 * COND_CODE_1(DT_INST_NODE_HAS_PROP(i, vid_pkt_size), ...) fallback to the
 * cdc-if node's width property (dsi_dw.c ~1751) -- SN65_HACTIVE below is that
 * same width property. */
#define SN65_VID_PKT_SIZE   DT_PROP_OR(SN65_MIPI_NODE, vid_pkt_size, SN65_HACTIVE)
#define SN65_DSI_MODE_IDX   DT_ENUM_IDX(SN65_MIPI_NODE, dpi_video_mode)
#define SN65_DSI_BURST      (SN65_DSI_MODE_IDX == 2)
#define SN65_DSI_SYNC_PULSE (SN65_DSI_MODE_IDX == 1)

#define SN65_BURST_HS_CLK_HZ \
	MIN(((uint64_t)SN65_PCLK_HZ * SN65_BPP * 4U) / (SN65_DATA_LANES * 3U), SN65_LANE_BW_BPS)

#define SN65_NONBURST_HS_CLK_HZ \
	((((uint64_t)SN65_VID_PKT_SIZE * SN65_BPP / 8U + 12U) * SN65_PCLK_HZ * 8U) / \
	     ((uint64_t)SN65_VID_PKT_SIZE * SN65_DATA_LANES) + \
	 ((SN65_DSI_SYNC_PULSE ? 64ULL : 32ULL) * SN65_PCLK_HZ) / \
	     ((uint64_t)SN65_DATA_LANES * SN65_HACTIVE))

#define SN65_HS_BIT_CLK_HZ (SN65_DSI_BURST ? SN65_BURST_HS_CLK_HZ : SN65_NONBURST_HS_CLK_HZ)

/* Guarded by !SN65_DSI_BURST: in burst mode SN65_NONBURST_HS_CLK_HZ is not
 * the rate this link actually uses (SN65_HS_BIT_CLK_HZ picks the burst
 * formula instead), so it must not gate a burst config that never needs it
 * to hold. */
BUILD_ASSERT(!SN65_DSI_BURST || SN65_LANE_BW_BPS >= SN65_NONBURST_HS_CLK_HZ,
             "sn65dsi83: panel-max-lane-bandwidth must be >= the non-burst formula's actual bit "
             "rate, or the DSI host's own clamp (applied regardless of mode) silently slows the "
             "link below what this CSR bank is tuned for");

/* The DSI clock LANE toggles at half the D-PHY bit rate (DDR): dsi_dw.c's
 * dw_calc_clocks() sets `phy->pll_fout = hs_bit_clk >> 1`. */
#define SN65_DSI_HS_CLK_HZ (SN65_HS_BIT_CLK_HZ / 2U)

/* CSR 0x0B[7:3] DSI_CLK_DIVIDER: register value v means "divide by v + 1". */
#define SN65_DSI_CLK_DIV_VAL   (DIV_ROUND_CLOSEST(SN65_DSI_HS_CLK_HZ, SN65_PCLK_HZ))
#define SN65_DSI_CLK_DIV_FIELD (SN65_DSI_CLK_DIV_VAL - 1U)

BUILD_ASSERT(SN65_DSI_CLK_DIV_VAL >= 1U && SN65_DSI_CLK_DIV_VAL <= 25U,
             "sn65dsi83: the computed DSI clock lane vs the cdc-if clock-frequency do not form a "
             "DSI_CLK_DIVIDER the bridge can express (CSR 0x0B, divide-by 1..25)");

/* The LVDS clock the divider actually reaches, for the range lookup + tolerance check below. */
#define SN65_LVDS_CLK_HZ (SN65_DSI_HS_CLK_HZ / SN65_DSI_CLK_DIV_VAL)

#define SN65_LVDS_CLK_ERR_HZ \
	(SN65_LVDS_CLK_HZ > SN65_PCLK_HZ ? (SN65_LVDS_CLK_HZ - SN65_PCLK_HZ) \
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

#define SN65_LVDS_CLK_RANGE \
	(SN65_LVDS_CLK_HZ < 37500000U    ? 0U \
	 : SN65_LVDS_CLK_HZ < 62500000U  ? 1U \
	 : SN65_LVDS_CLK_HZ < 87500000U  ? 2U \
	 : SN65_LVDS_CLK_HZ < 112500000U ? 3U \
	 : SN65_LVDS_CLK_HZ < 137500000U ? 4U \
	                                 : 5U)

/*
 * CSR 0x12 CHA_DSI_CLK_RANGE: 5 MHz bins starting at 0x08=40 MHz, i.e. the
 * register value is simply the clock in whole 5 MHz units (integer division
 * truncates toward the bin's low edge, matching "0x08 - 40<=f<45 MHz").
 */
#define SN65_DSI_CLK_RANGE (SN65_DSI_HS_CLK_HZ / 5000000U)

BUILD_ASSERT(SN65_DSI_CLK_RANGE >= 8U && SN65_DSI_CLK_RANGE <= 100U,
             "sn65dsi83: DSI clock lane must fall inside 40..500 MHz (CSR 0x12 CHA_DSI_CLK_RANGE)");

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
	struct i2c_dt_spec       i2c;
	const struct device     *mipi_dsi;
	struct gpio_dt_spec      enable_gpio;
	struct sn65dsi83_recipe *recipe; /* NULL: this core keeps the bus and polls the bridge itself */
};

struct sn65dsi83_data {
	struct k_work_delayable health_work;
	struct sn65dsi83_stats  stats;
};

/*
 * The CSR bank, written at init and replayed verbatim by a recovery
 * (sn65dsi83_recovery.c).  A table would hide which value is a fixed datasheet
 * constant (format polarities) versus a DT-derived one (the clock/lane/timing
 * values above) -- the comments say which.
 */
#define SN65_CSR(r, v) { (r), (uint8_t)(v) }

static const struct sn65dsi83_csr sn65dsi83_csr[] = {
	SN65_CSR(SN65_REG_CLK_SRC, (SN65_LVDS_CLK_RANGE << 1) | 1U /* HS_CLK_SRC */),
	SN65_CSR(SN65_REG_CLK_DIV, (SN65_DSI_CLK_DIV_FIELD << 3) /* REFCLK_MULTIPLIER=0 */),
	SN65_CSR(SN65_REG_DSI_LANE, SN65_DSI_LANE_REG),
	SN65_CSR(0x11U, 0x00U), /* CHA_DSI_DATA_EQ/CLK_EQ: no equalization */
	SN65_CSR(SN65_REG_DSI_CLK_RANGE, SN65_DSI_CLK_RANGE),
	SN65_CSR(SN65_REG_LVDS_FMT, SN65_LVDS_FMT_REG),
	SN65_CSR(SN65_REG_LINE_LEN_LOW, SN65_HACTIVE & 0xFFU),
	SN65_CSR(SN65_REG_LINE_LEN_HIGH, (SN65_HACTIVE >> 8) & 0x0FU),
	SN65_CSR(SN65_REG_VDISP_LOW, SN65_VACTIVE & 0xFFU),
	SN65_CSR(SN65_REG_VDISP_HIGH, (SN65_VACTIVE >> 8) & 0x0FU),
	SN65_CSR(SN65_REG_SYNC_DLY_LOW, SN65_SYNC_DELAY & 0xFFU),
	SN65_CSR(SN65_REG_SYNC_DLY_HIGH, (SN65_SYNC_DELAY >> 8) & 0x0FU),
	SN65_CSR(SN65_REG_HSYNC_PW_LOW, SN65_HSYNC_LEN & 0xFFU),
	SN65_CSR(SN65_REG_HSYNC_PW_HIGH, (SN65_HSYNC_LEN >> 8) & 0x03U),
	SN65_CSR(SN65_REG_VSYNC_PW_LOW, SN65_VSYNC_LEN & 0xFFU),
	SN65_CSR(SN65_REG_VSYNC_PW_HIGH, (SN65_VSYNC_LEN >> 8) & 0x03U),
	SN65_CSR(SN65_REG_HBP, SN65_HBACK_PORCH & 0xFFU),
	SN65_CSR(SN65_REG_VBP, SN65_VBACK_PORCH & 0xFFU),
	SN65_CSR(SN65_REG_HFP, SN65_HFRONT_PORCH & 0xFFU),
	SN65_CSR(SN65_REG_VFP, SN65_VFRONT_PORCH & 0xFFU),
	SN65_CSR(SN65_REG_TEST_PATTERN,
	         IS_ENABLED(CONFIG_SN65DSI83_TEST_PATTERN) ? SN65_TEST_PATTERN_EN : 0x00U),
};

BUILD_ASSERT(ARRAY_SIZE(sn65dsi83_csr) <= SN65_RECIPE_MAX,
             "sn65dsi83: the CSR table no longer fits the shared recovery recipe");

#ifdef CONFIG_SN65DSI83_RECOVERY
/* The bridge's health pass on the core that owns its I2C bus; serialised with every other
 * user of the bus by the I2C driver (one core, one driver instance).  Reported with printk:
 * the apps this runs in keep CONFIG_LOG off, so a LOG_WRN here would never be seen. */
static void sn65dsi83_health_work(struct k_work *work)
{
	struct k_work_delayable       *dwork  = k_work_delayable_from_work(work);
	struct sn65dsi83_data         *data   = CONTAINER_OF(dwork, struct sn65dsi83_data, health_work);
	const struct sn65dsi83_config *config = DEVICE_DT_INST_GET(0)->config;
	struct sn65dsi83_report        rep;

	if (sn65dsi83_health_poll(&config->i2c,
	                          sn65dsi83_csr,
	                          ARRAY_SIZE(sn65dsi83_csr),
	                          &data->stats,
	                          &rep,
	                          k_uptime_get()) == 0) {
		sn65dsi83_report_print(&rep, &data->stats);
	}

	k_work_reschedule(dwork, K_MSEC(CONFIG_SN65DSI83_RECOVERY_INTERVAL_MS));
}

/* Called as soon as the bridge has answered its ID check, BEFORE the CSR bank is written: a
 * boot init that then fails half-way is recoverable by the same replay.  A warm boot calls it
 * without the check: the recipe is a compile-time constant, publishing it again changes nothing. */
static void sn65dsi83_arm_recovery(const struct device *dev)
{
	const struct sn65dsi83_config *config = dev->config;

	if (config->recipe != NULL) {
		/* The bus goes to another core after this init (alp,i2c-handover): hand it
		 * the table; it polls, this core never touches the bridge again. */
		struct sn65dsi83_recipe *r = config->recipe;

		for (size_t i = 0; i < ARRAY_SIZE(sn65dsi83_csr); i++) {
			r->csr[i] = sn65dsi83_csr[i];
		}
		r->n = ARRAY_SIZE(sn65dsi83_csr);
		barrier_dmem_fence_full();
		r->magic = SN65_RECIPE_MAGIC;
		barrier_dmem_fence_full();
	} else {
		struct sn65dsi83_data *data = dev->data;

		k_work_init_delayable(&data->health_work, sn65dsi83_health_work);
		k_work_schedule(&data->health_work, K_MSEC(CONFIG_SN65DSI83_RECOVERY_INTERVAL_MS));
	}
}
#else
static inline void sn65dsi83_arm_recovery(const struct device *dev)
{
	ARG_UNUSED(dev);
}
#endif /* CONFIG_SN65DSI83_RECOVERY */

static int sn65dsi83_init(const struct device *dev)
{
	const struct sn65dsi83_config *config = dev->config;
	struct mipi_dsi_device         mdev   = { 0 };
	const struct sn65dsi83_boot_plan plan =
	    sn65dsi83_boot_plan_for(SN65_BOOT_IS_WARM(), config->recipe != NULL);
	int ret;

	/* A warm boot never looks at the bus: it is not initialised on this core and belongs to
	 * the core that took it (alp,i2c-handover). */
	if (plan.touch_bus && !device_is_ready(config->i2c.bus)) {
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

	if (plan.clear_recipe) {
		/* A recipe and counters left in SRAM by a previous boot are not ours. */
		config->recipe->magic          = 0U;
		config->recipe->recoveries     = 0U;
		config->recipe->failures       = 0U;
		config->recipe->errors_cleared = 0U;
		barrier_dmem_fence_full();
	}

	if (plan.toggle_en) {
		/* Step 1: EN low for >= 10 ms (datasheet init seq 3). */
		ret = gpio_pin_configure_dt(&config->enable_gpio, GPIO_OUTPUT_INACTIVE);
		if (ret != 0) {
			LOG_ERR("EN GPIO configure failed (%d)", ret);
			return ret;
		}
		k_msleep(10);
	} else {
		/* Warm boot: EN stays high (the bridge keeps its configuration); the pin is only
		 * claimed as an output at its current level. */
		ret = gpio_pin_configure_dt(&config->enable_gpio, GPIO_OUTPUT_ACTIVE);
		if (ret != 0) {
			LOG_ERR("EN GPIO configure failed (%d)", ret);
			return ret;
		}
	}

	/* Step 2: attach to the DSI host (configures it, leaves the clock lane LP). */
	mdev.data_lanes = SN65_DATA_LANES;
	mdev.pixfmt     = SN65_PIXFMT;
	/*
	 * Deliberately NOT | MIPI_DSI_MODE_VIDEO_BURST (or any sync-pulse flag)
	 * here: dsi_dw_attach()'s eff_mdev.mode_flags |= config->mode_flags_or
	 * only ever ORs the DSI host's own DT-declared dpi-video-mode flags ONTO
	 * whatever this struct already carries -- it never clears one a
	 * peripheral driver hardcoded.  A hardcoded BURST here would survive
	 * even on a shield whose &mipi_dsi sets dpi-video-mode =
	 * "non-burst-sync-events" (this one does; see the overlay), silently
	 * putting the host in the wrong mode.  Passing plain MIPI_DSI_MODE_VIDEO
	 * and letting the host's own mode_flags_or supply the rest is the same
	 * pattern upstream's himax,hx8394 driver uses (see the property's own
	 * doc in snps,designware-dsi.yaml).
	 */
	mdev.mode_flags = MIPI_DSI_MODE_VIDEO;
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

	if (!plan.touch_bus) {
		/* Warm boot: the bus owner replays the CSRs if the host restart cost the bridge its
		 * PLL lock; this core only keeps the owner's recipe valid. */
		sn65dsi83_arm_recovery(dev);
		printk("sn65dsi83: warm boot, bridge and I2C bus left to the bus owner\n");
		return 0;
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
	ret = sn65dsi83_check_id(&config->i2c);
	if (ret != 0) {
		return ret;
	}

	/* From here a failure is recoverable by replaying the init: arm that first. */
	sn65dsi83_arm_recovery(dev);

	/* Step 6: program the CSR bank (datasheet init seq 5). */
	ret = sn65dsi83_csr_write(&config->i2c, sn65dsi83_csr, ARRAY_SIZE(sn65dsi83_csr));
	if (ret != 0) {
		return ret;
	}

	/* Step 7: PLL_EN, poll lock, SOFT_RESET, clear errors (init seq 6-10). */
	return sn65dsi83_pll_start(&config->i2c);
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

uint32_t sn65dsi83_recovery_count(const struct device *dev)
{
	const struct sn65dsi83_config *config;

	if (dev == NULL) {
		return 0;
	}

	config = dev->config;
	if (config->recipe != NULL) {
		return config->recipe->recoveries; /* counted by the core that owns the bus */
	}
	return ((const struct sn65dsi83_data *)dev->data)->stats.recoveries;
}

/* The bridge is driven through dsi_dw_set_mode(), which only the DesignWare DSI host provides. */
#define SN65DSI83_ASSERT_HOST(inst) \
	BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_INST_PHANDLE(inst, mipi_dsi), snps_designware_dsi), \
	             "ti,sn65dsi83 mipi-dsi must point at a snps,designware-dsi host");
DT_INST_FOREACH_STATUS_OKAY(SN65DSI83_ASSERT_HOST)

#define SN65DSI83_RECIPE(inst) \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, recovery_recipe_address), \
	            ((struct sn65dsi83_recipe *)DT_INST_PROP(inst, recovery_recipe_address)), \
	            (NULL))

#define SN65DSI83_INIT(inst) \
	static struct sn65dsi83_data         sn65dsi83_data_##inst; \
	static const struct sn65dsi83_config sn65dsi83_config_##inst = { \
		.i2c         = I2C_DT_SPEC_INST_GET(inst), \
		.mipi_dsi    = DEVICE_DT_GET(DT_INST_PHANDLE(inst, mipi_dsi)), \
		.enable_gpio = GPIO_DT_SPEC_INST_GET(inst, enable_gpios), \
		.recipe      = SN65DSI83_RECIPE(inst), \
	}; \
	DEVICE_DT_INST_DEFINE(inst, \
	                      sn65dsi83_init, \
	                      NULL, \
	                      &sn65dsi83_data_##inst, \
	                      &sn65dsi83_config_##inst, \
	                      POST_KERNEL, \
	                      CONFIG_APPLICATION_INIT_PRIORITY, \
	                      NULL);

DT_INST_FOREACH_STATUS_OKAY(SN65DSI83_INIT)

/*
 * A bridge on a deferred-init bus must itself be deferred (Zephyr's build-time init-priority check:
 * "non-deferred device depends on deferred device"), and the bus is deferred so that a warm boot of
 * this core can leave a controller the other core is using untouched (alp,i2c-handover alive-address).
 * The driver then starts itself from a SYS_INIT at APPLICATION level, priority 90 (a deferred device
 * is not part of the POST_KERNEL 90 slot the non-deferred one takes; APPLICATION comes after every
 * POST_KERNEL driver, so the DSI host, the CDC200 and the GPIO controller are up, and before the
 * handover release at APPLICATION 99).  The bus is up by then on a cold boot (the handover glue
 * initialises it at POST_KERNEL 49), and a warm boot's sn65dsi83_init() never opens it.  The
 * priority has to be a literal (SYS_INIT pastes it).
 */
#define SN65_START_PRIO 90
BUILD_ASSERT(CONFIG_APPLICATION_INIT_PRIORITY == SN65_START_PRIO,
             "display_sn65dsi83.c: SN65_START_PRIO must equal CONFIG_APPLICATION_INIT_PRIORITY");

#define SN65DSI83_DEFERRED_START(inst) \
	COND_CODE_1(DT_INST_PROP_OR(inst, zephyr_deferred_init, 0), \
	            (static int sn65dsi83_start_##inst(void) \
	             { \
		             return device_init(DEVICE_DT_INST_GET(inst)); \
	             } SYS_INIT(sn65dsi83_start_##inst, APPLICATION, SN65_START_PRIO);), \
	            ())
DT_INST_FOREACH_STATUS_OKAY(SN65DSI83_DEFERRED_START)
