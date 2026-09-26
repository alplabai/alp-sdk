/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Alp Lab AB
 *
 * Shared alp_camera_config_t::fps -> struct video_frmival policy (#2278).
 *
 * Before this header, alif_isp_pico.c was the only camera backend that read
 * cfg->fps at all -- zephyr_video.c and v2n_n44_isp.c silently ignored it
 * (#2276 fixed the former, #2278 covers the other two).  Rather than hand-
 * copy alif_isp_pico.c's fps block into two more backends and let all three
 * drift the way _errno_to_alp() drifted 27 times before #1638, the shared
 * policy lives here once and every camera backend that owns a frame-
 * interval-capable device calls it.
 *
 * Policy:
 *   - effective fps = requested_fps if nonzero, else default_fps.  If BOTH
 *     are 0, the device is left untouched -- *settled comes back {0, 0}.
 *   - video_set_frmival() -ENOSYS / -ENOTSUP ("this device has no frame-
 *     rate control at all"): a caller-requested nonzero fps is a loud
 *     decline (ALP_ERR_NOSUPPORT + LOG_ERR); a backend-only default is a
 *     quiet LOG_WRN + ALP_OK -- a caller who left fps at 0 didn't ask for
 *     anything to fail their open() over.
 *   - any other negative rc maps through the shared errno baseline
 *     (alp_errno.h) and is returned as-is -- the caller's open() fails.
 *   - on success, read back with video_get_frmival() rather than trust the
 *     write-back (a driver may clamp to its own supported-rate table, e.g.
 *     the OV5647's {10,15,30,45,60,90,120}); a failed read-back falls back
 *     to reporting the request as settled and only warns.
 *
 * Header-only (mirrors src/backends/camera/yuv_to_rgb565.h's shape) so the
 * three call sites (zephyr_video.c, v2n_n44_isp.c, alif_isp_pico.c) share
 * one body instead of three copies.  Logging macros resolve against
 * whichever LOG_MODULE_REGISTER() the including .c file declared before
 * pulling this header in, same as the file-scoped statics that pattern
 * always relied on.
 */

#ifndef ALP_BACKENDS_CAMERA_FRMIVAL_H
#define ALP_BACKENDS_CAMERA_FRMIVAL_H

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/logging/log.h>

#include <alp/peripheral.h>

#include "alp_errno.h"

/**
 * @brief Apply @c alp_camera_config_t::fps to @p dev, honoring or loudly
 *        declining the request (#2278).
 *
 * @param[in]  dev            Frame-interval-capable video device to
 *                             program (the sensor endpoint, not
 *                             necessarily the camera_id's top-level
 *                             device -- e.g. alif_isp_pico.c passes its
 *                             OV5647 sensor device, not the ISP `dev`).
 * @param[in]  camera_id       Portable camera_id, log lines only.
 * @param[in]  requested_fps   @c cfg->fps as the caller set it; 0 means
 *                             "no explicit request".
 * @param[in]  default_fps     Backend's own fallback when @p requested_fps
 *                             is 0; 0 means the backend has no opinion
 *                             either, so the device is left untouched.
 * @param[out] settled         Receives the frame interval actually in
 *                             effect afterward.  {0, 0} if this call chose
 *                             not to touch the device.  May be NULL.
 *
 * @return ALP_OK, or ALP_ERR_NOSUPPORT when @p requested_fps is nonzero and
 *         @p dev has no frame-rate control at all, or the mapped status of
 *         any other video_set_frmival() failure.
 */
static inline alp_status_t camera_apply_fps(const struct device  *dev,
                                            uint32_t              camera_id,
                                            uint8_t               requested_fps,
                                            uint8_t               default_fps,
                                            struct video_frmival *settled)
{
	if (settled != NULL) {
		*settled = (struct video_frmival){ 0 };
	}

	uint8_t effective = (requested_fps != 0u) ? requested_fps : default_fps;
	if (effective == 0u) {
		/* Neither the caller nor the backend has an opinion here --
		 * leave the device at whatever rate it powered up with. */
		return ALP_OK;
	}

	struct video_frmival request = { .numerator = 1, .denominator = effective };
	int                  rc      = video_set_frmival(dev, &request);

	if (rc == -ENOSYS || rc == -ENOTSUP) {
		if (requested_fps != 0u) {
			LOG_ERR("camera%u: %u fps requested but this device does not "
			        "support frame-rate control",
			        camera_id,
			        requested_fps);
			return ALP_ERR_NOSUPPORT;
		}
		/* Only the backend's own default asked for this rate --
		 * don't fail an open() the caller never asked to fail. */
		LOG_WRN("camera%u: backend default of %u fps requested but this "
		        "device does not support frame-rate control",
		        camera_id,
		        effective);
		return ALP_OK;
	}
	if (rc != 0) {
		return alp_status_from_zephyr_errno(rc);
	}

	/* Don't trust the write-back -- a driver may clamp to its own
	 * supported-rate table, so read back what actually landed. */
	struct video_frmival actual = { 0 };
	if (video_get_frmival(dev, &actual) != 0) {
		LOG_WRN("camera%u: video_get_frmival() failed after a successful "
		        "video_set_frmival(); reporting the %u fps request as settled",
		        camera_id,
		        effective);
		actual = request;
	}

	if (settled != NULL) {
		*settled = actual;
	}

	bool settled_as_requested =
	    (actual.numerator == request.numerator) && (actual.denominator == request.denominator);
	if (settled_as_requested) {
		LOG_DBG("camera%u: requested %u fps, settled on %u/%u",
		        camera_id,
		        effective,
		        actual.denominator,
		        actual.numerator);
	} else {
		LOG_INF("camera%u: requested %u fps, settled on %u/%u",
		        camera_id,
		        effective,
		        actual.denominator,
		        actual.numerator);
	}

	return ALP_OK;
}

#endif /* ALP_BACKENDS_CAMERA_FRMIVAL_H */
