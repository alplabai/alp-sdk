/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Apply alp_camera_config_t::fps to a Zephyr video device (#2278).
 *
 * Shared by the Zephyr video-class camera backends (zephyr_video.c,
 * v2n_n44_isp.c). fps == 0 keeps the driver's own default. A nonzero fps
 * is a request: the driver settles on the nearest rate it supports, and a
 * driver with no settable rate (or one that rejects the request) is logged
 * and the stream keeps running at the driver's rate -- opening never fails
 * on the frame rate alone, matching alif_isp_pico.c.
 *
 * The including translation unit must LOG_MODULE_REGISTER() before it
 * includes this header.
 */

#ifndef ALP_BACKENDS_CAMERA_FRMIVAL_H
#define ALP_BACKENDS_CAMERA_FRMIVAL_H

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/logging/log.h>

static inline void alp_camera_apply_fps(const struct device *dev, uint32_t camera_id, uint8_t fps)
{
	if (fps == 0u) {
		return;
	}

	struct video_frmival frmival = { .numerator = 1u, .denominator = fps };
	int                  rc      = video_set_frmival(dev, &frmival);

	if (rc == -ENOSYS || rc == -ENOTSUP) {
		LOG_WRN("camera%u: driver has no settable frame rate; fps request (%u) not applied",
		        camera_id,
		        fps);
	} else if (rc != 0) {
		LOG_WRN("camera%u: video_set_frmival(%u fps) failed: rc=%d", camera_id, fps, rc);
	}
}

/* The rate the driver actually settled on, x 1000 (#2279); 0 when the
 * driver cannot report its frame interval.  Store it in the handle's
 * state.fps_x1000 at open for alp_camera_get_fps(). */
static inline uint32_t alp_camera_read_fps_x1000(const struct device *dev)
{
	struct video_frmival frmival = { 0 };

	if (video_get_frmival(dev, &frmival) != 0 || frmival.numerator == 0u) {
		return 0u;
	}
	return (uint32_t)(((uint64_t)frmival.denominator * 1000u + frmival.numerator / 2u) /
	                  frmival.numerator);
}

#endif /* ALP_BACKENDS_CAMERA_FRMIVAL_H */
