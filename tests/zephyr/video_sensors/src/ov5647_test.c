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
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/drivers/video-controls.h>
#include <zephyr/drivers/video.h>
#include <zephyr/sys/byteorder.h>
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

/* OV5647_TC_REG20_VFLIP / OV5647_TC_REG21_MIRROR in ov5647.c -- bits[2:1] of each register.
 * ctrls->vflip.val/hflip.val default to 0 (unset), which is why TC_REG20_BINNED/TC_REG21_BINNED
 * above already equal exactly the maintainer-confirmed default orientation (0x41/0x01) with no
 * mask bits merged in. */
#define TC_REG20_VFLIP_MASK  0x06
#define TC_REG21_MIRROR_MASK 0x06

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
 * of the common init and into the per-mode blocks (issue #2248 fix-up round 2) because the line
 * count depends on HTS; the binned mode's values are bench-confirmed (runs 61/62). 0x3a08
 * (round 3) is part of the same per-mode group even though its VALUE happens to be 0x01 in both
 * modes -- it moved out of the common init too. */
#define REG_AEC_RSVD_3A08  0x3a08
#define REG_AEC_RSVD_3A09  0x3a09
#define REG_AEC_RSVD_3A0A  0x3a0a
#define REG_AEC_RSVD_3A0B  0x3a0b
#define REG_AEC_RSVD_3A0D  0x3a0d
#define REG_AEC_RSVD_3A0E  0x3a0e
#define REG_BLC_RSVD_4004  0x4004
#define AEC_BAND_STEP_3A08 0x01
#define AEC_BAND_STEP_3A09 0x2e
#define AEC_BAND_STEP_3A0A 0x00
#define AEC_BAND_STEP_3A0B 0xfb
#define AEC_BAND_STEP_3A0D 0x02
#define AEC_BAND_STEP_3A0E 0x01
#define BLC_RSVD_4004_VAL  0x02
/* Crop path's OWN AEC band step (issue #2248 fix-up round 4, BENCH-UNVERIFIED) -- mainline's
 * full-resolution values (raspberrypi/linux rpi-6.6.y drivers/media/i2c/ov5647.c: 0x3a09 = 0x28
 * (296) and 0x3a0a/0x3a0b = 0x00/0xf6 (246) from ov5647_2592x1944_10bpp[]; 0x3a08 = 0x01 from
 * ov5647_common_regs[] instead -- CITATION CORRECTED (fix-up round 4): that rpi-6.6.y mode table
 * has NO 0x3a08 entry at all, only mainline Linux 6.6's own 2592x1944 table declares it per mode;
 * see ov5647.c's ov5647_init_regs[] comment above OV5647_AEC_RSVD_3A18 for the full citation),
 * LINE-TIME-SCALED from mainline's 32.51us line (HTS 2844 / pixel_rate 87500000) to this driver's
 * crop-path 46.29us line (OV5647_HTS_CROP 2700 / OV5647_PIXEL_RATE 58333333): fix-up round 3
 * copied mainline's line counts unscaled, which represents the WRONG real-time AC period at this
 * driver's different line time -- see ov5647.c's ov5647_set_mode_regs() comment for the full
 * arithmetic. 0x3a08/0x3a09/0x3a0a/0x3a0b depend only on HTS_CROP, so they are the SAME for every
 * crop size. 0x3a0d/0x3a0e (max bands per frame) are PER MODE instead (issue #2248 fix-up round
 * 5): floor(VTS/band) with VTS = height + OV5647_VBLANK_MIN, the requested height's OWN
 * minimum-blanking VTS -- not a constant pinned to OV5647_FULL_HEIGHT. The values below are for
 * the 1280x960 crop this file's tests use; see test_crop_aec_max_bands_computed_per_mode for the
 * 2592x1944 case (VTS 1968), the same rule mainline's own 0x08/0x06 reproduce. */
#define AEC_CROP_BANDSTEP_3A08_VAL 0x00
#define AEC_CROP_BANDSTEP_3A09_VAL 0xd0
#define AEC_CROP_BANDSTEP_3A0A_VAL 0x00
#define AEC_CROP_BANDSTEP_3A0B_VAL 0xad
/* 1280x960: min-blanking VTS 960 + 24 = 984 -> floor(984/173) = 5, floor(984/208) = 4. */
#define AEC_CROP_BANDSTEP_3A0D_VAL 0x05
#define AEC_CROP_BANDSTEP_3A0E_VAL 0x04
#define BLC_CROP_4004_VAL          0x04

/*
 * Every entry ov5647_init_regs[] (ov5647.c) writes in software standby, EXCLUDING only the
 * per-mode AEC band-step registers above (moved out in round 3; covered by the binned/crop-mode
 * tests instead). This INCLUDES the PLL/pad-drive octet's five non-PLL-divider registers
 * (0x303c/0x3106/0x3016/0x301c/0x301d) that test_pll_init_written_before_first_running_mode_
 * select does NOT check (that test covers only the three PLL divider values, 0x3035/0x3036/
 * 0x3037) -- round 3 fix-up: an earlier version of this table was NOT exhaustive over the whole
 * array, and deleting 0x303c/0x3106/0x301c from ov5647_init_regs[] left every test in this suite
 * passing. Checked exhaustively, not by sample, so a register silently dropped from
 * ov5647_init_regs[] is caught here even if its value happens to coincide with the sensor's
 * power-on default (which a values-only readback could not tell apart from "never written").
 *
 * ALSO INCLUDES (round 4 fix-up): 0x3503 (OV5647_MANUAL_CTRL = OV5647_MANUAL_CTRL_VTS, drives the
 * frame length from TIMING_VTS instead of letting the AEC stretch it) and 0x350c/0x350d
 * (OV5647_VTS_DIFF = 0, a REG16 -- two CCI byte transactions, high then low). Round 3's version of
 * this table omitted both even though ov5647_init_regs[] already wrote them; a mutation deleting
 * either from the driver left every test in this suite passing.
 */
static const struct ov5647_emul_write common_init_regs[] = {
	{ 0x3503, 0x04 }, { 0x350c, 0x00 }, { 0x350d, 0x00 }, { 0x303c, 0x11 }, { 0x3017, 0xf0 },
	{ 0x301c, 0xf8 }, { 0x301d, 0xf0 }, { 0x3106, 0xf5 }, { 0x3016, 0x08 }, { 0x3000, 0x00 },
	{ 0x3001, 0x00 }, { 0x3002, 0x00 }, { 0x3018, 0x44 }, { 0x370c, 0x03 }, { 0x3630, 0x2e },
	{ 0x3632, 0xe2 }, { 0x3633, 0x23 }, { 0x3634, 0x44 }, { 0x3620, 0x64 }, { 0x3621, 0xe0 },
	{ 0x3600, 0x37 }, { 0x3704, 0xa0 }, { 0x3703, 0x5a }, { 0x3715, 0x78 }, { 0x3717, 0x01 },
	{ 0x3731, 0x02 }, { 0x370b, 0x60 }, { 0x3705, 0x1a }, { 0x3f05, 0x02 }, { 0x3f06, 0x10 },
	{ 0x3f01, 0x0a }, { 0x3c01, 0x80 }, { 0x3b07, 0x0c }, { 0x3636, 0x06 }, { 0x3827, 0xec },
	{ 0x4001, 0x02 }, { 0x4000, 0x09 }, { 0x3a18, 0x00 }, { 0x3a19, 0xf8 }, { 0x3a0f, 0x58 },
	{ 0x3a10, 0x50 }, { 0x3a1b, 0x58 }, { 0x3a1e, 0x50 }, { 0x3a11, 0x60 }, { 0x3a1f, 0x28 },
	{ 0x5000, 0x06 }, { 0x5003, 0x08 }, { 0x5a00, 0x08 },
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

	/* The log tail checked here is actually ov5647_test_before()'s own re-park (its
	 * video_set_format() call, run ahead of every test including this one), not literally
	 * ov5647_init()'s -- see that hook's own comment for why the tail shape is identical
	 * either way (same park quartet, same ordering).
	 */
	assert_write_sequence_tail(
	    expect, ARRAY_SIZE(expect), "park sequence tail (ov5647_test_before()'s re-park)");
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
 * AUTHORIZED LOCAL DIVERGENCE #3 (issue #2248): the common analog/BLC/AEC/ISP-enable init. This
 * test reads the BOOT-time log (ov5647_init() now boots directly into 640x480 -- the binned
 * path, since the run-62 default-frame-rate fix, see ov5647_init()'s own FIXED comment -- and
 * MODE_SELECT only goes running once during boot), so -- like
 * test_pll_init_written_before_first_running_mode_select above -- it must run before any test
 * clears the log (test_stream_start_unparks below).
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
	{ REG_AEC_RSVD_3A08, AEC_BAND_STEP_3A08 },
	{ REG_AEC_RSVD_3A09, AEC_BAND_STEP_3A09 },
	{ REG_AEC_RSVD_3A0A, AEC_BAND_STEP_3A0A },
	{ REG_AEC_RSVD_3A0B, AEC_BAND_STEP_3A0B },
	{ REG_AEC_RSVD_3A0D, AEC_BAND_STEP_3A0D },
	{ REG_AEC_RSVD_3A0E, AEC_BAND_STEP_3A0E },
	{ REG_BLC_RSVD_4004, BLC_RSVD_4004_VAL },
};

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

	/* issue #2248 fix-up round 4: the crop path's AEC band step is now LINE-TIME-SCALED from
	 * mainline's full-resolution values, not mainline's byte-for-byte line counts (round 3's
	 * fix, which was still wrong -- mainline's line counts assume a 32.51us line, this driver's
	 * crop path runs 46.29us). Checked via the WRITE LOG, not a final-value readback: a
	 * final-value check alone cannot distinguish "the crop block wrote this register" from "the
	 * binned mode's earlier write is still sitting there, unwritten by the crop block" unless
	 * the two modes' values happen to differ (they do here, but the write-log check does not
	 * rely on that coincidence -- same reasoning as binned_mode_regs[] above).
	 */
	static const struct ov5647_emul_write crop_bandstep_regs[] = {
		{ REG_AEC_RSVD_3A08, AEC_CROP_BANDSTEP_3A08_VAL },
		{ REG_AEC_RSVD_3A09, AEC_CROP_BANDSTEP_3A09_VAL },
		{ REG_AEC_RSVD_3A0A, AEC_CROP_BANDSTEP_3A0A_VAL },
		{ REG_AEC_RSVD_3A0B, AEC_CROP_BANDSTEP_3A0B_VAL },
		{ REG_AEC_RSVD_3A0D, AEC_CROP_BANDSTEP_3A0D_VAL },
		{ REG_AEC_RSVD_3A0E, AEC_CROP_BANDSTEP_3A0E_VAL },
		{ REG_BLC_RSVD_4004, BLC_CROP_4004_VAL },
	};

	for (size_t i = 0; i < ARRAY_SIZE(crop_bandstep_regs); i++) {
		size_t idx;

		zassert_true(
		    find_write_index(emul, crop_bandstep_regs[i].reg, crop_bandstep_regs[i].value, &idx),
		    "crop-path set_format(1280x960) never wrote register 0x%04x = 0x%02x "
		    "(entry %zu of crop_bandstep_regs[]) -- either it was dropped, or it "
		    "still has the binned mode's line-time-scaled value",
		    crop_bandstep_regs[i].reg,
		    crop_bandstep_regs[i].value,
		    i);
	}

	zassert_ok(ov5647_emul_get_reg(emul, REG_AEC_RSVD_3A08, &val));
	zassert_equal(val,
	              AEC_CROP_BANDSTEP_3A08_VAL,
	              "0x3a08 = 0x%02x after 1280x960, want the crop path's own 0x%02x",
	              val,
	              AEC_CROP_BANDSTEP_3A08_VAL);
	zassert_ok(ov5647_emul_get_reg(emul, REG_AEC_RSVD_3A09, &val));
	zassert_equal(val,
	              AEC_CROP_BANDSTEP_3A09_VAL,
	              "0x3a09 = 0x%02x after 1280x960, want the crop path's own 0x%02x, not the "
	              "binned-mode value 0x%02x",
	              val,
	              AEC_CROP_BANDSTEP_3A09_VAL,
	              AEC_BAND_STEP_3A09);
	zassert_ok(ov5647_emul_get_reg(emul, REG_AEC_RSVD_3A0A, &val));
	zassert_equal(val,
	              AEC_CROP_BANDSTEP_3A0A_VAL,
	              "0x3a0a = 0x%02x after 1280x960, want the crop path's own 0x%02x",
	              val,
	              AEC_CROP_BANDSTEP_3A0A_VAL);
	zassert_ok(ov5647_emul_get_reg(emul, REG_AEC_RSVD_3A0B, &val));
	zassert_equal(val,
	              AEC_CROP_BANDSTEP_3A0B_VAL,
	              "0x3a0b = 0x%02x after 1280x960, want the crop path's own 0x%02x, not the "
	              "binned-mode value 0x%02x",
	              val,
	              AEC_CROP_BANDSTEP_3A0B_VAL,
	              AEC_BAND_STEP_3A0B);
	zassert_ok(ov5647_emul_get_reg(emul, REG_AEC_RSVD_3A0D, &val));
	zassert_equal(val,
	              AEC_CROP_BANDSTEP_3A0D_VAL,
	              "0x3a0d = 0x%02x after 1280x960, want the crop path's own 0x%02x, not the "
	              "binned-mode (max-bands) value 0x%02x",
	              val,
	              AEC_CROP_BANDSTEP_3A0D_VAL,
	              AEC_BAND_STEP_3A0D);
	zassert_ok(ov5647_emul_get_reg(emul, REG_AEC_RSVD_3A0E, &val));
	zassert_equal(val,
	              AEC_CROP_BANDSTEP_3A0E_VAL,
	              "0x3a0e = 0x%02x after 1280x960, want the crop path's own 0x%02x, not the "
	              "binned-mode (max-bands) value 0x%02x",
	              val,
	              AEC_CROP_BANDSTEP_3A0E_VAL,
	              AEC_BAND_STEP_3A0E);
	zassert_ok(ov5647_emul_get_reg(emul, REG_BLC_RSVD_4004, &val));
	zassert_equal(val,
	              BLC_CROP_4004_VAL,
	              "0x4004 = 0x%02x after 1280x960, want the crop path's own 0x%02x",
	              val,
	              BLC_CROP_4004_VAL);

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
 * issue #2248 fix-up round 5, MAJOR BUG: 0x3a0d/0x3a0e (max bands per frame) were pinned to the
 * 2592x1944 crop's own minimum-blanking VTS (1968) for EVERY crop size -- a smaller crop got the
 * SAME 11/9-band figures even though they overstate what that smaller frame can hold: see
 * ov5647_set_mode_regs()'s block comment (11 * 173 = 1903, 9 * 208 = 1872, both bigger than
 * 1280x960's own ~984-line minimum-blanking VTS). Checked for two DIFFERENT crop sizes in one
 * test, so a regression back to one constant pair for both cannot hide behind whichever size a
 * test happens to check alone (test_set_format_switch_never_leaves_binning_on_crop_window above
 * only ever exercises 1280x960).
 */
ZTEST(ov5647, test_crop_aec_max_bands_computed_per_mode)
{
	const struct emul  *emul     = ov5647_emul();
	struct video_format fmt_1280 = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width       = 1280,
		.height      = 960,
	};
	struct video_format fmt_full = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width       = 2592,
		.height      = 1944,
	};
	uint8_t val;

	zassert_ok(video_stream_stop(ov5647_dev(), VIDEO_BUF_TYPE_OUTPUT));

	/* 1280x960: min-blanking VTS 960 + 24 = 984 -> floor(984/173) = 5, floor(984/208) = 4. */
	zassert_ok(video_set_format(ov5647_dev(), &fmt_1280));
	zassert_ok(ov5647_emul_get_reg(emul, REG_AEC_RSVD_3A0D, &val));
	zassert_equal(
	    val, 0x05, "0x3a0d = 0x%02x after 1280x960, want 0x05 (floor((960+24)/173))", val);
	zassert_ok(ov5647_emul_get_reg(emul, REG_AEC_RSVD_3A0E, &val));
	zassert_equal(
	    val, 0x04, "0x3a0e = 0x%02x after 1280x960, want 0x04 (floor((960+24)/208))", val);

	/* 2592x1944: min-blanking VTS 1944 + 24 = 1968 -> floor(1968/173) = 11, floor(1968/208) =
	 * 9 -- unchanged from before round 5, since 1968 is the VTS the pre-round-5 constant was
	 * itself derived from.
	 */
	zassert_ok(video_set_format(ov5647_dev(), &fmt_full));
	zassert_ok(ov5647_emul_get_reg(emul, REG_AEC_RSVD_3A0D, &val));
	zassert_equal(
	    val, 0x0b, "0x3a0d = 0x%02x after 2592x1944, want 0x0b (floor((1944+24)/173))", val);
	zassert_ok(ov5647_emul_get_reg(emul, REG_AEC_RSVD_3A0E, &val));
	zassert_equal(
	    val, 0x09, "0x3a0e = 0x%02x after 2592x1944, want 0x09 (floor((1944+24)/208))", val);
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
 * must be accepted (VTS 524 >= 480 + 24) and 90 fps must be rejected (VTS 349 < 504). This test
 * queries indices 4 and 5 directly (video_enum_frmival() is a thin per-index wrapper -- it does
 * NOT scan from index 0, unlike video_closest_frmival(), see ov5647.c's FIXED comment on
 * ov5647_init()'s fmt initialiser for that scan's own "stops at first rejected entry" behaviour),
 * so it does NOT exercise whether indices 0-3 (10/15/30/45 fps) are also reachable at 640x480 --
 * only that the 60/90 boundary itself is correct.
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

/*
 * issue #2248 fix-up round 3, item 1: MAJOR BUG -- the frame rate used to stick after a format
 * change. ov5647_set_fmt() fed data->frmrate (the last EFFECTIVE, possibly clamped, rate) back
 * in as the new request, instead of the rate the user originally asked for. set_format(2592x1944)
 * clamps to 10 fps (HTS_CROP can't sustain 15 fps at that height); a later set_format(640x480)
 * used to inherit that clamped 10 rather than re-requesting 15, even though 640x480 can reach 15
 * fine. Fixed by data->requested_frmrate, set only by ov5647_set_frmival() (see its
 * declaration), which ov5647_set_fmt() now re-requests on every format change.
 */
ZTEST(ov5647, test_set_format_frmrate_does_not_stick_across_format_change)
{
	const struct emul  *emul     = ov5647_emul();
	struct video_format fmt_full = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width       = 2592,
		.height      = 1944,
	};
	struct video_format fmt_640 = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width       = 640,
		.height      = 480,
	};
	struct video_frmival frmival;
	uint8_t              val;

	zassert_ok(video_stream_stop(ov5647_dev(), VIDEO_BUF_TYPE_OUTPUT));

	/* Sanity: full-res really does clamp to 10 fps at the ambient (15) requested rate --
	 * HTS_CROP (2700) can't sustain 15 fps at 1944 lines (VTS 1440 < 1968).
	 */
	zassert_ok(video_set_format(ov5647_dev(), &fmt_full), "video_set_format(2592x1944) failed");
	zassert_ok(video_get_frmival(ov5647_dev(), &frmival));
	zassert_equal(frmival.denominator,
	              10,
	              "2592x1944 did not clamp to 10 fps as expected (got %u/%u) -- the rest of "
	              "this test's premise depends on this clamp actually happening",
	              frmival.denominator,
	              frmival.numerator);

	/* The regression: switching to 640x480 must re-request 15, not inherit the clamped 10. */
	zassert_ok(video_set_format(ov5647_dev(), &fmt_640), "video_set_format(640x480) failed");
	zassert_ok(video_get_frmival(ov5647_dev(), &frmival));
	zassert_equal(frmival.denominator,
	              DEFAULT_FRMRATE_DENOM_15,
	              "frame rate is %u/%u after switching from 2592x1944 (clamped to 10) back "
	              "to 640x480, want 15/1 -- the rate must not stick at a prior mode's clamped "
	              "value",
	              frmival.denominator,
	              frmival.numerator);

	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_VTS_HI, &val));
	zassert_equal(val,
	              VTS_640X480_15FPS_HI,
	              "0x380e = 0x%02x after switching back to 640x480, want the 15 fps VTS high "
	              "byte 0x%02x again",
	              val,
	              VTS_640X480_15FPS_HI);
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_VTS_LO, &val));
	zassert_equal(val,
	              VTS_640X480_15FPS_LO,
	              "0x380f = 0x%02x after switching back to 640x480, want the 15 fps VTS low "
	              "byte 0x%02x again",
	              val,
	              VTS_640X480_15FPS_LO);
}

/*
 * issue #2248 fix-up round 3, item 2: MAJOR BUG -- format changes silently undid the flip
 * controls. ov5647_set_mode_regs() writes 0x3820/0x3821 as whole bytes (AUTHORIZED LOCAL
 * DIVERGENCE #3), which wiped OV5647_TC_REG20_VFLIP/OV5647_TC_REG21_MIRROR bits a caller had set
 * via VIDEO_CID_VFLIP/VIDEO_CID_HFLIP, while the ctrl itself kept reporting the value the caller
 * set -- ctrl and hardware silently diverged. Fixed by re-applying both ctrls after
 * ov5647_set_mode_regs() inside ov5647_set_fmt(). Also checks the maintainer-confirmed default:
 * with both ctrls at their defaults (0), the mode bytes must still be exactly 0x41/0x01 -- the
 * fix must not add mask bits on top of an already-correct default.
 */
ZTEST(ov5647, test_set_format_preserves_flip_ctrls)
{
	const struct emul  *emul = ov5647_emul();
	struct video_format fmt  = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width       = 640,
		.height      = 480,
	};
	struct video_control vflip_on = { .id = VIDEO_CID_VFLIP, .val = 1 };
	struct video_control hflip_on = { .id = VIDEO_CID_HFLIP, .val = 1 };
	uint8_t              val;

	zassert_ok(video_stream_stop(ov5647_dev(), VIDEO_BUF_TYPE_OUTPUT));
	zassert_ok(video_set_format(ov5647_dev(), &fmt), "video_set_format(640x480) failed");

	/* Maintainer-confirmed default (run 62): both ctrls unset must give exactly 0x41/0x01,
	 * not those values with extra mask bits merged in.
	 */
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_TC_REG20, &val));
	zassert_equal(val,
	              TC_REG20_BINNED,
	              "0x3820 = 0x%02x at default hflip/vflip, want exactly 0x%02x "
	              "(maintainer-confirmed orientation, run 62)",
	              val,
	              TC_REG20_BINNED);
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_TC_REG21, &val));
	zassert_equal(val,
	              TC_REG21_BINNED,
	              "0x3821 = 0x%02x at default hflip/vflip, want exactly 0x%02x "
	              "(maintainer-confirmed orientation, run 62)",
	              val,
	              TC_REG21_BINNED);

	zassert_ok(video_set_ctrl(ov5647_dev(), &vflip_on));
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_TC_REG20, &val));
	zassert_equal(val,
	              TC_REG20_BINNED | TC_REG20_VFLIP_MASK,
	              "0x3820 = 0x%02x right after VIDEO_CID_VFLIP=1, want 0x%02x",
	              val,
	              TC_REG20_BINNED | TC_REG20_VFLIP_MASK);

	/* The regression: a format change must not silently undo the ctrl just set.
	 *
	 * Reset fmt.pixelformat to the BASE fourcc before re-submitting: issue #2248 fix-up round
	 * 5 made set_format() write the EFFECTIVE (flip-shifted) fourcc back into *fmt on success
	 * (matching get_format(), see ov5647_set_fmt()'s own comment) -- the PREVIOUS call above,
	 * still at the default (unflipped) orientation, was a no-op remap, but VFLIP is now on, so
	 * blindly resubmitting fmt as the previous call left it would submit a fourcc this driver
	 * only accepts when VFLIP is what it was at THAT time, not now.
	 */
	fmt.pixelformat = VIDEO_PIX_FMT_SBGGR10P;
	zassert_ok(video_set_format(ov5647_dev(), &fmt), "video_set_format(640x480) failed");
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_TC_REG20, &val));
	zassert_equal(val,
	              TC_REG20_BINNED | TC_REG20_VFLIP_MASK,
	              "0x3820 = 0x%02x after a set_format() with VFLIP still logically 1, want "
	              "0x%02x -- set_format() must not silently undo VIDEO_CID_VFLIP",
	              val,
	              TC_REG20_BINNED | TC_REG20_VFLIP_MASK);

	/* Toggle HFLIP on top of the still-set VFLIP; both must survive yet another set_format. */
	zassert_ok(video_set_ctrl(ov5647_dev(), &hflip_on));
	fmt.pixelformat = VIDEO_PIX_FMT_SBGGR10P; /* same reset as above -- see that comment */
	zassert_ok(video_set_format(ov5647_dev(), &fmt), "video_set_format(640x480) failed");
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_TC_REG20, &val));
	zassert_equal(val,
	              TC_REG20_BINNED | TC_REG20_VFLIP_MASK,
	              "0x3820 = 0x%02x after a second set_format() with VFLIP still 1, want 0x%02x",
	              val,
	              TC_REG20_BINNED | TC_REG20_VFLIP_MASK);
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_TC_REG21, &val));
	zassert_equal(val,
	              TC_REG21_BINNED | TC_REG21_MIRROR_MASK,
	              "0x3821 = 0x%02x after a set_format() with HFLIP still logically 1, want "
	              "0x%02x -- set_format() must not silently undo VIDEO_CID_HFLIP",
	              val,
	              TC_REG21_BINNED | TC_REG21_MIRROR_MASK);

	/* No in-test cleanup: ov5647_test_before() (the ZTEST_SUITE before-hook) resets both flip
	 * ctrls ahead of every test, including whichever one runs next -- issue #2248 fix-up
	 * round 4, see its declaration.
	 */
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

/*
 * issue #2248 fix-up round 4: MAJOR BUG, introduced by round 3's OWN fix for the sticky-rate bug
 * (test_set_format_frmrate_does_not_stick_across_format_change above) -- data->requested_frmrate
 * was a bare uint32_t storing only frmival->denominator, so a request whose NUMERATOR is not 1
 * (e.g. {2, 60}, still 30 fps) silently turned into a DIFFERENT rate ({1, 60}, 60 fps) on the next
 * format change. This is not a hypothetical shape: Zephyr's own `video frmival <dev> <interval>`
 * shell command sends requests exactly this way (`100ms` is {100, 1000}, not {1, 10}). Fixed by
 * storing the WHOLE struct video_frmival as data->requested_frmival -- see its declaration.
 */
ZTEST(ov5647, test_set_frmival_numerator_survives_format_change)
{
	struct video_format fmt_640 = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width       = 640,
		.height      = 480,
	};
	struct video_format fmt_full = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width       = 2592,
		.height      = 1944,
	};
	/* Deliberately NOT 1/30 -- the same 30 fps rate, expressed with a numerator != 1. */
	struct video_frmival req = { .numerator = 2, .denominator = 60 };
	struct video_frmival got;

	zassert_ok(video_set_format(ov5647_dev(), &fmt_640));
	zassert_ok(video_set_frmival(ov5647_dev(), &req));
	zassert_ok(video_get_frmival(ov5647_dev(), &got));
	zassert_equal(got.denominator,
	              30,
	              "video_set_frmival({2, 60}) (30 fps) gave %u/%u fps at 640x480, want 30/1",
	              got.denominator,
	              got.numerator);

	/* The regression: switching away and back must re-request the ORIGINAL {2, 60}, landing on
	 * 30 fps again -- not a denominator-only {1, 60} a bare-uint32_t requested_frmrate would
	 * have silently produced, landing on 60 fps instead.
	 */
	zassert_ok(video_set_format(ov5647_dev(), &fmt_full));
	zassert_ok(video_set_format(ov5647_dev(), &fmt_640));
	zassert_ok(video_get_frmival(ov5647_dev(), &got));
	zassert_equal(got.denominator,
	              30,
	              "frame rate is %u/%u after switching away from and back to 640x480, want "
	              "30/1 -- requested_frmival must preserve the numerator, not just the "
	              "denominator",
	              got.denominator,
	              got.numerator);
}

/*
 * issue #2248 fix-up round 5, pre-existing bug: Zephyr's video_closest_frmival() tracks the best
 * candidate against a running best-diff initialised to INT32_MAX ns, and only updates its match
 * out-param when a candidate BEATS that running best -- if EVERY candidate's diff from the
 * request exceeds INT32_MAX ns, the out-param is never touched even though the function still
 * returns 0. ov5647_set_frmival() used to echo that untouched (still the caller's raw request)
 * out-param straight back to ITS OWN caller, while the VTS register was actually programmed for
 * a different rate (whatever index the out-param happened to default to). See
 * ov5647_set_frmival()'s own comment for the exact mechanism this reproduces.
 */
ZTEST(ov5647, test_set_frmival_far_request_reports_applied_rate)
{
	/* {5, 1} is a 5 s frame interval (0.2 fps) -- every candidate 640x480 offers (up to 60 fps,
	 * see test_enum_frmival_640x480_accepts_60_rejects_90) differs from it by several seconds,
	 * comfortably past INT32_MAX ns (~2.147 s).
	 */
	struct video_frmival req = { .numerator = 5, .denominator = 1 };
	struct video_frmival active;

	zassert_ok(video_set_frmival(ov5647_dev(), &req),
	           "video_closest_frmival() returning 0 without ever matching a candidate must "
	           "not be treated as a driver error");
	zassert_ok(video_get_frmival(ov5647_dev(), &active));

	zassert_equal(req.numerator,
	              active.numerator,
	              "set_frmival({5,1}) reported %u/%u but the ACTIVE rate is %u/%u -- must "
	              "report what was actually applied, not the raw request",
	              req.numerator,
	              req.denominator,
	              active.numerator,
	              active.denominator);
	zassert_equal(req.denominator,
	              active.denominator,
	              "set_frmival({5,1}) reported %u/%u but the ACTIVE rate is %u/%u -- must "
	              "report what was actually applied, not the raw request",
	              req.numerator,
	              req.denominator,
	              active.numerator,
	              active.denominator);
}

/* i2c_emul mock -- fails only the VTS register bytes (0x380e/0x380f); every other access returns
 * -ENOSYS, which struct i2c_emul's own mock_api falls back to the real emulator API for (see its
 * doc comment), so it passes through unmodified.
 */
static int
ov5647_test_fail_vts_write(const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr)
{
	ARG_UNUSED(target);
	ARG_UNUSED(addr);

	if (num_msgs == 1 && msgs[0].len == 3) {
		uint16_t reg = sys_get_be16(msgs[0].buf);

		if (reg == 0x380e || reg == 0x380f) {
			return -EIO;
		}
	}
	return -ENOSYS;
}

static struct i2c_emul_api ov5647_test_fail_vts_api = { .transfer = ov5647_test_fail_vts_write };

/*
 * issue #2248 fix-up round 4: data->requested_frmival is only overwritten AFTER the VTS write
 * succeeds -- see its own declaration ("A rejected/failed request must not overwrite a
 * previously-saved one"). That claim had no test exercising a REAL write failure; a mutation
 * deleting the "only on success" guard would have left every existing test passing, since none
 * of them ever made an I2C write fail. Exercised here via i2c_emul's mock_api, not the claim
 * alone.
 */
ZTEST(ov5647, test_set_frmival_failed_write_does_not_overwrite_request)
{
	const struct emul  *emul     = ov5647_emul();
	struct video_format fmt_full = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width       = 2592,
		.height      = 1944,
	};
	struct video_format fmt_640 = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width       = 640,
		.height      = 480,
	};
	/* Deliberately a DIFFERENT rate than the {1, 15} ov5647_test_before() already requested,
	 * so a leak is observable.
	 */
	struct video_frmival req30 = { .numerator = 1, .denominator = 30 };
	struct video_frmival got;

	emul->bus.i2c->mock_api = &ov5647_test_fail_vts_api;
	zassert_true(video_set_frmival(ov5647_dev(), &req30) < 0,
	             "a real I2C failure on the VTS registers was not reported to the caller");
	emul->bus.i2c->mock_api = NULL;

	/* ov5647_set_fmt() re-requests data->requested_frmival on every format change -- if the
	 * REJECTED {1, 30} above had overwritten it anyway, this would land on 30 fps, not the
	 * original 15 ov5647_test_before() set.
	 */
	zassert_ok(video_set_format(ov5647_dev(), &fmt_full));
	zassert_ok(video_set_format(ov5647_dev(), &fmt_640));
	zassert_ok(video_get_frmival(ov5647_dev(), &got));
	zassert_equal(got.denominator,
	              DEFAULT_FRMRATE_DENOM_15,
	              "a REJECTED set_frmival() request leaked into requested_frmival: rate is "
	              "%u/%u after switching away and back to 640x480, want the original 15/1",
	              got.numerator,
	              got.denominator);
}

/*
 * issue #2248, round 4 (no NEW register writes, but an API contract change, not mere
 * documentation): the flip ctrls shift the Bayer colour order, and ov5647_get_fmt() must report
 * that shift -- see ov5647_bayer_pixfmt()'s comment in ov5647.c for the RPi-reference-derived
 * mapping and why (0,0) -> SBGGR, (1,0) -> SGBRG, (0,1) -> SGRBG, (1,1) -> SRGGB in terms of THIS
 * driver's ctrl values. Only the default (0,0) order is bench/maintainer-verified; the other
 * three are DERIVED, not bench-checked -- see docs/camera-shields.md.
 */
ZTEST(ov5647, test_get_fmt_reports_default_bayer_order)
{
	struct video_format got;

	zassert_ok(video_get_format(ov5647_dev(), &got));
	zassert_equal(got.pixelformat,
	              VIDEO_PIX_FMT_SBGGR10P,
	              "get_format() reports 0x%08x at the default (unflipped) orientation, want "
	              "SBGGR10P -- the maintainer-confirmed default Bayer order (run 62)",
	              got.pixelformat);
}

ZTEST(ov5647, test_get_fmt_reports_hflip_bayer_order)
{
	struct video_control hflip_on = { .id = VIDEO_CID_HFLIP, .val = 1 };
	struct video_format  got;

	zassert_ok(video_set_ctrl(ov5647_dev(), &hflip_on));
	zassert_ok(video_get_format(ov5647_dev(), &got));
	zassert_equal(got.pixelformat,
	              VIDEO_PIX_FMT_SGBRG10P,
	              "get_format() reports 0x%08x with HFLIP=1, want SGBRG10P",
	              got.pixelformat);
}

ZTEST(ov5647, test_get_fmt_reports_vflip_bayer_order)
{
	struct video_control vflip_on = { .id = VIDEO_CID_VFLIP, .val = 1 };
	struct video_format  got;

	zassert_ok(video_set_ctrl(ov5647_dev(), &vflip_on));
	zassert_ok(video_get_format(ov5647_dev(), &got));
	zassert_equal(got.pixelformat,
	              VIDEO_PIX_FMT_SGRBG10P,
	              "get_format() reports 0x%08x with VFLIP=1, want SGRBG10P",
	              got.pixelformat);
}

ZTEST(ov5647, test_get_fmt_reports_both_flips_bayer_order)
{
	struct video_control hflip_on = { .id = VIDEO_CID_HFLIP, .val = 1 };
	struct video_control vflip_on = { .id = VIDEO_CID_VFLIP, .val = 1 };
	struct video_format  got;

	zassert_ok(video_set_ctrl(ov5647_dev(), &hflip_on));
	zassert_ok(video_set_ctrl(ov5647_dev(), &vflip_on));
	zassert_ok(video_get_format(ov5647_dev(), &got));
	zassert_equal(got.pixelformat,
	              VIDEO_PIX_FMT_SRGGB10P,
	              "get_format() reports 0x%08x with HFLIP=1 and VFLIP=1, want SRGGB10P",
	              got.pixelformat);
}

/*
 * issue #2248 fix-up round 5, MAJOR BUG: ov5647_set_fmt() only ever matched the two BASE fourccs
 * (SBGGR8/SBGGR10P) against ov5647_fmts[], but ov5647_get_fmt() above reports a flip-shifted one
 * once either flip ctrl is set -- so a get_format() -> set_format() round trip failed with
 * -ENOTSUP for every flip state except the default. One ZTEST per flip state, mirroring the four
 * get_fmt tests above.
 */
ZTEST(ov5647, test_set_fmt_roundtrip_default)
{
	struct video_format fmt;

	zassert_ok(video_get_format(ov5647_dev(), &fmt));
	zassert_equal(fmt.pixelformat, VIDEO_PIX_FMT_SBGGR10P);
	zassert_ok(video_set_format(ov5647_dev(), &fmt),
	           "get_format() -> set_format() round trip failed at the default orientation");
	zassert_equal(fmt.pixelformat,
	              VIDEO_PIX_FMT_SBGGR10P,
	              "set_format() changed the reported fourcc to 0x%08x on a no-op round trip",
	              fmt.pixelformat);
}

ZTEST(ov5647, test_set_fmt_roundtrip_hflip)
{
	struct video_control hflip_on = { .id = VIDEO_CID_HFLIP, .val = 1 };
	struct video_format  fmt;

	zassert_ok(video_set_ctrl(ov5647_dev(), &hflip_on));
	zassert_ok(video_get_format(ov5647_dev(), &fmt));
	zassert_equal(fmt.pixelformat, VIDEO_PIX_FMT_SGBRG10P);
	zassert_ok(video_set_format(ov5647_dev(), &fmt),
	           "get_format() -> set_format() round trip failed at HFLIP=1 (SGBRG10P)");
	zassert_equal(fmt.pixelformat,
	              VIDEO_PIX_FMT_SGBRG10P,
	              "set_format() changed the reported fourcc to 0x%08x on a no-op round trip",
	              fmt.pixelformat);
}

ZTEST(ov5647, test_set_fmt_roundtrip_vflip)
{
	struct video_control vflip_on = { .id = VIDEO_CID_VFLIP, .val = 1 };
	struct video_format  fmt;

	zassert_ok(video_set_ctrl(ov5647_dev(), &vflip_on));
	zassert_ok(video_get_format(ov5647_dev(), &fmt));
	zassert_equal(fmt.pixelformat, VIDEO_PIX_FMT_SGRBG10P);
	zassert_ok(video_set_format(ov5647_dev(), &fmt),
	           "get_format() -> set_format() round trip failed at VFLIP=1 (SGRBG10P)");
	zassert_equal(fmt.pixelformat,
	              VIDEO_PIX_FMT_SGRBG10P,
	              "set_format() changed the reported fourcc to 0x%08x on a no-op round trip",
	              fmt.pixelformat);
}

ZTEST(ov5647, test_set_fmt_roundtrip_both_flips)
{
	struct video_control hflip_on = { .id = VIDEO_CID_HFLIP, .val = 1 };
	struct video_control vflip_on = { .id = VIDEO_CID_VFLIP, .val = 1 };
	struct video_format  fmt;

	zassert_ok(video_set_ctrl(ov5647_dev(), &hflip_on));
	zassert_ok(video_set_ctrl(ov5647_dev(), &vflip_on));
	zassert_ok(video_get_format(ov5647_dev(), &fmt));
	zassert_equal(fmt.pixelformat, VIDEO_PIX_FMT_SRGGB10P);
	zassert_ok(video_set_format(ov5647_dev(), &fmt),
	           "get_format() -> set_format() round trip failed at HFLIP=1/VFLIP=1 (SRGGB10P)");
	zassert_equal(fmt.pixelformat,
	              VIDEO_PIX_FMT_SRGGB10P,
	              "set_format() changed the reported fourcc to 0x%08x on a no-op round trip",
	              fmt.pixelformat);
}

/*
 * issue #2248 fix-up round 5: a caller submitting the BASE fourcc directly (not the flip-shifted
 * one) must still work while a flip ctrl is active -- ov5647_set_fmt()'s remap-to-base check must
 * be a no-op for an already-base fourcc, not accidentally reject it.
 */
ZTEST(ov5647, test_set_fmt_base_fourcc_accepted_while_flipped)
{
	struct video_control hflip_on = { .id = VIDEO_CID_HFLIP, .val = 1 };
	struct video_format  fmt      = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width       = 640,
		.height      = 480,
	};
	struct video_format got;

	zassert_ok(video_set_ctrl(ov5647_dev(), &hflip_on));
	zassert_ok(video_set_format(ov5647_dev(), &fmt),
	           "set_format() rejected the BASE fourcc (SBGGR10P) while HFLIP=1");

	/* set_format() reports the EFFECTIVE fourcc back, matching get_format() -- HFLIP=1 shifts
	 * the BASE request that was just accepted to SGBRG10P.
	 */
	zassert_equal(fmt.pixelformat,
	              VIDEO_PIX_FMT_SGBRG10P,
	              "set_format() reported 0x%08x for a base-fourcc request while HFLIP=1, want "
	              "the effective SGBRG10P",
	              fmt.pixelformat);

	zassert_ok(video_get_format(ov5647_dev(), &got));
	zassert_equal(got.pixelformat,
	              fmt.pixelformat,
	              "get_format() (0x%08x) disagrees with what set_format() just reported "
	              "(0x%08x)",
	              got.pixelformat,
	              fmt.pixelformat);
}

/*
 * issue #2248 fix-up round 5: a flip ctrl changed AFTER set_format() (no second set_format()
 * call) must still be reflected the next time get_format() is queried -- data->fmt only ever
 * stores the BASE fourcc; ov5647_get_fmt() re-derives the effective one from the CURRENT ctrl
 * state on every call, so this needs no driver change to hold, only a test proving it does.
 */
ZTEST(ov5647, test_get_fmt_reflects_flip_changed_after_set_format)
{
	struct video_control hflip_on = { .id = VIDEO_CID_HFLIP, .val = 1 };
	struct video_format  fmt      = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width       = 640,
		.height      = 480,
	};
	struct video_format got;

	zassert_ok(video_set_format(ov5647_dev(), &fmt));
	zassert_ok(video_get_format(ov5647_dev(), &got));
	zassert_equal(got.pixelformat,
	              VIDEO_PIX_FMT_SBGGR10P,
	              "get_format() reports 0x%08x before any flip ctrl is touched, want SBGGR10P",
	              got.pixelformat);

	/* No set_format() call between the ctrl change and this get_format() -- the order must
	 * still update.
	 */
	zassert_ok(video_set_ctrl(ov5647_dev(), &hflip_on));
	zassert_ok(video_get_format(ov5647_dev(), &got));
	zassert_equal(got.pixelformat,
	              VIDEO_PIX_FMT_SGBRG10P,
	              "get_format() reports 0x%08x after HFLIP=1 with no intervening set_format(), "
	              "want SGBRG10P",
	              got.pixelformat);
}

/*
 * ZTEST_SUITE before-hook (issue #2248 fix-up round 4): resets the state every test in this suite
 * implicitly assumes as its starting point -- both flip ctrls unset, the default {1, 15} frame
 * interval, and 640x480 SBGGR10P -- instead of relying on in-test cleanup at the END of whichever
 * test happened to set them (which a failed assertion skips) or on ztest's NAME-SORTED execution
 * order leaving the "right" test to run last.
 *
 * Ordering is deliberate, not incidental: video_set_format() is called LAST, after both ctrls and
 * the frame interval are already at their reset values, so this hook's OWN writes end in exactly
 * the same lane-park quartet (MODE_SELECT -> MIPI_CTRL00 -> FRAME_OFF_NUM -> PAD_OUT) that
 * ov5647_init() itself ends with -- ov5647_set_fmt() re-requests data->requested_frmival (already
 * reset by the video_set_frmival() call below) and re-applies both ctrls (already off) before its
 * own final ov5647_lane_park(). This keeps test_park_order_after_init's write-log TAIL check valid
 * even though this hook now runs ahead of it too: the tail is still a park sequence, just this
 * hook's own re-park rather than literally ov5647_init()'s. The two boot-log tests that search the
 * WHOLE log rather than its tail (test_pll_init_written_before_first_running_mode_select,
 * test_common_analog_blc_aec_before_first_running_mode_select) are unaffected either way -- this
 * hook never touches the PLL or common-init registers those look for, only set_mode_regs()'s
 * per-mode block, ctrls, and VTS.
 */
static void ov5647_test_before(void *fixture)
{
	struct video_control hflip_off = { .id = VIDEO_CID_HFLIP, .val = 0 };
	struct video_control vflip_off = { .id = VIDEO_CID_VFLIP, .val = 0 };
	struct video_frmival frmival   = { .numerator = 1, .denominator = 15 };
	struct video_format  fmt       = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width       = 640,
		.height      = 480,
	};

	ARG_UNUSED(fixture);

	/* set_format()/set_ctrl() are rejected or reordered against a running mode while
	 * streaming -- stop first so this reset does not depend on whichever streaming state the
	 * PREVIOUS test (name-sort order, not declaration order) left the sensor in.
	 */
	zassert_ok(video_stream_stop(ov5647_dev(), VIDEO_BUF_TYPE_OUTPUT));
	zassert_ok(video_set_ctrl(ov5647_dev(), &hflip_off));
	zassert_ok(video_set_ctrl(ov5647_dev(), &vflip_off));
	zassert_ok(video_set_frmival(ov5647_dev(), &frmival));
	zassert_ok(video_set_format(ov5647_dev(), &fmt));
}

ZTEST_SUITE(ov5647, NULL, NULL, ov5647_test_before, NULL, NULL);
