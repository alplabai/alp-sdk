/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Runtime ztest for the OV9281 streaming driver (zephyr/drivers/video/ov9281.c), against the
 * I2C emulator in ov9281_emul.c on native_sim's real "zephyr,i2c-emul-controller" (&i2c0 --
 * see app.overlay). OV5647 and IMX296 stay build-only (no emulator): see testcase.yaml.
 */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/video-controls.h>
#include <zephyr/drivers/video.h>
#include <zephyr/ztest.h>

#include "ov9281_emul.h"

#define OV9281_NODE DT_NODELABEL(ov9281_test)

/* Register addresses this test peeks -- kept local rather than pulling ov9281.c's private
 * OV9281_REG8()-wrapped values, which also encode CCI address/data-size metadata this test
 * doesn't need. */
#define REG_OUT_WIDTH_H  0x3808
#define REG_OUT_WIDTH_L  0x3809
#define REG_OUT_HEIGHT_H 0x380a
#define REG_OUT_HEIGHT_L 0x380b
#define REG_VTS_H        0x380e
#define REG_VTS_L        0x380f
#define REG_TEST_PATTERN 0x5e00
#define TEST_PATTERN_ENABLE_BIT BIT(7)

static const struct device *ov9281_dev(void)
{
	return DEVICE_DT_GET(OV9281_NODE);
}

static const struct emul *ov9281_emul(void)
{
	return EMUL_DT_GET(OV9281_NODE);
}

ZTEST(ov9281, test_device_is_ready)
{
	zassert_true(device_is_ready(ov9281_dev()), "OV9281 device not ready");
}

ZTEST(ov9281, test_get_caps_lists_exactly_three_grey_modes)
{
	struct video_caps caps = {.type = VIDEO_BUF_TYPE_OUTPUT};
	static const struct {
		uint32_t width;
		uint32_t height;
	} expect[] = {
		{640, 400},
		{1280, 720},
		{1280, 800},
	};
	int count;

	zassert_ok(video_get_caps(ov9281_dev(), &caps));
	zassert_not_null(caps.format_caps);

	for (count = 0; caps.format_caps[count].pixelformat != 0; count++) {
		zassert_equal(caps.format_caps[count].pixelformat, VIDEO_PIX_FMT_GREY,
			     "mode %d is not GREY", count);
	}
	zassert_equal(count, 3, "expected exactly 3 modes, got %d", count);

	for (int i = 0; i < ARRAY_SIZE(expect); i++) {
		bool found = false;

		for (int j = 0; j < count; j++) {
			if (caps.format_caps[j].width_min == expect[i].width &&
			    caps.format_caps[j].width_max == expect[i].width &&
			    caps.format_caps[j].height_min == expect[i].height &&
			    caps.format_caps[j].height_max == expect[i].height) {
				found = true;
				break;
			}
		}
		zassert_true(found, "%ux%u mode missing from get_caps", expect[i].width,
			    expect[i].height);
	}
}

static void set_format_ok(uint32_t width, uint32_t height)
{
	struct video_format fmt = {
		.type = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_GREY,
		.width = width,
		.height = height,
	};

	zassert_ok(video_set_format(ov9281_dev(), &fmt), "set_format(%ux%u GREY) failed", width,
		  height);
}

ZTEST(ov9281, test_set_format_succeeds_for_each_supported_mode)
{
	set_format_ok(640, 400);
	set_format_ok(1280, 720);
	set_format_ok(1280, 800);
}

ZTEST(ov9281, test_set_format_1280x800_programs_the_derived_registers)
{
	const struct emul *emul = ov9281_emul();
	uint8_t val;

	/* ov9281_set_fmt() skips the register reload when the requested mode is already active
	 * (see the comment in ov9281.c) -- switch to a different mode first so the 1280x800
	 * load below is a real one this test can check, independent of what earlier tests left
	 * active. */
	set_format_ok(640, 400);
	set_format_ok(1280, 800);

	/* Output width stays 1280 (0x0500), unchanged from the 1280x720 table */
	zassert_ok(ov9281_emul_get_reg(emul, REG_OUT_WIDTH_H, &val));
	zassert_equal(val, 0x05, "0x%04x", REG_OUT_WIDTH_H);
	zassert_ok(ov9281_emul_get_reg(emul, REG_OUT_WIDTH_L, &val));
	zassert_equal(val, 0x00, "0x%04x", REG_OUT_WIDTH_L);

	/* Output height 800 = 0x0320 */
	zassert_ok(ov9281_emul_get_reg(emul, REG_OUT_HEIGHT_H, &val));
	zassert_equal(val, 0x03, "0x%04x", REG_OUT_HEIGHT_H);
	zassert_ok(ov9281_emul_get_reg(emul, REG_OUT_HEIGHT_L, &val));
	zassert_equal(val, 0x20, "0x%04x", REG_OUT_HEIGHT_L);

	/* VTS 910 = 0x038e */
	zassert_ok(ov9281_emul_get_reg(emul, REG_VTS_H, &val));
	zassert_equal(val, 0x03, "0x%04x", REG_VTS_H);
	zassert_ok(ov9281_emul_get_reg(emul, REG_VTS_L, &val));
	zassert_equal(val, 0x8e, "0x%04x", REG_VTS_L);
}

ZTEST(ov9281, test_set_format_rejects_unsupported_fourcc)
{
	struct video_format fmt = {
		.type = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width = 1280,
		.height = 800,
	};

	zassert_equal(video_set_format(ov9281_dev(), &fmt), -ENOTSUP,
		     "RAW10 at a valid size should be rejected");
}

ZTEST(ov9281, test_set_format_rejects_unsupported_size)
{
	struct video_format fmt = {
		.type = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_GREY,
		.width = 800,
		.height = 600,
	};

	zassert_equal(video_set_format(ov9281_dev(), &fmt), -ENOTSUP,
		     "an unlisted 800x600 GREY size should be rejected");
}

ZTEST(ov9281, test_pattern_ctrl_sets_the_register_bit)
{
	const struct emul *emul = ov9281_emul();
	struct video_control ctrl = {.id = VIDEO_CID_TEST_PATTERN, .val = 1};
	uint8_t val;

	zassert_ok(video_set_ctrl(ov9281_dev(), &ctrl));
	zassert_ok(ov9281_emul_get_reg(emul, REG_TEST_PATTERN, &val));
	zassert_true(val & TEST_PATTERN_ENABLE_BIT, "TEST_PATTERN bit7 not set: 0x%02x", val);

	ctrl.val = 0;
	zassert_ok(video_set_ctrl(ov9281_dev(), &ctrl));
	zassert_ok(ov9281_emul_get_reg(emul, REG_TEST_PATTERN, &val));
	zassert_false(val & TEST_PATTERN_ENABLE_BIT, "TEST_PATTERN bit7 stuck on: 0x%02x", val);
}

ZTEST(ov9281, test_exposure_above_ceiling_is_rejected)
{
	/* 1280x800's vts = 910, so the exposure ceiling (vts - OV9281_EXP_MAX_OFFSET) is 885 --
	 * see ov9281_init_ctrls()/ov9281_set_fmt() in ov9281.c. video_set_ctrl() range-checks
	 * against that ceiling and rejects out-of-range values with -EINVAL before the driver's
	 * own defensive CLAMP() ever runs (video_ctrls.c:video_set_ctrl()). */
	struct video_control over = {.id = VIDEO_CID_EXPOSURE, .val = 886};
	struct video_control at_ceiling = {.id = VIDEO_CID_EXPOSURE, .val = 885};

	/* Force a real reload (see the comment in the register-check test above) so the
	 * exposure ceiling below is freshly computed by this test's own 1280x800 load, not
	 * inherited from whatever mode an earlier test left active. */
	set_format_ok(640, 400);
	set_format_ok(1280, 800);

	zassert_equal(video_set_ctrl(ov9281_dev(), &over), -EINVAL,
		     "exposure 1 row past the ceiling should be rejected");
	zassert_ok(video_set_ctrl(ov9281_dev(), &at_ceiling), "exposure at the ceiling should be accepted");
}

ZTEST_SUITE(ov9281, NULL, NULL, NULL, NULL, NULL);
