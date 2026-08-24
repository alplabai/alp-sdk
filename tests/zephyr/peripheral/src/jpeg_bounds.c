/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/jpeg.h> -- caller-geometry bounds tests (#1645).  Before this
 * fix, alp_jpeg_encode() checked only non-NULL pointers and non-zero
 * width/height: a y_stride of 0 aliased every row to row 0 in
 * sw_baseline (returning ALP_OK with a wrong-but-valid JPEG), a
 * stride below width read across row boundaries, and the backend's
 * advertised max_width/max_height were never enforced against the
 * caller's request.  native_sim always wins the sw_baseline backend
 * (priority 50, no HW competitor), so these tests exercise the
 * dispatcher-level checks in src/jpeg_dispatch.c directly -- the
 * Alif Hantro HW backend (src/backends/jpeg/alif_hantro.c) is
 * Kconfig-gated to real E8 silicon and does not build here at all.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <alp/jpeg.h>
#include <alp/peripheral.h>

ZTEST(alp_peripheral, test_jpeg_rejects_zero_stride)
{
	static uint8_t plane[64 * 64];
	static uint8_t out[8192];
	size_t         out_len = 0;

	alp_jpeg_config_t cfg = ALP_JPEG_CONFIG_DEFAULT;
	alp_jpeg_t       *h   = alp_jpeg_open(&cfg);
	zassert_not_null(h, "sw_baseline is the priority-50 default on native_sim");

	alp_jpeg_encode_req_t req = {
		.width     = 64u,
		.height    = 64u,
		.format    = ALP_PIXFMT_YUV420_PLANAR,
		.subsample = ALP_JPEG_SUBSAMPLE_400, /* mono -- no chroma planes needed */
		.quality   = 80,
		.y_plane   = plane,
		.y_stride  = 0u, /* the defect: aliases every row to row 0 */
	};

	zassert_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len),
	              ALP_ERR_INVAL,
	              "a zero stride must be refused, not encoded as row 0 repeated");

	req.y_stride = 32u; /* smaller than width -- reads across row boundaries */
	zassert_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len),
	              ALP_ERR_INVAL,
	              "a stride below width must be refused");

	req.y_stride = 64u; /* valid */
	zassert_not_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len),
	                  ALP_ERR_INVAL,
	                  "a correct stride must still be accepted");

	alp_jpeg_close(h);
}

ZTEST(alp_peripheral, test_jpeg_rejects_undersized_chroma_stride)
{
	/* 4:2:0 chroma planes are half-width: a u_stride/v_stride below
	 * width/2 must be refused too, exercised separately from the luma
	 * check above so a fix that only guards y_stride doesn't pass. */
	static uint8_t y[64 * 64];
	static uint8_t u[32 * 32];
	static uint8_t v[32 * 32];
	static uint8_t out[8192];
	size_t         out_len = 0;

	alp_jpeg_config_t cfg = ALP_JPEG_CONFIG_DEFAULT;
	alp_jpeg_t       *h   = alp_jpeg_open(&cfg);
	zassert_not_null(h);

	alp_jpeg_encode_req_t req = {
		.width     = 64u,
		.height    = 64u,
		.format    = ALP_PIXFMT_YUV420_PLANAR,
		.subsample = ALP_JPEG_SUBSAMPLE_420,
		.quality   = 80,
		.y_plane   = y,
		.y_stride  = 64u,
		.u_plane   = u,
		.u_stride  = 16u, /* below width/2 (32) -- reads across row boundaries */
		.v_plane   = v,
		.v_stride  = 32u,
	};

	zassert_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len),
	              ALP_ERR_INVAL,
	              "an undersized chroma stride must be refused");

	req.u_stride = 32u; /* valid */
	zassert_not_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len),
	                  ALP_ERR_INVAL,
	                  "a correct chroma stride must still be accepted");

	alp_jpeg_close(h);
}

ZTEST(alp_peripheral, test_jpeg_enforces_advertised_max)
{
	alp_jpeg_config_t cfg = ALP_JPEG_CONFIG_DEFAULT;
	alp_jpeg_t       *h   = alp_jpeg_open(&cfg);
	zassert_not_null(h);

	alp_jpeg_caps_t caps;
	zassert_equal(alp_jpeg_capabilities(h, &caps), ALP_OK);

	static uint8_t        out[256];
	size_t                out_len = 0;
	alp_jpeg_encode_req_t req     = {
		.width  = (uint16_t)(caps.max_width + 1u),
		.height = 16u,
	};
	zassert_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len),
	              ALP_ERR_OUT_OF_RANGE,
	              "width above the advertised max_width must be refused");

	alp_jpeg_close(h);
}
