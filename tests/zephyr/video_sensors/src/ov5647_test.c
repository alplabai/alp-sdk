/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Runtime ztest locking in the OV5647 lane-park fix (issue #2248, the AUTHORIZED LOCAL DIVERGENCE
 * recorded at the top of zephyr/drivers/video/ov5647.c) against the I2C emulator in
 * ov5647_emul.c, on native_sim's real "zephyr,i2c-emul-controller" (&i2c0 -- see app.overlay).
 *
 * SCOPE: lane-park only. Does NOT assert anything about OV5647_PIXEL_RATE / VIDEO_CID_PIXEL_RATE
 * -- a previous "fix" to that value was investigated, disproven and reverted; the current value
 * is the unmodified upstream one, and pinning it here would cement a number nobody has validated
 * (see ov5647.c's OV5647_PIXEL_RATE comment).
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

/* Bench-proven values from ov5647_init_regs[] (OV5647_PLL_PREDIV/MULT/SYS_DIV in ov5647.c) */
#define SC_PLL_CTRL3_PREDIV    3
#define SC_PLL_MULTIPLIER_MULT 105
#define SC_PLL_CTRL1_SYS_DIV_2 0x21

/* AUTHORIZED LOCAL DIVERGENCE #3 (issue #2248, bench runs 56/58/60) -- the full-FOV binned
 * 640x480 mode, its ordering trap against the crop path, and the common analog/BLC/AEC init.
 * Kept local for the same reason as the quartet/PLL trio above. */
/* video_common's ADDR16 helpers write a 16-bit register as two separate 8-bit CCI transactions,
 * high byte at the base address then low byte at base+1 (see ov5647_emul.c's header comment and
 * how it seeds the 0x300a/0x300b chip-ID pair) -- so the LOW byte, not the base address, is
 * where a small value like FULLFOV_X_ADDR_START (16, fits entirely in one byte) actually lands.
 */
#define REG_TIMING_X_ADDR_START_LO 0x3801
#define REG_TIMING_X_INC           0x3814
#define REG_TIMING_Y_INC           0x3815
#define REG_TIMING_TC_REG20        0x3820
#define REG_TIMING_TC_REG21        0x3821
#define REG_ANALOG_RSVD_370C       0x370c
#define REG_BLC_RSVD_4001          0x4001
#define REG_AEC_RSVD_3A18          0x3a18

#define FULLFOV_X_ADDR_START 16
#define SUBSAMPLE_1TO1       0x11
#define SUBSAMPLE_BINNED     0x35
#define TC_REG20_1TO1        0x40
#define TC_REG20_BINNED      0x41
#define TC_REG21_1TO1        0x00
#define TC_REG21_BINNED      0x07
#define ANALOG_RSVD_370C_VAL 0x03
#define BLC_RSVD_4001_VAL    0x02
#define AEC_RSVD_3A18_VAL    0x00

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
	size_t             running_idx, analog_idx, blc_idx, aec_idx;

	zassert_true(find_write_index(emul, REG_MODE_SELECT, MODE_SELECT_RUNNING, &running_idx),
	             "no write ever set MODE_SELECT (0x0100) running (0x01) during ov5647_init()");

	zassert_true(find_write_index(emul, REG_ANALOG_RSVD_370C, ANALOG_RSVD_370C_VAL, &analog_idx),
	             "0x370c was never written to the bench-confirmed value 0x%02x during "
	             "ov5647_init() -- the common analog init (AUTHORIZED LOCAL DIVERGENCE #3) "
	             "must land in ov5647_init_regs[]",
	             ANALOG_RSVD_370C_VAL);
	zassert_true(analog_idx < running_idx,
	             "0x370c written at log index %zu, at or after MODE_SELECT went running at "
	             "index %zu -- common analog/BLC/AEC init must run in standby, right after "
	             "the 0x0103 reset, same as the PLL block",
	             analog_idx,
	             running_idx);

	zassert_true(find_write_index(emul, REG_BLC_RSVD_4001, BLC_RSVD_4001_VAL, &blc_idx),
	             "0x4001 was never written to the bench-confirmed value 0x%02x during "
	             "ov5647_init()",
	             BLC_RSVD_4001_VAL);
	zassert_true(blc_idx < running_idx,
	             "0x4001 written at log index %zu, at or after MODE_SELECT went running at "
	             "index %zu",
	             blc_idx,
	             running_idx);

	zassert_true(find_write_index(emul, REG_AEC_RSVD_3A18, AEC_RSVD_3A18_VAL, &aec_idx),
	             "0x3a18 was never written to the bench-confirmed value 0x%02x during "
	             "ov5647_init()",
	             AEC_RSVD_3A18_VAL);
	zassert_true(aec_idx < running_idx,
	             "0x3a18 written at log index %zu, at or after MODE_SELECT went running at "
	             "index %zu",
	             aec_idx,
	             running_idx);
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
ZTEST(ov5647, test_set_format_640x480_binned_fullfov_before_park)
{
	const struct emul  *emul = ov5647_emul();
	struct video_format fmt  = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width       = 640,
		.height      = 480,
	};
	size_t window_idx, subsample_idx, binning_idx, running_idx;

	/* Does not depend on execution order -- always re-park first (harmless if already
	 * parked), same pattern as test_set_format_leaves_parked below. */
	zassert_ok(video_stream_stop(ov5647_dev(), VIDEO_BUF_TYPE_OUTPUT));
	ov5647_emul_clear_log(emul);

	zassert_ok(video_set_format(ov5647_dev(), &fmt), "video_set_format(640x480) failed");

	zassert_true(
	    find_write_index(emul, REG_TIMING_X_ADDR_START_LO, FULLFOV_X_ADDR_START, &window_idx),
	    "640x480 set_fmt() never wrote the full-array window start (0x3801 = %d) -- "
	    "run-56/60 evidence: 640x480 must be a full-array BINNED mode, not the "
	    "648x488 centre crop",
	    FULLFOV_X_ADDR_START);
	zassert_true(find_write_index(emul, REG_TIMING_X_INC, SUBSAMPLE_BINNED, &subsample_idx),
	             "640x480 set_fmt() never wrote 0x3814 = 0x%02x (binned subsample)",
	             SUBSAMPLE_BINNED);
	zassert_true(find_write_index(emul, REG_TIMING_TC_REG20, TC_REG20_BINNED, &binning_idx),
	             "640x480 set_fmt() never wrote 0x3820 = 0x%02x (binning enable)",
	             TC_REG20_BINNED);
	zassert_true(find_write_index(emul, REG_MODE_SELECT, MODE_SELECT_RUNNING, &running_idx),
	             "no write set MODE_SELECT running after 640x480 set_fmt() -- the lane park "
	             "must not have run");

	zassert_true(window_idx < running_idx,
	             "full-array window written at log index %zu, at/after the lane park's "
	             "running write at index %zu",
	             window_idx,
	             running_idx);
	zassert_true(subsample_idx < running_idx,
	             "0x3814 written at log index %zu, at/after the lane park's running write at "
	             "index %zu",
	             subsample_idx,
	             running_idx);
	zassert_true(binning_idx < running_idx,
	             "0x3820 written at log index %zu, at/after the lane park's running write at "
	             "index %zu",
	             binning_idx,
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
	zassert_ok(ov5647_emul_get_reg(emul, REG_TIMING_TC_REG20, &val));
	zassert_equal(val,
	              TC_REG20_BINNED,
	              "0x3820 = 0x%02x after switching back to 640x480, want binned 0x%02x again",
	              val,
	              TC_REG20_BINNED);
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
