/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake video controller backing the alp,test-video-frmival-err DT node
 * (see ../dts/bindings/alp,test-video-frmival-err.yaml).  Implements ONLY
 * .set_frmival, and it always fails with -EIO -- a non-ENOSYS error --
 * so the test source can call camera_apply_fps() (camera_frmival.h)
 * directly against a device whose frame-rate control exists but is
 * transiently/permanently broken, distinct from alp,test-video-no-frmival
 * (which has no frame-rate control AT ALL, i.e. -ENOSYS).  Not wired to
 * any alp-cameraN alias -- this device is never opened through
 * alp_camera_open(), only referenced directly in the test source (#2278
 * round 2).
 */

#define DT_DRV_COMPAT alp_test_video_frmival_err

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/video.h>

static int test_video_frmival_err_set_frmival(const struct device  *dev,
                                              struct video_frmival *frmival)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(frmival);
	return -EIO;
}

static DEVICE_API(video, test_video_frmival_err_api) = {
	.set_frmival = test_video_frmival_err_set_frmival,
};

static int test_video_frmival_err_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

#define TEST_VIDEO_FRMIVAL_ERR_INIT(n) \
	DEVICE_DT_INST_DEFINE(n, \
	                      test_video_frmival_err_init, \
	                      NULL, \
	                      NULL, \
	                      NULL, \
	                      POST_KERNEL, \
	                      CONFIG_VIDEO_INIT_PRIORITY, \
	                      &test_video_frmival_err_api);

DT_INST_FOREACH_STATUS_OKAY(TEST_VIDEO_FRMIVAL_ERR_INIT)
