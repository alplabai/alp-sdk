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
 *
 * Every encode() call is captured into a local before any zassert on
 * it, and alp_jpeg_close() runs unconditionally before the asserts --
 * CONFIG_ALP_SDK_MAX_JPEG_HANDLES has no Kconfig symbol at all (just
 * the src/jpeg_dispatch.c #ifndef fallback of 1), so a handle leaked
 * on an aborted zassert starves every later test in this suite and
 * they report "h is NULL" instead of their own failure (#1645 review).
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

	alp_status_t rc_zero = alp_jpeg_encode(h, &req, out, sizeof(out), &out_len);

	req.y_stride               = 32u; /* smaller than width -- reads across row boundaries */
	alp_status_t rc_undersized = alp_jpeg_encode(h, &req, out, sizeof(out), &out_len);

	req.y_stride       = 64u; /* valid */
	alp_status_t rc_ok = alp_jpeg_encode(h, &req, out, sizeof(out), &out_len);

	alp_jpeg_close(h);

	zassert_equal(
	    rc_zero, ALP_ERR_INVAL, "a zero stride must be refused, not encoded as row 0 repeated");
	zassert_equal(rc_undersized, ALP_ERR_INVAL, "a stride below width must be refused");
	zassert_not_equal(rc_ok, ALP_ERR_INVAL, "a correct stride must still be accepted");
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

	alp_status_t rc_undersized = alp_jpeg_encode(h, &req, out, sizeof(out), &out_len);

	req.u_stride       = 32u; /* valid */
	alp_status_t rc_ok = alp_jpeg_encode(h, &req, out, sizeof(out), &out_len);

	alp_jpeg_close(h);

	zassert_equal(rc_undersized, ALP_ERR_INVAL, "an undersized chroma stride must be refused");
	zassert_not_equal(rc_ok, ALP_ERR_INVAL, "a correct chroma stride must still be accepted");
}

ZTEST(alp_peripheral, test_jpeg_enforces_advertised_max)
{
	alp_jpeg_config_t cfg = ALP_JPEG_CONFIG_DEFAULT;
	alp_jpeg_t       *h   = alp_jpeg_open(&cfg);
	zassert_not_null(h);

	alp_jpeg_caps_t caps;
	alp_status_t    caps_rc = alp_jpeg_capabilities(h, &caps);

	/* Otherwise-fully-valid request (real plane, a stride that always
	 * satisfies the declared width) so only the dimension under test can
	 * be the reason for a rejection -- a request that also fails the
	 * stride/format checks would pass this test on either check-order,
	 * silently stopping being a range-check test (#1645 review). */
	static uint8_t        plane[8];
	static uint8_t        out[256];
	size_t                out_len = 0;
	alp_jpeg_encode_req_t req     = {
		.width     = 16u,
		.height    = 16u,
		.format    = ALP_PIXFMT_YUV420_PLANAR,
		.subsample = ALP_JPEG_SUBSAMPLE_400,
		.quality   = 80,
		.y_plane   = plane,
		.y_stride  = 16u,
	};

	/* uint16_t width/height can't express a value above UINT16_MAX, so a
	 * backend advertising UINT16_MAX (e.g. alif_hantro.c, unreachable on
	 * native_sim) has no representable out-of-range request -- skip that
	 * dimension rather than silently wrapping to 0 (#1645 review). */
	bool         width_testable = caps.max_width < UINT16_MAX;
	alp_status_t rc_width       = ALP_OK;
	if (width_testable) {
		req.width    = (uint16_t)(caps.max_width + 1u);
		req.y_stride = req.width;
		rc_width     = alp_jpeg_encode(h, &req, out, sizeof(out), &out_len);
		req.width    = 16u;
		req.y_stride = 16u;
	}

	bool         height_testable = caps.max_height < UINT16_MAX;
	alp_status_t rc_height       = ALP_OK;
	if (height_testable) {
		req.height = (uint16_t)(caps.max_height + 1u);
		rc_height  = alp_jpeg_encode(h, &req, out, sizeof(out), &out_len);
	}

	alp_jpeg_close(h);

	zassert_equal(caps_rc, ALP_OK);
	if (width_testable) {
		zassert_equal(
		    rc_width, ALP_ERR_OUT_OF_RANGE, "width above the advertised max_width must be refused");
	}
	if (height_testable) {
		zassert_equal(rc_height,
		              ALP_ERR_OUT_OF_RANGE,
		              "height above the advertised max_height must be refused");
	}
}
