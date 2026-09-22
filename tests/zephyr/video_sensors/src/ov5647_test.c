/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Runtime ztest locking in the OV5647 lane-park fix (issue #2248, the AUTHORIZED LOCAL DIVERGENCE
 * recorded at the top of zephyr/drivers/video/ov5647.c) against the I2C emulator in
 * ov5647_emul.c, on native_sim's real "zephyr,i2c-emul-controller" (&i2c0 -- see app.overlay).
 *
 * SCOPE has grown past lane-park alone (issue #2248's several bench-driven fix-up rounds): this
 * suite also covers the PLL + MIPI-TX pad-drive init (AUTHORIZED LOCAL DIVERGENCE #2), the
 * full-FOV binned 640x480 mode, its ordering trap against the crop path, the common init, and
 * per-mode HTS/frame-rate (AUTHORIZED LOCAL DIVERGENCE #3). OV5647_PIXEL_RATE IS now indirectly
 * pinned here: the VTS assertions below only match their expected register values because
 * cfg->pixel_rate resolves to 58333333 (OV5647_PLL_MULT = 70 at 25 MHz XVCLK) -- a regression to
 * a different PLL_MULT changes the computed VTS and fails those checks, even though no test reads
 * VIDEO_CID_PIXEL_RATE directly.
 *
 * ov5647.c's header says: delete this vendored driver the moment the alp-sdk Zephyr pin advances
 * to a revision containing upstream PR #119301 -- BUT NOT BEFORE the lane-park fix is confirmed
 * either re-applied to the upstream-derived driver or already present in it. This suite is the
 * mechanical half of that check: it fails if a future re-backport silently drops the fix.
 */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/video.h>
#include <zephyr/ztest.h>

#include "ov5647_emul.h"

#define OV5647_NODE DT_NODELABEL(ov5647_test)

/* Register addresses this test peeks/drives -- kept local rather than pulling ov5647.c's private
 * OV5647_REG8()-wrapped values, which also encode CCI address/data-size metadata this test
 * doesn't need. These four are the park set; see ov5647.c's OV5647_MIPI_CTRL00 comment. */
#define REG_MODE_SELECT   0x0100
#define REG_MIPI_CTRL00   0x4800
#define REG_FRAME_OFF_NUM 0x4202
#define REG_PAD_OUT       0x300d

/* The three PLL registers of AUTHORIZED LOCAL DIVERGENCE #2's eight-register init set (issue
 * #2248, bench run 52) -- kept local for the same reason as the quartet above. */
#define REG_SC_PLL_CTRL3      0x3037 /* prediv */
#define REG_SC_PLL_MULTIPLIER 0x3036
#define REG_SC_PLL_CTRL1      0x3035 /* sysdiv, upper nibble */

#define MODE_SELECT_RUNNING 0x01

#define MIPI_CTRL00_PARKED      0x25 /* CLOCK_LANE_GATE | BUS_IDLE | CLOCK_LANE_DISABLE */
#define MIPI_CTRL00_STREAMING   0x04 /* BUS_IDLE only -- continuous clock while streaming */
#define FRAME_OFF_NUM_PARKED    0x0f
#define FRAME_OFF_NUM_STREAMING 0x00
#define PAD_OUT_PARKED          0x01
#define PAD_OUT_STREAMING       0x00

/* Bench-proven values from ov5647_init_regs[] (OV5647_PLL_PREDIV/MULT/SYS_DIV in ov5647.c).
 * MULT is 70 (0x46), not the earlier 105 -- AUTHORIZED LOCAL DIVERGENCE #3, bench run 61
 * (RPi/OmniVision reference PLL for the 640x480 10bpp mode; see ov5647.c's PLL constants
 * comment). */
#define SC_PLL_CTRL3_PREDIV    3
#define SC_PLL_MULTIPLIER_MULT 70
#define SC_PLL_CTRL1_SYS_DIV_2 0x21

/* AUTHORIZED LOCAL DIVERGENCE #3 (issue #2248, bench runs 56/58/60/61 -- run 61, the
 * RPi/OmniVision reference values, is current) -- the full-FOV binned 640x480 mode, its ordering
 * trap against the crop path, the per-mode HTS, and the common init. Kept local for the same
 * reason as the quartet/PLL trio above. */
/* video_common's ADDR16 helpers write a 16-bit register as two separate 8-bit CCI transactions,
 * high byte at the base address then low byte at base+1 (see ov5647_emul.c's header comment and
 * how it seeds the 0x300a/0x300b chip-ID pair) -- so the LOW byte, not the base address, is
 * where a small value like FULLFOV_X_ADDR_START (16, fits entirely in one byte) actually lands.
 * HTS (1852 / 2700) needs both bytes checked since neither fits in one byte. */
#define REG_TIMING_X_ADDR_START_LO 0x3801
#define REG_TIMING_Y_ADDR_START_LO 0x3803
#define REG_TIMING_X_ADDR_END_LO   0x3805
#define REG_TIMING_Y_ADDR_END_LO   0x3807
#define REG_TIMING_X_OUTPUT_HI     0x3808
#define REG_TIMING_X_OUTPUT_LO     0x3809
#define REG_TIMING_Y_OUTPUT_HI     0x380a
#define REG_TIMING_Y_OUTPUT_LO     0x380b
#define REG_TIMING_HTS_HI          0x380c
#define REG_TIMING_HTS_LO          0x380d
#define REG_TIMING_VTS_HI          0x380e
#define REG_TIMING_VTS_LO          0x380f
#define REG_TIMING_X_INC           0x3814
#define REG_TIMING_Y_INC           0x3815
#define REG_TIMING_TC_REG20        0x3820
#define REG_TIMING_TC_REG21        0x3821
#define REG_ANALOG_CTRL12          0x3612
#define REG_ANALOG_CTRL18          0x3618
#define REG_SENSOR_CTRL08          0x3708
#define REG_SENSOR_CTRL09          0x3709
/* MIPI bit-mode field -- ov5647_set_fmt() writes this before ov5647_set_mode_regs(); see the
 * ordering assertion below. */
#define REG_SC_PLL_CTRL0    0x3034
#define MIPI_BIT_MODE_RAW10 10

/* Registers that must NEVER be written by this driver -- Alif's ISP/BLC additions that run 58
 * traced its CSI "incorrect frame sequence" fatals to (AUTHORIZED LOCAL DIVERGENCE #3, see
 * ov5647.c's common-init block comment). Not in the reference driver's tables either. */
#define REG_ISP_RSVD_5001 0x5001
#define REG_ISP_RSVD_5002 0x5002
#define REG_BLC_RSVD_4050 0x4050
#define REG_BLC_RSVD_4051 0x4051

#define FULLFOV_X_ADDR_START 16
#define FULLFOV_Y_ADDR_START 0
/* 0x0a2f, low byte -- high byte (0x0a) is never 0, so the low-byte check alone is enough to
 * distinguish this from the crop path's window. */
#define FULLFOV_X_ADDR_END_LO 0x2f
#define FULLFOV_Y_ADDR_END_LO 0x9f /* 0x079f, low byte */
#define OUTPUT_640_HI         0x02
#define OUTPUT_640_LO         0x80
#define OUTPUT_480_HI         0x01
#define OUTPUT_480_LO         0xe0
#define ANALOG_CTRL12_BINNED  0x59
#define ANALOG_CTRL18_BINNED  0x00
#define SENSOR_CTRL08_BINNED  0x64
#define SENSOR_CTRL09_BINNED  0x52
#define SUBSAMPLE_1TO1        0x11
#define SUBSAMPLE_BINNED      0x35
#define TC_REG20_1TO1         0x40
#define TC_REG20_BINNED       0x41
#define TC_REG21_1TO1         0x00
/* 0x01, not Alif's variant 0x07 -- AUTHORIZED LOCAL DIVERGENCE #3, run 61 (see ov5647.c's
 * orientation note: 0x07 produced a left-right-mirrored scene on the bench). */
#define TC_REG21_BINNED 0x01

/* HTS is now PER MODE (run 61) -- 1852 (0x073c) for the 640x480 binned mode, the driver's
 * original bench-proven 2700 (0x0a8c) for the crop path. Both bytes checked since neither value
 * fits in a single CCI byte transaction. */
#define HTS_640X480_BINNED_HI 0x07
#define HTS_640X480_BINNED_LO 0x3c
#define HTS_CROP_HI           0x0a
#define HTS_CROP_LO           0x8c

/* VTS is derived from cfg->pixel_rate (OV5647_PIXEL_RATE, itself from OV5647_PLL_MULT) and the
 * ACTIVE mode's HTS -- see ov5647_frmrate_to_vts()/ov5647_hts_for() in ov5647.c. Both values
 * below assume pixel_rate = 58333333 (OV5647_PLL_MULT = 70): 2099 (0x0833) at 640x480 binned
 * (HTS 1852) and 15 fps; 1440 (0x05a0) at a crop size (HTS 2700) and 15 fps -- the ambient rate
 * every test in this suite runs at once issue #2248's run-62 default-frame-rate fix is in place
 * (see the REGRESSION test below). */
#define VTS_640X480_15FPS_HI     0x08
#define VTS_640X480_15FPS_LO     0x33
#define VTS_CROP_15FPS_HI        0x05
#define VTS_CROP_15FPS_LO        0xa0
#define DEFAULT_FRMRATE_DENOM_15 15

/* 50/60 Hz AEC band step (in LINES -- see ov5647.c's ov5647_set_mode_regs() comment). Moved out
 * of the common init and into the per-mode blocks (issue #2248 fix-up) because the line count
 * depends on HTS; the binned mode's values are bench-confirmed (runs 61/62), the crop path's are
 * the same numbers reused and marked BENCH-UNVERIFIED at HTS_CROP in ov5647.c. */
#define REG_AEC_RSVD_3A09  0x3a09
#define REG_AEC_RSVD_3A0A  0x3a0a
#define REG_AEC_RSVD_3A0B  0x3a0b
#define REG_AEC_RSVD_3A0D  0x3a0d
#define REG_AEC_RSVD_3A0E  0x3a0e
#define REG_BLC_RSVD_4004  0x4004
#define AEC_BAND_STEP_3A09 0x2e
#define AEC_BAND_STEP_3A0A 0x00
#define AEC_BAND_STEP_3A0B 0xfb
#define AEC_BAND_STEP_3A0D 0x02
#define AEC_BAND_STEP_3A0E 0x01
#define BLC_RSVD_4004_VAL  0x02

/*
 * Every entry AUTHORIZED LOCAL DIVERGENCE #3's "common init" block writes in
 * ov5647_init_regs[] (ov5647.c) -- the 0x3000/0x3018/analog/BLC/AEC/ISP-enable set taken from
 * the RPi/OmniVision reference, EXCLUDING the PLL/pad-drive octet (AUTHORIZED LOCAL DIVERGENCE
 * #2, covered by test_pll_init_written_before_first_running_mode_select) and the mode-specific
 * AEC band-step registers above (covered by the binned-mode test). Checked exhaustively, not by
 * sample, so a register silently dropped from ov5647_init_regs[] is caught here even if its
 * value happens to coincide with the sensor's power-on default (which a values-only readback
 * could not tell apart from "never written").
 */
static const struct ov5647_emul_write common_init_regs[] = {
	{ 0x3000, 0x00 }, { 0x3001, 0x00 }, { 0x3002, 0x00 }, { 0x3018, 0x44 }, { 0x370c, 0x03 },
	{ 0x3630, 0x2e }, { 0x3632, 0xe2 }, { 0x3633, 0x23 }, { 0x3634, 0x44 }, { 0x3620, 0x64 },
	{ 0x3621, 0xe0 }, { 0x3600, 0x37 }, { 0x3704, 0xa0 }, { 0x3703, 0x5a }, { 0x3715, 0x78 },
	{ 0x3717, 0x01 }, { 0x3731, 0x02 }, { 0x370b, 0x60 }, { 0x3705, 0x1a }, { 0x3f05, 0x02 },
	{ 0x3f06, 0x10 }, { 0x3f01, 0x0a }, { 0x3c01, 0x80 }, { 0x3b07, 0x0c }, { 0x3636, 0x06 },
	{ 0x3827, 0xec }, { 0x4001, 0x02 }, { 0x4000, 0x09 }, { 0x3a18, 0x00 }, { 0x3a19, 0xf8 },
	{ 0x3a08, 0x01 }, { 0x3a0f, 0x58 }, { 0x3a10, 0x50 }, { 0x3a1b, 0x58 }, { 0x3a1e, 0x50 },
	{ 0x3a11, 0x60 }, { 0x3a1f, 0x28 }, { 0x5000, 0x06 }, { 0x5003, 0x08 }, { 0x5a00, 0x08 },
};

static const struct device *ov5647_dev(void)
{
	return DEVICE_DT_GET(OV5647_NODE);
}

static const struct emul *ov5647_emul(void)
{
	return EMUL_DT_GET(OV5647_NODE);
}

/*
 * Assert that the LAST @p n writes recorded since the emulator's log was cleared exactly match
 * @p expect, in order. Order, not just final content, is the whole point of this suite: parking
 * while the sensor still sits in software standby writes the identical final register values but
 * never reaches LP-11 (bench-disproven variant -- see ov5647.c's OV5647_MIPI_CTRL00 comment), so
 * a check that only reads back final register contents cannot tell the two apart.
 */
static void
assert_write_sequence_tail(const struct ov5647_emul_write *expect, size_t n, const char *why)
{
	const struct emul *emul  = ov5647_emul();
	size_t             count = ov5647_emul_log_count(emul);

	zassert_true(count >= n,
	             "%s: only %zu register writes recorded, need at least the last %zu to check "
	             "the sequence order -- either fewer writes happened than the fix requires, "
	             "or the log filled past OV5647_EMUL_LOG_CAPACITY",
	             why,
	             count,
	             n);

	for (size_t i = 0; i < n; i++) {
		struct ov5647_emul_write w;

		zassert_ok(ov5647_emul_log_get(emul, count - n + i, &w),
		           "%s: log entry %zu unreadable",
		           why,
		           count - n + i);
		zassert_equal(w.reg,
		              expect[i].reg,
		              "%s: write #%zu of the sequence hit register 0x%04x, expected "
		              "0x%04x -- the ORDER is the fix (see ov5647.c's OV5647_MIPI_CTRL00 "
		              "comment): a reordered write can reach the same final register "
		              "values while still leaving the CSI-2 lanes out of LP-11",
		              why,
		              i,
		              w.reg,
		              expect[i].reg);
		zassert_equal(w.value,
		              expect[i].value,
		              "%s: register 0x%04x written as 0x%02x, "
		              "expected 0x%02x",
		              why,
		              w.reg,
		              w.value,
		              expect[i].value);
	}
}

ZTEST(ov5647, test_chip_id_probe_and_device_ready)
{
	zassert_true(device_is_ready(ov5647_dev()),
	             "OV5647 device not ready -- the chip-ID probe in ov5647_init() must have "
	             "failed against the emulator's seeded 0x300a/0x300b = 0x56/0x47, so "
	             "ov5647_init() returned -ENODEV and the lane-park sequence below never ran "
	             "at all");
}

ZTEST(ov5647, test_parked_quartet_after_init)
{
	const struct emul *emul = ov5647_emul();
	uint8_t            val;

	zassert_ok(ov5647_emul_get_reg(emul, REG_MODE_SELECT, &val));
	zassert_equal(val,
	              MODE_SELECT_RUNNING,
	              "MODE_SELECT (0x0100) = 0x%02x after ov5647_init(), want 0x01 (running) -- "
	              "the park is defined running, not from bare software standby",
	              val);

	zassert_ok(ov5647_emul_get_reg(emul, REG_MIPI_CTRL00, &val));
	zassert_equal(val,
	              MIPI_CTRL00_PARKED,
	              "MIPI_CTRL00 (0x4800) = 0x%02x after ov5647_init(), want 0x%02x (clock lane "
	              "gated + disabled, bus idle) -- without this the MIPI clock lane keeps "
	              "toggling and the CSI-2 link never reaches LP-11 Stop state",
	              val,
	              MIPI_CTRL00_PARKED);

	zassert_ok(ov5647_emul_get_reg(emul, REG_FRAME_OFF_NUM, &val));
	zassert_equal(val,
	              FRAME_OFF_NUM_PARKED,
	              "FRAME_OFF_NUM (0x4202) = 0x%02x after ov5647_init(), want 0x%02x (parked) "
	              "-- a CSI-2 receiver waiting for Stop-state before stream start will not "
	              "see it and cannot open the link",
	              val,
	              FRAME_OFF_NUM_PARKED);

	zassert_ok(ov5647_emul_get_reg(emul, REG_PAD_OUT, &val));
	zassert_equal(val,
	              PAD_OUT_PARKED,
	              "PAD_OUT (0x300d) = 0x%02x after ov5647_init(), want 0x%02x (parked) -- "
	              "this bench-only register (undocumented in the datasheet) is part of the "
	              "same park sequence; losing it alone still breaks LP-11",
	              val,
	              PAD_OUT_PARKED);
}

ZTEST(ov5647, test_park_order_after_init)
{
	static const struct ov5647_emul_write expect[] = {
		{ REG_MODE_SELECT, MODE_SELECT_RUNNING },
		{ REG_MIPI_CTRL00, MIPI_CTRL00_PARKED },
		{ REG_FRAME_OFF_NUM, FRAME_OFF_NUM_PARKED },
		{ REG_PAD_OUT, PAD_OUT_PARKED },
	};

	assert_write_sequence_tail(expect, ARRAY_SIZE(expect), "park sequence after ov5647_init()");
}

/*
 * Find the index of the first log entry writing @p reg with @p value; returns true and sets
 * @p out_idx if found. Used below to check ORDER, not just presence: see ov5647.c's
 * AUTHORIZED LOCAL DIVERGENCE #2 comment -- the PLL dividers latch at the 0x0103 software reset,
 * not on standby exit, so a write issued after MODE_SELECT has already gone running (0x01)
 * updates the register file without moving the running PLL (bench run 51 lost a cycle to exactly
 * this). A check that only confirms the PLL values are present ANYWHERE in the log would pass
 * that broken, reordered variant -- order is the fix.
 */
static bool find_write_index(const struct emul *emul, uint16_t reg, uint8_t value, size_t *out_idx)
{
	size_t count = ov5647_emul_log_count(emul);

	for (size_t i = 0; i < count; i++) {
		struct ov5647_emul_write w;

		zassert_ok(ov5647_emul_log_get(emul, i, &w), "log entry %zu unreadable", i);
		if (w.reg == reg && w.value == value) {
			*out_idx = i;
			return true;
		}
	}

	return false;
}

/* Like find_write_index(), but matches on @p reg alone -- for asserting a register was NEVER
 * written, at ANY value, not just not-with-one-particular-value. */
static bool reg_was_ever_written(const struct emul *emul, uint16_t reg)
{
	size_t count = ov5647_emul_log_count(emul);

	for (size_t i = 0; i < count; i++) {
		struct ov5647_emul_write w;

		zassert_ok(ov5647_emul_log_get(emul, i, &w), "log entry %zu unreadable", i);
		if (w.reg == reg) {
			return true;
		}
	}

	return false;
}

ZTEST(ov5647, test_pll_init_written_before_first_running_mode_select)
{
	const struct emul *emul = ov5647_emul();
	size_t             running_idx;
	size_t             pll_ctrl3_idx;
	size_t             pll_multiplier_idx;
	size_t             pll_ctrl1_idx;

	zassert_true(find_write_index(emul, REG_MODE_SELECT, MODE_SELECT_RUNNING, &running_idx),
	             "no write ever set MODE_SELECT (0x0100) running (0x01) during ov5647_init() "
	             "-- ov5647_lane_park() must not have run");

	zassert_true(find_write_index(emul, REG_SC_PLL_CTRL3, SC_PLL_CTRL3_PREDIV, &pll_ctrl3_idx),
	             "SC_PLL_CTRL3 (0x3037) was never written to the bench-proven prediv value "
	             "(%d) during ov5647_init()",
	             SC_PLL_CTRL3_PREDIV);
	zassert_true(pll_ctrl3_idx < running_idx,
	             "SC_PLL_CTRL3 (0x3037, prediv) was written at log index %zu, at or after "
	             "MODE_SELECT went running at index %zu -- the PLL dividers latch at the "
	             "0x0103 software reset, not on standby exit, so this write must land before "
	             "the park (see ov5647.c's AUTHORIZED LOCAL DIVERGENCE #2 comment)",
	             pll_ctrl3_idx,
	             running_idx);

	zassert_true(
	    find_write_index(emul, REG_SC_PLL_MULTIPLIER, SC_PLL_MULTIPLIER_MULT, &pll_multiplier_idx),
	    "SC_PLL_MULTIPLIER (0x3036) was never written to the bench-proven value "
	    "(%d) during ov5647_init()",
	    SC_PLL_MULTIPLIER_MULT);
	zassert_true(pll_multiplier_idx < running_idx,
	             "SC_PLL_MULTIPLIER (0x3036) was written at log index %zu, at or after "
	             "MODE_SELECT went running at index %zu -- see SC_PLL_CTRL3 above",
	             pll_multiplier_idx,
	             running_idx);

	zassert_true(find_write_index(emul, REG_SC_PLL_CTRL1, SC_PLL_CTRL1_SYS_DIV_2, &pll_ctrl1_idx),
	             "SC_PLL_CTRL1 (0x3035) was never written to the bench-proven sysdiv=2 value "
	             "(0x%02x) during ov5647_init()",
	             SC_PLL_CTRL1_SYS_DIV_2);
	zassert_true(pll_ctrl1_idx < running_idx,
	             "SC_PLL_CTRL1 (0x3035) was written at log index %zu, at or after MODE_SELECT "
	             "went running at index %zu -- see SC_PLL_CTRL3 above",
	             pll_ctrl1_idx,
	             running_idx);
}

/*
 * AUTHORIZED LOCAL DIVERGENCE #3 (issue #2248, bench runs 56/58/60): the full-FOV binned
 * 640x480 mode, ordering safety against the crop path (bench run 54's trap), and the common
 * analog/BLC/AEC init. These three tests still read the BOOT-time log (ov5647_init()'s own
 * set_fmt(FULL_WIDTH x FULL_HEIGHT) call takes the crop path, and MODE_SELECT only goes
 * running once during boot), so -- like test_pll_init_written_before_first_running_mode_select
 * above -- they must run before any test clears the log (test_stream_start_unparks below).
 */
ZTEST(ov5647, test_common_analog_blc_aec_before_first_running_mode_select)
{
	const struct emul *emul = ov5647_emul();
	size_t             running_idx;

	zassert_true(find_write_index(emul, REG_MODE_SELECT, MODE_SELECT_RUNNING, &running_idx),
	             "no write ever set MODE_SELECT (0x0100) running (0x01) during ov5647_init()");

	/* Exhaustive, not a 3-register sample: every entry in common_init_regs[] (mirroring
	 * ov5647.c's ov5647_init_regs[] common-init block exactly) must land before the park.
	 */
	for (size_t i = 0; i < ARRAY_SIZE(common_init_regs); i++) {
		size_t idx;

		zassert_true(
		    find_write_index(emul, common_init_regs[i].reg, common_init_regs[i].value, &idx),
		    "common-init register 0x%04x was never written to the bench-confirmed "
		    "value 0x%02x during ov5647_init() -- entry %zu of "
		    "common_init_regs[] dropped from ov5647_init_regs[]?",
		    common_init_regs[i].reg,
		    common_init_regs[i].value,
		    i);
		zassert_true(idx < running_idx,
		             "0x%04x written at log index %zu, at or after MODE_SELECT went "
		             "running at index %zu -- common init must run in standby, right "
		             "after the 0x0103 reset, same as the PLL block",
		             common_init_regs[i].reg,
		             idx,
		             running_idx);
	}

	/* Guard against the four Alif-only additions that run 58 traced its CSI "incorrect frame
	 * sequence" fatals to -- see ov5647.c's common-init block comment. Checked at ANY value,
	 * not just the specific bench-observed one, and over the WHOLE boot log, not just its
	 * running-write-ordered prefix.
	 */
	zassert_false(reg_was_ever_written(emul, REG_ISP_RSVD_5001),
	              "0x5001 (Alif's ISP-enable addition) must never be written");
	zassert_false(reg_was_ever_written(emul, REG_ISP_RSVD_5002),
	              "0x5002 (Alif's ISP-enable addition) must never be written");
	zassert_false(reg_was_ever_written(emul, REG_BLC_RSVD_4050),
	              "0x4050 (runs 56/58/60's superseded BLC addition) must never be written");
	zassert_false(reg_was_ever_written(emul, REG_BLC_RSVD_4051),
	              "0x4051 (runs 56/58/60's superseded BLC addition) must never be written");
}

/*
 * Named test_set_format_... (not test_640x480_...) deliberately: ztest's iterable test-case
 * section runs tests in NAME-SORTED order, not declaration order (confirmed by running this
 * suite -- a "test_640x480_..." name sorted before test_chip_id_probe_and_device_ready and its
 * ov5647_emul_clear_log() call wiped the boot log that
 * test_pll_init_written_before_first_running_mode_select and
 * test_common_analog_blc_aec_before_first_running_mode_select above depend on, failing both
 * with no driver bug involved). This name sorts after every boot-log reader above (test_c... /
 * test_p...) and before the first clearing test in the pre-existing suite
 * (test_stream_start_unparks) -- the same safe zone test_set_format_leaves_parked already
 * occupies.
 */
/*
 * The full 640x480 binned-mode register block ov5647_set_mode_regs() writes (AUTHORIZED LOCAL
 * DIVERGENCE #3, bench runs 61/62) -- window (all four of 0x3800/0x3802/0x3804/0x3806, not just
 * the start), output size (both 0x3808 and 0x380a), subsample (both 0x3814 AND 0x3815 -- an
 * earlier version of this test checked only 0x3814), binning-enable/orientation, per-mode HTS,
 * binned-mode analog (0x3612/0x3618/0x3708/0x3709), and the AEC band step (issue #2248 fix-up:
 * moved here from ov5647_init_regs[], see ov5647.c). Checked with find_write_index() in a loop
 * below, each entry paired with an ordering assertion against running_idx.
 */
static const struct ov5647_emul_write binned_mode_regs[] = {
	{ REG_TIMING_X_ADDR_START_LO, FULLFOV_X_ADDR_START },
	{ REG_TIMING_Y_ADDR_START_LO, FULLFOV_Y_ADDR_START },
	{ REG_TIMING_X_ADDR_END_LO, FULLFOV_X_ADDR_END_LO },
	{ REG_TIMING_Y_ADDR_END_LO, FULLFOV_Y_ADDR_END_LO },
	{ REG_TIMING_X_OUTPUT_HI, OUTPUT_640_HI },
	{ REG_TIMING_X_OUTPUT_LO, OUTPUT_640_LO },
	{ REG_TIMING_Y_OUTPUT_HI, OUTPUT_480_HI },
	{ REG_TIMING_Y_OUTPUT_LO, OUTPUT_480_LO },
	{ REG_TIMING_HTS_HI, HTS_640X480_BINNED_HI },
	{ REG_TIMING_HTS_LO, HTS_640X480_BINNED_LO },
	{ REG_TIMING_X_INC, SUBSAMPLE_BINNED },
	{ REG_TIMING_Y_INC, SUBSAMPLE_BINNED },
	{ REG_TIMING_TC_REG20, TC_REG20_BINNED },
	/* AUTHORIZED LOCAL DIVERGENCE #3, run 61/62 (maintainer-confirmed orientation): the
	 * reference's 0x01, not Alif's 0x07 -- see ov5647.c's orientation note.
	 */
	{ REG_TIMING_TC_REG21, TC_REG21_BINNED },
	{ REG_ANALOG_CTRL12, ANALOG_CTRL12_BINNED },
	{ REG_ANALOG_CTRL18, ANALOG_CTRL18_BINNED },
	{ REG_SENSOR_CTRL08, SENSOR_CTRL08_BINNED },
	{ REG_SENSOR_CTRL09, SENSOR_CTRL09_BINNED },
	{ REG_AEC_RSVD_3A09, AEC_BAND_STEP_3A09 },
	{ REG_AEC_RSVD_3A0A, AEC_BAND_STEP_3A0A },
	{ REG_AEC_RSVD_3A0B, AEC_BAND_STEP_3A0B },
	{ REG_AEC_RSVD_3A0D, AEC_BAND_STEP_3A0D },
	{ REG_AEC_RSVD_3A0E, AEC_BAND_STEP_3A0E },
	{ REG_BLC_RSVD_4004, BLC_RSVD_4004_VAL },
};

ZTEST(ov5647, test_set_format_640x480_binned_fullfov_before_park)
{
	const struct emul  *emul = ov5647_emul();
	struct video_format fmt  = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width       = 640,
		.height      = 480,
	};
	size_t bitmode_idx, running_idx, vts_hi_idx, vts_lo_idx;

	/* Does not depend on execution order -- always re-park first (harmless if already
	 * parked), same pattern as test_set_format_leaves_parked below. */
	zassert_ok(video_stream_stop(ov5647_dev(), VIDEO_BUF_TYPE_OUTPUT));
	ov5647_emul_clear_log(emul);

	zassert_ok(video_set_format(ov5647_dev(), &fmt), "video_set_format(640x480) failed");

	zassert_true(find_write_index(emul, REG_SC_PLL_CTRL0, MIPI_BIT_MODE_RAW10, &bitmode_idx),
	             "640x480 set_fmt() never wrote 0x3034's RAW10 bit-mode field");
	zassert_true(find_write_index(emul, REG_MODE_SELECT, MODE_SELECT_RUNNING, &running_idx),
	             "no write set MODE_SELECT running after 640x480 set_fmt() -- the lane park "
	             "must not have run");
	zassert_true(find_write_index(emul, REG_TIMING_VTS_HI, VTS_640X480_15FPS_HI, &vts_hi_idx),
	             "640x480 set_fmt() never wrote VTS high byte 0x%02x (2099/0x0833 at 15 fps) "
	             "-- issue #2248 run-62 REGRESSION: default frame rate must be 15, not 10",
	             VTS_640X480_15FPS_HI);
	zassert_true(find_write_index(emul, REG_TIMING_VTS_LO, VTS_640X480_15FPS_LO, &vts_lo_idx),
	             "640x480 set_fmt() never wrote VTS low byte 0x%02x (2099/0x0833 at 15 fps)",
	             VTS_640X480_15FPS_LO);

	for (size_t i = 0; i < ARRAY_SIZE(binned_mode_regs); i++) {
		size_t idx;

		zassert_true(
		    find_write_index(emul, binned_mode_regs[i].reg, binned_mode_regs[i].value, &idx),
		    "640x480 set_fmt() never wrote register 0x%04x = 0x%02x (entry %zu "
		    "of binned_mode_regs[])",
		    binned_mode_regs[i].reg,
		    binned_mode_regs[i].value,
		    i);
		zassert_true(idx < running_idx,
		             "0x%04x written at log index %zu, at/after the lane park's running "
		             "write at index %zu",
		             binned_mode_regs[i].reg,
		             idx,
		             running_idx);
		zassert_true(idx > bitmode_idx,
		             "0x%04x written at log index %zu, at/before the 0x3034 bit-mode "
		             "write at index %zu -- the mode block must follow it",
		             binned_mode_regs[i].reg,
		             idx,
		             bitmode_idx);
	}

	zassert_true(vts_hi_idx < running_idx && vts_lo_idx < running_idx,
	             "VTS written at log index %zu/%zu, at/after the lane park's running write "
	             "at index %zu",
	             vts_hi_idx,
	             vts_lo_idx,
	             running_idx);
}

/* Same name-sorting reason as test_set_format_640x480_binned_fullfov_before_park above. */
ZTEST(ov5647, test_set_format_switch_never_leaves_binning_on_crop_window)
{
	const struct emul  *emul    = ov5647_emul();
	struct video_format fmt_640 = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width       = 640,
		.height      = 480,
	};
	struct video_format fmt_1280 = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width       = 1280,
		.height      = 960,
	};
	uint8_t val;

	zassert_ok(video_stream_stop(ov5647_dev(), VIDEO_BUF_TYPE_OUTPUT));

	zassert_ok(video_set_format(ov5647_dev(), &fmt_640));
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_X_INC, &val));
	zassert_equal(val,
	              SUBSAMPLE_BINNED,
	              "0x3814 = 0x%02x after 640x480, want binned 0x%02x",
	              val,
	              SUBSAMPLE_BINNED);

	/* The run-54 trap: switching to a crop size must not leave binning armed against the
	 * new crop window -- see ov5647.c's ORDERING TRAP note in the file header.
	 */
	zassert_ok(video_set_format(ov5647_dev(), &fmt_1280));

	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_X_INC, &val));
	zassert_equal(val,
	              SUBSAMPLE_1TO1,
	              "0x3814 = 0x%02x after switching to 1280x960, want 1:1 0x%02x -- the "
	              "run-54 trap: binning must never be left on with a crop window",
	              val,
	              SUBSAMPLE_1TO1);
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_Y_INC, &val));
	zassert_equal(
	    val, SUBSAMPLE_1TO1, "0x3815 = 0x%02x after 1280x960, want 0x%02x", val, SUBSAMPLE_1TO1);
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_TC_REG20, &val));
	zassert_equal(
	    val, TC_REG20_1TO1, "0x3820 = 0x%02x after 1280x960, want 0x%02x", val, TC_REG20_1TO1);
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_TC_REG21, &val));
	zassert_equal(
	    val, TC_REG21_1TO1, "0x3821 = 0x%02x after 1280x960, want 0x%02x", val, TC_REG21_1TO1);

	/* AUTHORIZED LOCAL DIVERGENCE #3, run 61: a crop size must write the crop-path HTS
	 * (2700 / 0x0a8c), not leave the binned mode's 1852 armed.
	 */
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_HTS_HI, &val));
	zassert_equal(
	    val, HTS_CROP_HI, "0x380c = 0x%02x after 1280x960, want crop 0x%02x", val, HTS_CROP_HI);
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_HTS_LO, &val));
	zassert_equal(
	    val, HTS_CROP_LO, "0x380d = 0x%02x after 1280x960, want crop 0x%02x", val, HTS_CROP_LO);

	/* issue #2248, item 2(b): the VTS actually PROGRAMMED for a crop size must be computed
	 * with HTS_CROP (2700), not the binned mode's 1852 -- 1440 (0x05a0) at the ambient 15
	 * fps rate (see the run-62 default-frame-rate fix). A frmrate_to_vts()/set_frmival()
	 * that ignores the active mode's HTS would compute the WRONG one of these two numbers
	 * for one of the two modes; checking both modes' VTS in the same test catches that.
	 */
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_VTS_HI, &val));
	zassert_equal(val,
	              VTS_CROP_15FPS_HI,
	              "0x380e = 0x%02x after 1280x960, want crop-HTS VTS 0x%02x",
	              val,
	              VTS_CROP_15FPS_HI);
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_VTS_LO, &val));
	zassert_equal(val,
	              VTS_CROP_15FPS_LO,
	              "0x380f = 0x%02x after 1280x960, want crop-HTS VTS 0x%02x",
	              val,
	              VTS_CROP_15FPS_LO);

	/* Switching back to 640x480 must re-apply the binned set, not leave the 1:1 crop
	 * values from the size in between. */
	zassert_ok(video_set_format(ov5647_dev(), &fmt_640));
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_X_INC, &val));
	zassert_equal(val,
	              SUBSAMPLE_BINNED,
	              "0x3814 = 0x%02x after switching back to 640x480, want binned 0x%02x "
	              "again",
	              val,
	              SUBSAMPLE_BINNED);
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_HTS_HI, &val));
	zassert_equal(val,
	              HTS_640X480_BINNED_HI,
	              "0x380c = 0x%02x after switching back to 640x480, want binned 0x%02x again",
	              val,
	              HTS_640X480_BINNED_HI);
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_HTS_LO, &val));
	zassert_equal(val,
	              HTS_640X480_BINNED_LO,
	              "0x380d = 0x%02x after switching back to 640x480, want binned 0x%02x again",
	              val,
	              HTS_640X480_BINNED_LO);
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_TC_REG20, &val));
	zassert_equal(val,
	              TC_REG20_BINNED,
	              "0x3820 = 0x%02x after switching back to 640x480, want binned 0x%02x again",
	              val,
	              TC_REG20_BINNED);
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_VTS_HI, &val));
	zassert_equal(val,
	              VTS_640X480_15FPS_HI,
	              "0x380e = 0x%02x after switching back to 640x480, want binned-HTS VTS "
	              "0x%02x again",
	              val,
	              VTS_640X480_15FPS_HI);
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_VTS_LO, &val));
	zassert_equal(val,
	              VTS_640X480_15FPS_LO,
	              "0x380f = 0x%02x after switching back to 640x480, want binned-HTS VTS "
	              "0x%02x again",
	              val,
	              VTS_640X480_15FPS_LO);
}

/*
 * issue #2248, run-62 REGRESSION test (items 2a/2d): ov5647_init() used to boot into the
 * full-resolution crop, where the driver's own default 15 fps is unreachable at HTS_CROP (VTS
 * 1440 < 1944 + 24) -- Zephyr's video_closest_frmival() then picked 10 fps, which stuck in
 * data->frmrate for every LATER open too, including 640x480 (the mode this driver actually
 * ships), even though nothing in src/backends/camera or aen-camera-firstlight ever calls
 * set_frmival to override it. ov5647_init() now boots directly into 640x480, where 15 fps IS
 * reachable, so the shipped default stays 15. Named test_set_format_... for the same
 * name-sorting reason as the other set_format tests above.
 */
ZTEST(ov5647, test_set_format_640x480_default_rate_is_15fps)
{
	const struct emul  *emul = ov5647_emul();
	struct video_format fmt  = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width       = 640,
		.height      = 480,
	};
	struct video_frmival frmival;
	uint8_t              val;

	zassert_ok(video_stream_stop(ov5647_dev(), VIDEO_BUF_TYPE_OUTPUT));
	zassert_ok(video_set_format(ov5647_dev(), &fmt), "video_set_format(640x480) failed");

	zassert_ok(video_get_frmival(ov5647_dev(), &frmival));
	zassert_equal(frmival.denominator,
	              DEFAULT_FRMRATE_DENOM_15,
	              "default frame rate at 640x480 is %u/%u fps, want 15/1 -- the run-62 "
	              "default-frame-rate regression",
	              frmival.denominator,
	              frmival.numerator);

	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_VTS_HI, &val));
	zassert_equal(val,
	              VTS_640X480_15FPS_HI,
	              "0x380e = 0x%02x at the default rate, want the 15 fps VTS high byte 0x%02x",
	              val,
	              VTS_640X480_15FPS_HI);
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_VTS_LO, &val));
	zassert_equal(val,
	              VTS_640X480_15FPS_LO,
	              "0x380f = 0x%02x at the default rate, want the 15 fps VTS low byte 0x%02x",
	              val,
	              VTS_640X480_15FPS_LO);
}

/*
 * issue #2248, item 2(c): 640x480's reachable frame rates via ov5647_enum_frmival() -- 60 fps
 * must be accepted (VTS 524 >= 480 + 24) and 90 fps must be rejected (VTS 349 < 504). Zephyr's
 * enumeration helpers stop at the first rejected (sorted-ascending) entry -- see ov5647.c's
 * FIXED comment on ov5647_init()'s fmt initialiser -- so this also indirectly exercises that
 * every rate up to 60 in ov5647_framerates[] is itself reachable at 640x480.
 */
ZTEST(ov5647, test_enum_frmival_640x480_accepts_60_rejects_90)
{
	struct video_format fmt = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width       = 640,
		.height      = 480,
	};
	struct video_frmival_enum fie_60 = {
		.index  = 4, /* ov5647_framerates[] = {10, 15, 30, 45, 60, 90, 120} */
		.format = &fmt,
	};
	struct video_frmival_enum fie_90 = {
		.index  = 5,
		.format = &fmt,
	};

	zassert_ok(video_enum_frmival(ov5647_dev(), &fie_60),
	           "60 fps rejected at 640x480 -- want accepted (VTS 524 >= 480 + 24)");
	zassert_equal(fie_60.discrete.denominator, 60, "enum_frmival index 4 is not 60 fps");

	zassert_true(video_enum_frmival(ov5647_dev(), &fie_90) < 0,
	             "90 fps accepted at 640x480 -- want rejected (VTS 349 < 480 + 24)");
}

ZTEST(ov5647, test_stream_start_unparks)
{
	const struct emul                    *emul     = ov5647_emul();
	static const struct ov5647_emul_write expect[] = {
		{ REG_MIPI_CTRL00, MIPI_CTRL00_STREAMING },
		{ REG_FRAME_OFF_NUM, FRAME_OFF_NUM_STREAMING },
		{ REG_PAD_OUT, PAD_OUT_STREAMING },
		{ REG_MODE_SELECT, MODE_SELECT_RUNNING },
	};

	ov5647_emul_clear_log(emul);

	zassert_ok(video_stream_start(ov5647_dev(), VIDEO_BUF_TYPE_OUTPUT),
	           "video_stream_start() failed against a freshly parked sensor");

	assert_write_sequence_tail(expect, ARRAY_SIZE(expect), "unpark on stream start");
}

ZTEST(ov5647, test_stream_stop_reparks)
{
	const struct emul                    *emul     = ov5647_emul();
	static const struct ov5647_emul_write expect[] = {
		{ REG_MODE_SELECT, MODE_SELECT_RUNNING },
		{ REG_MIPI_CTRL00, MIPI_CTRL00_PARKED },
		{ REG_FRAME_OFF_NUM, FRAME_OFF_NUM_PARKED },
		{ REG_PAD_OUT, PAD_OUT_PARKED },
	};

	/* Enter from a known streaming state so this test does not depend on execution order. */
	zassert_ok(video_stream_start(ov5647_dev(), VIDEO_BUF_TYPE_OUTPUT));

	ov5647_emul_clear_log(emul);

	zassert_ok(video_stream_stop(ov5647_dev(), VIDEO_BUF_TYPE_OUTPUT),
	           "video_stream_stop() failed while streaming");

	assert_write_sequence_tail(expect,
	                           ARRAY_SIZE(expect),
	                           "re-park on stream stop -- a stopped stream must still "
	                           "present LP-11 so a later open can unpark it cleanly");
}

ZTEST(ov5647, test_stop_then_start_cycle_leaves_unparked_streaming)
{
	const struct emul *emul = ov5647_emul();
	uint8_t            val;

	/* The previous test leaves the sensor stopped and parked; this test proves a later start
	 * still unparks cleanly -- a sensor stuck parked after a stop/start cycle would silently
	 * blackhole every frame while every check above still reports success. */
	zassert_ok(video_stream_stop(ov5647_dev(), VIDEO_BUF_TYPE_OUTPUT));
	zassert_ok(video_stream_start(ov5647_dev(), VIDEO_BUF_TYPE_OUTPUT));

	zassert_ok(ov5647_emul_get_reg(emul, REG_MODE_SELECT, &val));
	zassert_equal(val,
	              MODE_SELECT_RUNNING,
	              "MODE_SELECT stuck at 0x%02x after a stop-then-start cycle, want running "
	              "(0x01)",
	              val);
	zassert_ok(ov5647_emul_get_reg(emul, REG_MIPI_CTRL00, &val));
	zassert_equal(val,
	              MIPI_CTRL00_STREAMING,
	              "MIPI_CTRL00 stuck parked (0x%02x) after a stop-then-start cycle, want "
	              "streaming (0x%02x) -- the sensor would present LP-11 forever and never "
	              "send a frame",
	              val,
	              MIPI_CTRL00_STREAMING);
	zassert_ok(ov5647_emul_get_reg(emul, REG_FRAME_OFF_NUM, &val));
	zassert_equal(val,
	              FRAME_OFF_NUM_STREAMING,
	              "FRAME_OFF_NUM stuck at 0x%02x after a stop-then-start cycle, want 0x%02x "
	              "(streaming)",
	              val,
	              FRAME_OFF_NUM_STREAMING);
	zassert_ok(ov5647_emul_get_reg(emul, REG_PAD_OUT, &val));
	zassert_equal(val,
	              PAD_OUT_STREAMING,
	              "PAD_OUT stuck at 0x%02x after a stop-then-start cycle, want 0x%02x "
	              "(streaming)",
	              val,
	              PAD_OUT_STREAMING);
}

ZTEST(ov5647, test_set_format_leaves_parked)
{
	const struct emul  *emul = ov5647_emul();
	struct video_format fmt  = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SBGGR8,
		.width       = 640,
		.height      = 480,
	};
	uint8_t val;

	/* set_format() is rejected with -EBUSY while streaming; leave the previous test's
	 * streaming state so this test does not depend on execution order. */
	zassert_ok(video_stream_stop(ov5647_dev(), VIDEO_BUF_TYPE_OUTPUT));

	zassert_ok(video_set_format(ov5647_dev(), &fmt), "video_set_format(640x480 SBGGR8) failed");

	/* ov5647_set_fmt() writes MODE_SELECT to standby before its mode registers, then
	 * re-parks via ov5647_lane_park() at the end. If a re-backport drops that final
	 * re-park call, MODE_SELECT is left at 0x00 (standby) here even though the other three
	 * park registers still read their old parked values from before this test ran --
	 * silently masking the regression on a values-only check of those three alone. */
	zassert_ok(ov5647_emul_get_reg(emul, REG_MODE_SELECT, &val));
	zassert_equal(val,
	              MODE_SELECT_RUNNING,
	              "MODE_SELECT = 0x%02x after video_set_format(), want 0x01 (running/parked) "
	              "-- a format change must leave the sensor parked again, not stuck in the "
	              "software standby set_fmt() drops into before reprogramming the PLL bit "
	              "mode field on what would otherwise be a running sensor",
	              val);
	zassert_ok(ov5647_emul_get_reg(emul, REG_MIPI_CTRL00, &val));
	zassert_equal(val,
	              MIPI_CTRL00_PARKED,
	              "MIPI_CTRL00 = 0x%02x after video_set_format(), want 0x%02x (parked)",
	              val,
	              MIPI_CTRL00_PARKED);
	zassert_ok(ov5647_emul_get_reg(emul, REG_FRAME_OFF_NUM, &val));
	zassert_equal(val,
	              FRAME_OFF_NUM_PARKED,
	              "FRAME_OFF_NUM = 0x%02x after video_set_format(), want 0x%02x (parked)",
	              val,
	              FRAME_OFF_NUM_PARKED);
	zassert_ok(ov5647_emul_get_reg(emul, REG_PAD_OUT, &val));
	zassert_equal(val,
	              PAD_OUT_PARKED,
	              "PAD_OUT = 0x%02x after video_set_format(), want 0x%02x (parked)",
	              val,
	              PAD_OUT_PARKED);
}

ZTEST_SUITE(ov5647, NULL, NULL, NULL, NULL, NULL);
