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

#define IMX296_NODE DT_NODELABEL(imx296_test)

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

#define STANDBY_STANDBY  BIT(0)
#define XMSTA_STOP       BIT(0)
#define REVERSE_VREVERSE BIT(0)
#define REVERSE_HREVERSE BIT(1)

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
	 * the master-mode clock generator while the analog block is still in standby. */
	zassert_equal(imx296_emul_log_count(emul), 2, "expected exactly 2 writes to start streaming");

	zassert_ok(imx296_emul_log_get(emul, 0, &w));
	zassert_equal(w.reg, REG_STANDBY, "write 0 should be STANDBY");
	zassert_equal(w.value, 0, "STANDBY should be cancelled (0) to start streaming");

	zassert_ok(imx296_emul_log_get(emul, 1, &w));
	zassert_equal(w.reg, REG_XMSTA, "write 1 should be XMSTA");
	zassert_equal(w.value, 0, "XMSTA should be cleared (master-mode start) to start streaming");
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

ZTEST_SUITE(imx296, NULL, NULL, NULL, NULL, NULL);
