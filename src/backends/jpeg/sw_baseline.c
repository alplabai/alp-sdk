/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software baseline-JPEG fallback backend for <alp/jpeg.h>.  Wraps the
 * vendored TooJpeg-derived encoder (src/backends/jpeg/vendor/) so every
 * SoM without a hardware JPEG engine still gets a real encode path.
 * Registers "*" at priority 50: outranks the NOT_IMPLEMENTED stub
 * (priority 0, src/backends/jpeg/zephyr_stub.c) everywhere, and loses
 * to the Alif Hantro VC9000E hardware backend (priority 100,
 * "alif:ensemble:e8") once that lands.
 *
 * ponytail: baseline sequential, 4:2:0 + 4:0:0 only -- no MJPEG,
 * progressive, 4:2:2, or rate-control.  Swap for libjpeg-turbo only if
 * a customer measurably needs more than this gets them.
 */

#include <stddef.h>

#include <alp/backend.h>
#include <alp/jpeg.h>
#include <alp/peripheral.h>

#include "jpeg_ops.h"
#include "vendor/toojpeg_baseline.h"

static alp_status_t
sw_open(const alp_jpeg_config_t *cfg, alp_jpeg_backend_state_t *state, alp_jpeg_caps_t *caps_out)
{
	(void)cfg;
	state->be_data = NULL;
	*caps_out      = (alp_jpeg_caps_t){
		.hw_accelerated  = false,
		.mjpeg_supported = false,
		.max_width       = 16384u,
		.max_height      = 16384u,
		.subsample_mask  = (1u << ALP_JPEG_SUBSAMPLE_400) | (1u << ALP_JPEG_SUBSAMPLE_420),
		/* Only layout this backend understands: real fully-planar Y/U/V
		 * (also covers subsample _400 -- a mono frame is just this same
		 * layout with u_plane/v_plane NULL, not a distinct pixfmt). */
		.pixfmt_mask = (1u << ALP_PIXFMT_YUV420_PLANAR),
	};
	return ALP_OK;
}

static alp_status_t sw_encode(alp_jpeg_backend_state_t    *state,
                              const alp_jpeg_encode_req_t *req,
                              void                        *out_buf,
                              size_t                       out_cap,
                              size_t                      *out_len)
{
	(void)state;

	if (req->format != ALP_PIXFMT_YUV420_PLANAR) {
		return ALP_ERR_NOSUPPORT;
	}
	if (req->subsample == ALP_JPEG_SUBSAMPLE_422) {
		return ALP_ERR_NOSUPPORT;
	}

	int mono = (req->subsample == ALP_JPEG_SUBSAMPLE_400);
	if (req->y_plane == NULL || (!mono && (req->u_plane == NULL || req->v_plane == NULL))) {
		return ALP_ERR_INVAL;
	}

	size_t written = toojpeg_encode_yuv420(out_buf,
	                                       out_cap,
	                                       (const uint8_t *)req->y_plane,
	                                       req->y_stride,
	                                       (const uint8_t *)req->u_plane,
	                                       req->u_stride,
	                                       (const uint8_t *)req->v_plane,
	                                       req->v_stride,
	                                       req->width,
	                                       req->height,
	                                       mono,
	                                       req->quality);
	if (written == (size_t)-1) {
		return ALP_ERR_NOMEM;
	}
	if (written == 0) {
		return ALP_ERR_IO;
	}
	*out_len = written;
	return ALP_OK;
}

static void sw_close(alp_jpeg_backend_state_t *state)
{
	(void)state;
}

static const alp_jpeg_ops_t _ops = {
	.open   = sw_open,
	.encode = sw_encode,
	.close  = sw_close,
};

/* Carries the class's one-per-class static-archive anchor (see
 * docs/architecture/backend-registry.md "Static-archive anchors").  Placed
 * here rather than zephyr_stub.c on purpose: on the plain-CMake baremetal/
 * Yocto static links this backend is unconditionally compiled (Task 2) and
 * permanently outranks the priority-0 stub for every silicon_ref, so it is
 * the one that must survive a real consumer's member-pulling static link --
 * the stub becomes harmless dead weight there and needs no anchor of its
 * own.  On Zephyr the anchor macros are no-ops either way (whole-object
 * zephyr_library link). */
ALP_BACKEND_ANCHOR_DEFINE(jpeg);
ALP_BACKEND_REGISTER(jpeg,
                     sw_baseline,
                     {
                         .silicon_ref = "*",
                         .vendor      = "alp",
                         .base_caps   = 0u,
                         .priority    = 50u,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
