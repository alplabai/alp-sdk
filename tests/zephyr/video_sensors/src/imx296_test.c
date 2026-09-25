/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Runtime ztest for the IMX296 streaming driver (zephyr/drivers/video/imx296.c), against the I2C
 * emulator in imx296_emul.c on native_sim's real "zephyr,i2c-emul-controller" (&i2c0 -- see
 * app.overlay). Clones the ov5647_test.c/ov9281_test.c pattern.
 *
 * CSI-2 streaming (a captured frame) is BENCH-UNVERIFIED for this driver (issue #2287, bench run
 * 229 confirmed only the I2C identity path -- see docs/camera-shields.md); this suite proves the
 * register-level logic the driver runs BEFORE any frame would arrive, it does not stand in for a
 * bench capture.
 */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/video-controls.h>
#include <zephyr/drivers/video.h>
#include <zephyr/ztest.h>

#include "imx296_emul.h"

#define IMX296_NODE        DT_NODELABEL(imx296_test)
#define IMX296_BAD_ID_NODE DT_NODELABEL(imx296_bad_id_test)

/* Register addresses this test peeks/pokes -- kept local rather than pulling imx296.c's private
 * IMX296_REG*()-wrapped values, which also encode CCI address/data-size metadata this test
 * doesn't need. */
#define REG_STANDBY  0x3000
#define REG_XMSTA    0x300a
#define REG_REVERSE  0x300e
#define REG_SHS_LSB  0x308d
#define REG_SHS_MID  0x308e
#define REG_SHS_MSB  0x308f
#define REG_GAIN_LSB 0x3204
#define REG_GAIN_MSB 0x3205

/* VMAX (24-bit LE), HMAX (16-bit LE), INCKSEL0..3 and CSI_TIMING -- see imx296.c's
 * IMX296_REG_VMAX / IMX296_REG_HMAX / IMX296_REG_INCKSEL0..3 / IMX296_REG_CSI_TIMING and
 * imx296_inck_table[]'s 54 MHz row. */
#define REG_VMAX_LSB   0x3010
#define REG_VMAX_MID   0x3011
#define REG_VMAX_MSB   0x3012
#define REG_HMAX_LSB   0x3014
#define REG_HMAX_MSB   0x3015
#define REG_INCKSEL0   0x3089
#define REG_INCKSEL1   0x308a
#define REG_INCKSEL2   0x308b
#define REG_INCKSEL3   0x308c
#define REG_CSI_TIMING 0x418c

/* IMX296_REG_CSI_LANE_HS / IMX296_REG_BLKLEVEL / IMX296_REG_ROI_ENABLE -- see imx296.c's own
 * comments above those macros. */
#define REG_CSI_LANE_HS  0x3005
#define REG_BLKLEVEL_LSB 0x3254
#define REG_BLKLEVEL_MSB 0x3255
#define REG_ROI_ENABLE   0x3300

/* IMX296_REG_TRIGEN / IMX296_REG_LOWLAGTRG / IMX296_REG_SYNCSEL -- see imx296.c's own comments
 * above those macros (issue #2287's external-trigger addition). */
#define REG_TRIGEN    0x300b
#define REG_LOWLAGTRG 0x30ae
#define REG_SYNCSEL   0x3036

#define STANDBY_STANDBY  BIT(0)
#define XMSTA_STOP       BIT(0)
#define REVERSE_VREVERSE BIT(0)
#define REVERSE_HREVERSE BIT(1)
#define TRIGEN_TRIGGER   BIT(0)
#define LOWLAGTRG_FAST   BIT(0)
#define SYNCSEL_NORMAL   0xc0

/* Mirrors imx296.c's IMX296_CID_TRIGGER_MODE -- not a public header, so redefined here from the
 * same VIDEO_CID_PRIVATE_BASE convention (see that macro's comment in imx296.c). */
#define IMX296_CID_TRIGGER_MODE     (VIDEO_CID_PRIVATE_BASE + 0x01)
#define IMX296_TRIGGER_MODE_FREE_RUN 0
#define IMX296_TRIGGER_MODE_EXTERNAL 1

/* Sony IMX296 datasheet "Register List of All-pixel scan mode" (page 49): VMAX = 1118 lines/frame
 * -- see IMX296_VMAX in imx296.c. Exposure is in lines of integration (VMAX - SHS); SHS_MIN = 4
 * ("Register List of Shutter setting", page 60) gives the exposure ceiling VMAX - 4 = 1114. */
#define IMX296_VMAX  1118
#define EXPOSURE_MAX (IMX296_VMAX - 4)
#define GAIN_MAX     480

static const struct device *imx296_dev(void)
{
	return DEVICE_DT_GET(IMX296_NODE);
}

static const struct emul *imx296_emul(void)
{
	return EMUL_DT_GET(IMX296_NODE);
}

ZTEST(imx296, test_device_is_ready)
{
	/* Only reachable if imx296_init()'s SENSOR_INFO probe matched the bench-confirmed
	 * colour-IMX296LQR-C signature imx296_emul.c seeds -- see that file's header comment. */
	zassert_true(device_is_ready(imx296_dev()), "IMX296 device not ready");
}

ZTEST(imx296, test_sensor_info_mismatch_leaves_device_not_ready)
{
	/* imx296_bad_id_test (app.overlay) shares this driver but its emulator instance
	 * (imx296_emul.c, matched by CCI address 0x1b) seeds a SENSOR_INFO that does NOT match
	 * IMX296_SENSOR_INFO_LQR_COLOUR -- imx296_init() must reject it with -ENODEV rather than
	 * silently proceeding to program VMAX/HMAX/INCKSEL against unidentified silicon. */
	zassert_false(device_is_ready(DEVICE_DT_GET(IMX296_BAD_ID_NODE)),
	              "device with a mismatched SENSOR_INFO should not be ready");
}

ZTEST(imx296, test_init_register_values)
{
	/* Final register state left by imx296_init() for the 54 MHz INCK the app.overlay's
	 * imx296_input_clock fixed-clock provides -- imx296_inck_table[]'s 54 MHz row. Read as
	 * final values (not log order -- see test_init_quiesce_order below for that), so this
	 * survives regardless of what earlier tests in this suite have done since boot. */
	const struct emul *emul = imx296_emul();
	uint8_t            val;

	/* VMAX = 1118 = 0x00045E, 24-bit LE */
	zassert_ok(imx296_emul_get_reg(emul, REG_VMAX_LSB, &val));
	zassert_equal(val, 0x5e, "VMAX LSB");
	zassert_ok(imx296_emul_get_reg(emul, REG_VMAX_MID, &val));
	zassert_equal(val, 0x04, "VMAX mid byte");
	zassert_ok(imx296_emul_get_reg(emul, REG_VMAX_MSB, &val));
	zassert_equal(val, 0x00, "VMAX MSB");

	/* HMAX = 1100 = 0x044C, 16-bit LE */
	zassert_ok(imx296_emul_get_reg(emul, REG_HMAX_LSB, &val));
	zassert_equal(val, 0x4c, "HMAX LSB");
	zassert_ok(imx296_emul_get_reg(emul, REG_HMAX_MSB, &val));
	zassert_equal(val, 0x04, "HMAX MSB");

	zassert_ok(imx296_emul_get_reg(emul, REG_INCKSEL0, &val));
	zassert_equal(val, 0xb0, "INCKSEL0 (54 MHz row)");
	zassert_ok(imx296_emul_get_reg(emul, REG_INCKSEL1, &val));
	zassert_equal(val, 0x0f, "INCKSEL1 (54 MHz row)");
	zassert_ok(imx296_emul_get_reg(emul, REG_INCKSEL2, &val));
	zassert_equal(val, 0xb0, "INCKSEL2 (54 MHz row)");
	zassert_ok(imx296_emul_get_reg(emul, REG_INCKSEL3, &val));
	zassert_equal(val, 0x0c, "INCKSEL3 (54 MHz row)");

	zassert_ok(imx296_emul_get_reg(emul, REG_CSI_TIMING, &val));
	zassert_equal(val, 0xa8, "CSI_TIMING (54 MHz row)");

	/* Bench-derived (issue #2287, E1M-AEN803 2026W36-0001): without this write the CSI-2 data
	 * lane never leaves LP-11. Not datasheet-documented -- see IMX296_REG_CSI_LANE_HS's
	 * comment in imx296.c. */
	zassert_ok(imx296_emul_get_reg(emul, REG_CSI_LANE_HS, &val));
	zassert_equal(val, 0xf0, "CSI_LANE_HS");

	/* "Register List of All-pixel scan mode" (page 49): BLKLEVEL, 12-bit LE, 0x03C. */
	zassert_ok(imx296_emul_get_reg(emul, REG_BLKLEVEL_LSB, &val));
	zassert_equal(val, 0x3c, "BLKLEVEL LSB");
	zassert_ok(imx296_emul_get_reg(emul, REG_BLKLEVEL_MSB, &val));
	zassert_equal(val, 0x00, "BLKLEVEL MSB");
}

/* Find the FIRST recorded write to @p reg since boot (or the last imx296_emul_clear_log()) --
 * used below to inspect imx296_init()'s own default write, even if a LATER-run test (ztest does
 * not guarantee declaration order between suites -- see ZTEST_SUITE's own docs) has since
 * overwritten that register via video_set_ctrl(). The register's FINAL value is not a safe check
 * here for that reason; its first logged write is. */
static bool imx296_test_first_write(const struct emul *emul, uint16_t reg, uint8_t *value)
{
	struct imx296_emul_write w;

	for (size_t i = 0; i < imx296_emul_log_count(emul); i++) {
		if (imx296_emul_log_get(emul, i, &w) == 0 && w.reg == reg) {
			*value = w.value;
			return true;
		}
	}

	return false;
}

ZTEST(imx296, test_shs_gain_reverse_defaults_written_at_init)
{
	/* imx296_init() now writes SHS/GAIN/REVERSE to their control-registry default values
	 * (see the comment above these writes in imx296.c) so hardware and the control cache
	 * agree from boot, not just after the first explicit video_set_ctrl(). Checked via the
	 * FIRST logged write to each register (see imx296_test_first_write() above), not the
	 * final register value, because ztest test order is not declaration order and another
	 * test's video_set_ctrl() may have since overwritten these same registers. */
	const struct emul *emul = imx296_emul();
	uint8_t            val;

	/*
	 * SHS is the shutter START line, not exposure lines (page 60): the register
	 * value IS IMX296_SHS_DEFAULT (14 = 0x00000E, 24-bit LE), not VMAX -
	 * IMX296_SHS_DEFAULT. An earlier version of this init write put VMAX -
	 * IMX296_SHS_DEFAULT (1104 = 0x000450) in the register instead -- a 14-line
	 * (~0.2 ms) exposure instead of the intended 1104-line one -- and this
	 * assertion locked that wrong value in rather than catching it; it now
	 * asserts the correct one.
	 */
	zassert_true(imx296_test_first_write(emul, REG_SHS_LSB, &val), "no write to SHS LSB logged");
	zassert_equal(val, 0x0e, "SHS LSB default");
	zassert_true(imx296_test_first_write(emul, REG_SHS_MID, &val), "no write to SHS mid logged");
	zassert_equal(val, 0x00, "SHS mid byte default");
	zassert_true(imx296_test_first_write(emul, REG_SHS_MSB, &val), "no write to SHS MSB logged");
	zassert_equal(val, 0x00, "SHS MSB default");

	zassert_true(imx296_test_first_write(emul, REG_GAIN_LSB, &val), "no write to GAIN LSB logged");
	zassert_equal(val, 0x00, "GAIN LSB default (0 dB)");
	zassert_true(imx296_test_first_write(emul, REG_GAIN_MSB, &val), "no write to GAIN MSB logged");
	zassert_equal(val, 0x00, "GAIN MSB default (0 dB)");

	zassert_true(imx296_test_first_write(emul, REG_REVERSE, &val), "no write to REVERSE logged");
	zassert_equal(val, 0x00, "REVERSE default (no flip)");
}

ZTEST(imx296, test_roi_mode_explicitly_disabled_at_init)
{
	/* FID0_ROIH1ON/FID0_ROIV1ON's POR default is already 0 (disabled), so a final-value read
	 * of REG_ROI_ENABLE cannot tell "imx296_init() wrote 0" from "imx296_init() never touched
	 * this register" -- checked via the write LOG instead (see imx296_test_first_write()
	 * above), which only has an entry if imx296_init() actually issued the write. */
	const struct emul *emul = imx296_emul();
	uint8_t            val;

	zassert_true(imx296_test_first_write(emul, REG_ROI_ENABLE, &val),
	             "imx296_init() did not write REG_ROI_ENABLE to force All-pixel scan mode");
	zassert_equal(val, 0x00, "ROI_ENABLE (All-pixel scan mode, ROI disabled)");
}

ZTEST(imx296, test_init_quiesce_order)
{
	/* "Slave Mode and Master Mode" (page 55) + "Standby mode" (page 54): imx296_init() must
	 * stop master-mode (XMSTA=stop) and arm STANDBY before touching anything else, then
	 * briefly CANCEL standby to read SENSOR_INFO, then RE-ARM standby before writing the
	 * "S" (standby-only) registers VMAX/HMAX/INCKSEL/CSI_TIMING -- a values-only check (see
	 * test_init_register_values above) cannot see this ordering, only the write log can.
	 * This must run before any test clears the log (i.e. before test_stream_start_... below)
	 * so it observes the pristine boot-time sequence.
	 */
	const struct emul       *emul = imx296_emul();
	struct imx296_emul_write w;

	zassert_true(imx296_emul_log_count(emul) >= 6,
	             "expected at least 6 writes by the end of imx296_init()'s quiesce+identity "
	             "round trip, got %zu",
	             imx296_emul_log_count(emul));

	zassert_ok(imx296_emul_log_get(emul, 0, &w));
	zassert_equal(w.reg, REG_XMSTA, "write 0 should be XMSTA (stop)");
	zassert_equal(w.value, XMSTA_STOP, "write 0: XMSTA should be set to stop");

	zassert_ok(imx296_emul_log_get(emul, 1, &w));
	zassert_equal(w.reg, REG_STANDBY, "write 1 should be STANDBY (arm)");
	zassert_equal(w.value, STANDBY_STANDBY, "write 1: STANDBY should be armed");

	zassert_ok(imx296_emul_log_get(emul, 2, &w));
	zassert_equal(w.reg, REG_STANDBY, "write 2 should be STANDBY (cancel, to read SENSOR_INFO)");
	zassert_equal(w.value, 0, "write 2: STANDBY should be cancelled");

	zassert_ok(imx296_emul_log_get(emul, 3, &w));
	zassert_equal(w.reg,
	              REG_STANDBY,
	              "write 3 should be STANDBY (re-arm, before the VMAX/HMAX/INCKSEL 'S' "
	              "registers)");
	zassert_equal(w.value, STANDBY_STANDBY, "write 3: STANDBY should be re-armed");

	/* Every VMAX/HMAX/INCKSEL/CSI_TIMING/CSI_LANE_HS/BLKLEVEL/ROI_ENABLE write must come AFTER
	 * the re-arm at index 3 -- writing an "S" register while standby is still cancelled would
	 * not be what the datasheet's "S" (set during standby) annotation documents. */
	for (size_t i = 0; i < imx296_emul_log_count(emul); i++) {
		zassert_ok(imx296_emul_log_get(emul, i, &w));
		if (w.reg == REG_VMAX_LSB || w.reg == REG_HMAX_LSB || w.reg == REG_INCKSEL0 ||
		    w.reg == REG_CSI_TIMING || w.reg == REG_CSI_LANE_HS || w.reg == REG_BLKLEVEL_LSB ||
		    w.reg == REG_ROI_ENABLE) {
			zassert_true(i > 3,
			             "write %zu (reg 0x%04x) is an 'S' register but landed before "
			             "the standby re-arm at index 3",
			             i,
			             w.reg);
		}
	}
}

ZTEST(imx296, test_get_caps_lists_exactly_one_mode)
{
	struct video_caps caps = { .type = VIDEO_BUF_TYPE_OUTPUT };
	int               count;

	zassert_ok(video_get_caps(imx296_dev(), &caps));
	zassert_not_null(caps.format_caps);

	for (count = 0; caps.format_caps[count].pixelformat != 0; count++) {
		zassert_equal(caps.format_caps[count].pixelformat,
		              VIDEO_PIX_FMT_SRGGB10P,
		              "mode %d is not SRGGB10P",
		              count);
		zassert_equal(caps.format_caps[count].width_min, 1456, "mode %d width", count);
		zassert_equal(caps.format_caps[count].width_max, 1456, "mode %d width", count);
		zassert_equal(caps.format_caps[count].height_min, 1088, "mode %d height", count);
		zassert_equal(caps.format_caps[count].height_max, 1088, "mode %d height", count);
	}
	zassert_equal(
	    count, 1, "the sensor's single fixed All-pixel scan mode should be the only entry");
}

ZTEST(imx296, test_set_format_succeeds_for_the_one_mode)
{
	struct video_format fmt = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SRGGB10P,
		.width       = 1456,
		.height      = 1088,
	};

	zassert_ok(video_set_format(imx296_dev(), &fmt));
}

ZTEST(imx296, test_set_format_rejects_unsupported_fourcc)
{
	struct video_format fmt = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_GREY,
		.width       = 1456,
		.height      = 1088,
	};

	zassert_equal(video_set_format(imx296_dev(), &fmt),
	              -ENOTSUP,
	              "GREY at the one valid size should be rejected -- this sensor is RAW10 only");
}

ZTEST(imx296, test_set_format_rejects_unsupported_size)
{
	struct video_format fmt = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SRGGB10P,
		.width       = 1440,
		.height      = 1080,
	};

	zassert_equal(video_set_format(imx296_dev(), &fmt),
	              -ENOTSUP,
	              "1440x1080 (the datasheet's RECORDING crop, not the TRANSMITTED frame) "
	              "should be rejected -- the driver's one mode is 1456x1088");
}

ZTEST(imx296, test_stream_start_cancels_standby_then_starts_master_mode)
{
	const struct emul       *emul = imx296_emul();
	struct imx296_emul_write w;

	imx296_emul_clear_log(emul);

	zassert_ok(video_stream_start(imx296_dev(), VIDEO_BUF_TYPE_OUTPUT));

	/* "Standby mode" (page 54) + "Slave Mode and Master Mode" (page 55): cancel STANDBY
	 * first (and wait out the regulator-settle time -- not observable via the register log),
	 * THEN start master-mode free-run by clearing XMSTA. Reversing this order would start
	 * the master-mode clock generator while the analog block is still in standby. The
	 * TRIGEN/LOWLAGTRG/SYNCSEL writes (issue #2287, see imx296.c's imx296_set_stream())
	 * land first, before STANDBY, since "Mode Transitions of Global Shutter Operation"
	 * (page 66) requires them to be set "via sensor standby". video_stream_start() above also
	 * blocks for IMX296_INIT_PERIOD_MS (issue #2287) after the XMSTA write -- likewise no
	 * register write, so likewise invisible to this log, but native_sim's simulated clock
	 * fast-forwards k_sleep() so it costs no real wall-clock time in this test. */
	zassert_equal(imx296_emul_log_count(emul), 5, "expected exactly 5 writes to start streaming");

	zassert_ok(imx296_emul_log_get(emul, 0, &w));
	zassert_equal(w.reg, REG_TRIGEN, "write 0 should be TRIGEN");
	zassert_equal(w.value, 0, "TRIGEN should stay 0 (normal mode) at the free-run default");

	zassert_ok(imx296_emul_log_get(emul, 1, &w));
	zassert_equal(w.reg, REG_LOWLAGTRG, "write 1 should be LOWLAGTRG");
	zassert_equal(w.value, 0, "LOWLAGTRG should stay 0 at the free-run default");

	zassert_ok(imx296_emul_log_get(emul, 2, &w));
	zassert_equal(w.reg, REG_SYNCSEL, "write 2 should be SYNCSEL");
	zassert_equal(w.value, SYNCSEL_NORMAL, "SYNCSEL should be 0xC0 (Normal Output)");

	zassert_ok(imx296_emul_log_get(emul, 3, &w));
	zassert_equal(w.reg, REG_STANDBY, "write 3 should be STANDBY");
	zassert_equal(w.value, 0, "STANDBY should be cancelled (0) to start streaming");

	zassert_ok(imx296_emul_log_get(emul, 4, &w));
	zassert_equal(w.reg, REG_XMSTA, "write 4 should be XMSTA");
	zassert_equal(w.value, 0, "XMSTA should be cleared (master-mode start) to start streaming");
}

ZTEST(imx296, test_trigger_mode_ctrl_default_is_free_run)
{
	/* IMX296_CID_TRIGGER_MODE's control-registry default (video_init_ctrl(), imx296.c) must
	 * be free-run, matching "default = free-run (current behaviour unchanged)". */
	struct video_control ctrl = { .id = IMX296_CID_TRIGGER_MODE, .val = -1 };

	zassert_ok(video_get_ctrl(imx296_dev(), &ctrl));
	zassert_equal(ctrl.val, IMX296_TRIGGER_MODE_FREE_RUN, "trigger-mode ctrl should default off");
}

ZTEST(imx296, test_trigger_mode_ctrl_rejects_out_of_range)
{
	struct video_control over = { .id = IMX296_CID_TRIGGER_MODE, .val = 2 };

	zassert_equal(video_set_ctrl(imx296_dev(), &over),
	              -EINVAL,
	              "trigger-mode ctrl only has two valid values, 0 and 1");
}

ZTEST(imx296, test_trigger_mode_ctrl_rejects_while_streaming)
{
	/*
	 * "Mode Transitions of Global Shutter Operation" (page 66): the TRIGEN/LOWLAGTRG switch
	 * can only be made "via sensor standby" -- there is no standby to make it through while
	 * already streaming, so imx296_set_ctrl() must reject with -EBUSY rather than silently
	 * queuing the change for a later stop/start.
	 */
	struct video_control trigger_on = { .id = IMX296_CID_TRIGGER_MODE,
		                            .val = IMX296_TRIGGER_MODE_EXTERNAL };
	struct video_control readback   = { .id = IMX296_CID_TRIGGER_MODE, .val = -1 };

	zassert_ok(video_stream_start(imx296_dev(), VIDEO_BUF_TYPE_OUTPUT));

	zassert_equal(video_set_ctrl(imx296_dev(), &trigger_on),
	              -EBUSY,
	              "trigger-mode ctrl must reject a change while streaming");

	zassert_ok(video_get_ctrl(imx296_dev(), &readback));
	zassert_equal(readback.val,
	              IMX296_TRIGGER_MODE_FREE_RUN,
	              "a rejected set_ctrl must leave the control's cached value unchanged");

	zassert_ok(video_stream_stop(imx296_dev(), VIDEO_BUF_TYPE_OUTPUT));
}

ZTEST(imx296, test_trigger_mode_writes_trigen_and_lowlagtrg_on_next_stream_start)
{
	/*
	 * "Global Shutter (Fast Trigger Mode) Operation" -> "Register List of shutter setting"
	 * (page 64): TRIGEN=1 (0x300B) + LOWLAGTRG=1 (0x30AE) select fast trigger mode. Setting
	 * the ctrl alone (video_set_ctrl()) must NOT touch hardware yet -- imx296.c's
	 * IMX296_CID_TRIGGER_MODE case is a deferred no-op, since the datasheet requires the
	 * switch to happen "via sensor standby" (page 66), i.e. only from imx296_set_stream().
	 * This also exercises "switching back": stopping and restarting with the ctrl reset to
	 * free-run must restore TRIGEN=0/LOWLAGTRG=0.
	 */
	const struct emul       *emul = imx296_emul();
	struct video_control     trigger_on  = { .id  = IMX296_CID_TRIGGER_MODE,
	                                         .val = IMX296_TRIGGER_MODE_EXTERNAL };
	struct video_control     trigger_off = { .id  = IMX296_CID_TRIGGER_MODE,
	                                         .val = IMX296_TRIGGER_MODE_FREE_RUN };
	struct imx296_emul_write w;
	uint8_t                  val;

	zassert_ok(video_stream_stop(imx296_dev(), VIDEO_BUF_TYPE_OUTPUT));

	imx296_emul_clear_log(emul);
	zassert_ok(video_set_ctrl(imx296_dev(), &trigger_on));
	zassert_equal(imx296_emul_log_count(emul),
	              0,
	              "video_set_ctrl() alone must not write TRIGEN/LOWLAGTRG yet");

	zassert_ok(video_stream_start(imx296_dev(), VIDEO_BUF_TYPE_OUTPUT));

	zassert_ok(imx296_emul_get_reg(emul, REG_TRIGEN, &val));
	zassert_equal(val, TRIGEN_TRIGGER, "TRIGEN should be set for fast trigger mode");
	zassert_ok(imx296_emul_get_reg(emul, REG_LOWLAGTRG, &val));
	zassert_equal(val, LOWLAGTRG_FAST, "LOWLAGTRG should select fast trigger mode");
	zassert_ok(imx296_emul_get_reg(emul, REG_SYNCSEL, &val));
	zassert_equal(val, SYNCSEL_NORMAL, "SYNCSEL should still be 0xC0 in trigger mode");

	/* Switch back: stop, reset the ctrl to free-run, restart -- TRIGEN/LOWLAGTRG must be
	 * restored to their free-run values, not left armed from the previous stream-start. */
	zassert_ok(video_stream_stop(imx296_dev(), VIDEO_BUF_TYPE_OUTPUT));
	zassert_ok(video_set_ctrl(imx296_dev(), &trigger_off));
	imx296_emul_clear_log(emul);
	zassert_ok(video_stream_start(imx296_dev(), VIDEO_BUF_TYPE_OUTPUT));

	zassert_true(imx296_test_first_write(emul, REG_TRIGEN, &val), "no write to TRIGEN logged");
	zassert_equal(val, 0, "TRIGEN should be restored to 0 (normal mode) switching back");
	zassert_true(imx296_test_first_write(emul, REG_LOWLAGTRG, &val),
	             "no write to LOWLAGTRG logged");
	zassert_equal(val, 0, "LOWLAGTRG should be restored to 0 switching back");

	zassert_ok(imx296_emul_log_get(emul, 0, &w));
	zassert_equal(w.reg, REG_TRIGEN, "TRIGEN write should still land first on switch-back");
}

ZTEST(imx296, test_stream_stop_stops_master_mode_then_re_arms_standby)
{
	const struct emul       *emul = imx296_emul();
	struct imx296_emul_write w;

	imx296_emul_clear_log(emul);

	zassert_ok(video_stream_stop(imx296_dev(), VIDEO_BUF_TYPE_OUTPUT));

	/* Mirror image of the start sequence: stop master-mode free-run FIRST (XMSTA = stop),
	 * THEN re-arm STANDBY -- the same order imx296_init()'s idempotent quiesce uses. */
	zassert_equal(imx296_emul_log_count(emul), 2, "expected exactly 2 writes to stop streaming");

	zassert_ok(imx296_emul_log_get(emul, 0, &w));
	zassert_equal(w.reg, REG_XMSTA, "write 0 should be XMSTA");
	zassert_equal(w.value, XMSTA_STOP, "XMSTA should be set to stop (1)");

	zassert_ok(imx296_emul_log_get(emul, 1, &w));
	zassert_equal(w.reg, REG_STANDBY, "write 1 should be STANDBY");
	zassert_equal(w.value, STANDBY_STANDBY, "STANDBY should be re-armed (1)");
}

ZTEST(imx296, test_exposure_ctrl_programs_shs_inverted)
{
	const struct emul   *emul = imx296_emul();
	struct video_control ctrl = { .id = VIDEO_CID_EXPOSURE, .val = EXPOSURE_MAX };
	uint8_t              val;

	/* "Calculation Formula of Exposure Time" (page 60): exposure ascends with more
	 * integration time, which is the INVERSE of SHS -- shs = VMAX - exposure. At the
	 * ceiling (VMAX - 4), shs should be exactly 4. */
	zassert_ok(video_set_ctrl(imx296_dev(), &ctrl));

	zassert_ok(imx296_emul_get_reg(emul, REG_SHS_LSB, &val));
	zassert_equal(val, 4, "SHS LSB");
	zassert_ok(imx296_emul_get_reg(emul, REG_SHS_MID, &val));
	zassert_equal(val, 0, "SHS mid byte");
	zassert_ok(imx296_emul_get_reg(emul, REG_SHS_MSB, &val));
	zassert_equal(val, 0, "SHS MSB");
}

ZTEST(imx296, test_exposure_above_ceiling_is_rejected)
{
	struct video_control over       = { .id = VIDEO_CID_EXPOSURE, .val = EXPOSURE_MAX + 1 };
	struct video_control at_ceiling = { .id = VIDEO_CID_EXPOSURE, .val = EXPOSURE_MAX };

	zassert_equal(video_set_ctrl(imx296_dev(), &over),
	              -EINVAL,
	              "exposure 1 line past the SHS_MIN=4 ceiling should be rejected");
	zassert_ok(video_set_ctrl(imx296_dev(), &at_ceiling),
	           "exposure at the ceiling should be accepted");
}

ZTEST(imx296, test_gain_ctrl_programs_the_register_le)
{
	const struct emul   *emul = imx296_emul();
	struct video_control ctrl = { .id = VIDEO_CID_ANALOGUE_GAIN, .val = GAIN_MAX };
	uint8_t              val;

	/* "Gain Adjustment Function" (page 56): GAIN[8:0], 0.1 dB step, 0-480 (48.0 dB). GAIN is
	 * little-endian (IMX296_REG16, unlike OV5647's big-endian chip-ID register) -- 480 =
	 * 0x01E0, so the LOW byte (0xE0) lives at the base address and the HIGH byte (0x01) one
	 * above it. */
	zassert_ok(video_set_ctrl(imx296_dev(), &ctrl));

	zassert_ok(imx296_emul_get_reg(emul, REG_GAIN_LSB, &val));
	zassert_equal(val, 0xe0, "GAIN LSB");
	zassert_ok(imx296_emul_get_reg(emul, REG_GAIN_MSB, &val));
	zassert_equal(val, 0x01, "GAIN MSB");
}

ZTEST(imx296, test_gain_above_ceiling_is_rejected)
{
	struct video_control over = { .id = VIDEO_CID_ANALOGUE_GAIN, .val = GAIN_MAX + 1 };

	zassert_equal(
	    video_set_ctrl(imx296_dev(), &over), -EINVAL, "gain past 480 (48.0 dB) should be rejected");
}

ZTEST(imx296, test_hflip_vflip_set_the_reverse_bits_independently)
{
	const struct emul   *emul      = imx296_emul();
	struct video_control hflip_on  = { .id = VIDEO_CID_HFLIP, .val = 1 };
	struct video_control hflip_off = { .id = VIDEO_CID_HFLIP, .val = 0 };
	struct video_control vflip_on  = { .id = VIDEO_CID_VFLIP, .val = 1 };
	uint8_t              val;

	/* "Horizontal / Vertical Normal Operation and Inverted Operation" (page 58): HREVERSE is
	 * REG_REVERSE bit1, VREVERSE is bit0 -- video_modify_cci_reg() is a read-modify-write, so
	 * setting one must not disturb the other. */
	zassert_ok(video_set_ctrl(imx296_dev(), &hflip_on));
	zassert_ok(imx296_emul_get_reg(emul, REG_REVERSE, &val));
	zassert_true(val & REVERSE_HREVERSE, "HREVERSE bit not set: 0x%02x", val);
	zassert_false(val & REVERSE_VREVERSE, "VREVERSE should still be clear: 0x%02x", val);

	zassert_ok(video_set_ctrl(imx296_dev(), &vflip_on));
	zassert_ok(imx296_emul_get_reg(emul, REG_REVERSE, &val));
	zassert_true(val & REVERSE_HREVERSE, "HREVERSE should survive VFLIP set: 0x%02x", val);
	zassert_true(val & REVERSE_VREVERSE, "VREVERSE bit not set: 0x%02x", val);

	zassert_ok(video_set_ctrl(imx296_dev(), &hflip_off));
	zassert_ok(imx296_emul_get_reg(emul, REG_REVERSE, &val));
	zassert_false(val & REVERSE_HREVERSE, "HREVERSE bit stuck on: 0x%02x", val);
	zassert_true(val & REVERSE_VREVERSE, "VREVERSE should survive HFLIP clear: 0x%02x", val);
}

/*
 * ZTEST_SUITE after-hook: stops the stream once every test in this suite finishes. Several tests
 * above (test_stream_start_..., the trigger-mode tests) leave the emulated sensor streaming;
 * without this, ztest's NAME-SORTED (not declaration) execution order would let whichever test
 * happens to run last before another leak its streaming state into it -- most concretely,
 * IMX296_CID_TRIGGER_MODE's -EBUSY-while-streaming rejection (test_trigger_mode_ctrl_rejects_
 * while_streaming) would otherwise depend on whichever streaming state the PREVIOUS test left
 * behind, rather than the state it itself sets up. Not asserted: a stream that was never started
 * this test still stops cleanly (imx296_set_stream(dev, false, ...) is unconditional), so this
 * exists purely to leave a known state for the next test, not to check anything about this one.
 */
static void imx296_test_after(void *fixture)
{
	ARG_UNUSED(fixture);

	(void)video_stream_stop(imx296_dev(), VIDEO_BUF_TYPE_OUTPUT);
}

ZTEST_SUITE(imx296, NULL, NULL, NULL, imx296_test_after, NULL);
