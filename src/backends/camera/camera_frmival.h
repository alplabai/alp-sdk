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
 *   - video_set_frmival() failing is graded by WHO asked, not by the errno,
 *     because a backend-only default must never brick an open() the caller
 *     never asked to fail (this restores alif_isp_pico.c's pre-#2278
 *     tolerance -- a transient SCCB NAK on the OV5647's default 10 fps
 *     request used to only LOG_WRN, never fail open()):
 *       - requested_fps == 0 (only a backend default asked): ANY negative
 *         rc -- ENOSYS, a transient I2C/SCCB NAK, anything -- is a quiet
 *         LOG_WRN + ALP_OK.
 *       - requested_fps != 0 (the CALLER explicitly asked): -ENOSYS /
 *         -ENOTSUP ("this device has no frame-rate control at all") is a
 *         loud decline (ALP_ERR_NOSUPPORT + LOG_ERR); any other negative rc
 *         maps through the shared errno baseline (alp_errno.h) and the
 *         caller's open() fails.
 *   - on success, read back with video_get_frmival() rather than trust the
 *     write-back (a driver may clamp to its own supported-rate table, e.g.
 *     the OV5647's {10,15,30,45,60,90,120}) -- and compare that readback
 *     against the ORIGINAL request, not a struct video_frmival that was
 *     passed BY POINTER into video_set_frmival() and may have been
 *     overwritten in place with the clamped value (video_sw_generator.c's
 *     set_frmival does exactly this).  A failed read-back, or one that
 *     reports a zero interval (some drivers write back {0,0} on a path
 *     that isn't really an error), falls back to reporting the original
 *     request as settled and only warns.
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
 *         any other video_set_frmival() failure the CALLER asked to incur
 *         (a backend-only default never fails open() -- see the policy
 *         above).
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

	/* video_set_frmival() takes its argument BY POINTER and some drivers
	 * (video_sw_generator.c's set_frmival, for one) overwrite it in place
	 * with the clamped/settled value -- pass a COPY so `request` still
	 * holds what was actually asked for when comparing against the later
	 * read-back. */
	const struct video_frmival request = { .numerator = 1, .denominator = effective };
	struct video_frmival       set_arg = request;
	int                        rc      = video_set_frmival(dev, &set_arg);

	if (rc != 0) {
		if (requested_fps == 0u) {
			/* Only the backend's own default asked for this rate --
			 * don't fail an open() the caller never asked to fail,
			 * no matter what video_set_frmival() returned (ENOSYS,
			 * a transient I2C/SCCB NAK, ...) -- restores the
			 * tolerance alif_isp_pico.c had before #2278. */
			LOG_WRN("camera%u: backend default of %u fps failed: rc=%d", camera_id, effective, rc);
			return ALP_OK;
		}
		if (rc == -ENOSYS || rc == -ENOTSUP) {
			LOG_ERR("camera%u: %u fps requested but this device does not "
			        "support frame-rate control",
			        camera_id,
			        requested_fps);
			return ALP_ERR_NOSUPPORT;
		}
		return alp_status_from_zephyr_errno(rc);
	}

	/* Don't trust the write-back -- a driver may clamp to its own
	 * supported-rate table, so read back what actually landed.  A failed
	 * read-back, or one reporting a zero interval, falls back to the
	 * ORIGINAL request (not set_arg, which set_frmival may have already
	 * overwritten). */
	struct video_frmival actual = { 0 };
	int                  get_rc = video_get_frmival(dev, &actual);
	if (get_rc != 0 || actual.numerator == 0u || actual.denominator == 0u) {
		LOG_WRN("camera%u: video_get_frmival() %s after a successful "
		        "video_set_frmival(); reporting the %u fps request as settled",
		        camera_id,
		        (get_rc != 0) ? "failed" : "returned a zero interval",
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
