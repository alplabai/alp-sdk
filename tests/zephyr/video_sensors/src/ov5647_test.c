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

#define MODE_SELECT_RUNNING 0x01

#define MIPI_CTRL00_PARKED      0x25 /* CLOCK_LANE_GATE | BUS_IDLE | CLOCK_LANE_DISABLE */
#define MIPI_CTRL00_STREAMING   0x04 /* BUS_IDLE only -- continuous clock while streaming */
#define FRAME_OFF_NUM_PARKED    0x0f
#define FRAME_OFF_NUM_STREAMING 0x00
#define PAD_OUT_PARKED          0x01
#define PAD_OUT_STREAMING       0x00

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
