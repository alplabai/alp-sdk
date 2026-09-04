/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pins src/jpeg_dispatch.c's alp_jpeg_encode() 0-caps exemption (issue
 * #1645): a backend that advertises max_width == 0 / max_height == 0 (no
 * real bound to advertise -- e.g. zephyr_stub.c) must NOT be rejected by
 * the OUT_OF_RANGE width/height check; the request has to still reach the
 * backend's own encode().
 *
 * tests/unit/jpeg_registry always arbitrates to sw_baseline (priority 50)
 * over zephyr_stub (priority 0), so that suite never actually exercises
 * this branch. This test registers its own priority-255 "*"-silicon_ref
 * double (the reserved test-double priority documented on
 * ALP_BACKEND_REGISTER in <alp/backend.h>) so it deterministically wins
 * arbitration regardless of which real jpeg backends are also linked in.
 * It lives in its OWN test executable, separate from jpeg_registry's,
 * because backend registration is link-wide: sharing a binary with
 * jpeg_registry's suite would hijack every alp_jpeg_open() there too and
 * break its sw_baseline-backed assertions.
 */
#include <string.h>

#include <zephyr/ztest.h>
#include <alp/backend.h>
#include <alp/jpeg.h>

#include "backends/jpeg/jpeg_ops.h"

static alp_status_t
_open(const alp_jpeg_config_t *cfg, alp_jpeg_backend_state_t *state, alp_jpeg_caps_t *caps_out)
{
	ARG_UNUSED(cfg);
	ARG_UNUSED(state);
	/* max_width/max_height left at 0 -- the "no bound advertised"
	 * sentinel, same as zephyr_stub.c. */
	*caps_out = (alp_jpeg_caps_t){
		.hw_accelerated  = false,
		.mjpeg_supported = false,
		.subsample_mask  = (1u << ALP_JPEG_SUBSAMPLE_420),
		.pixfmt_mask     = (1u << ALP_PIXFMT_YUV420_PLANAR),
	};
	return ALP_OK;
}

static alp_status_t _encode(alp_jpeg_backend_state_t    *state,
                            const alp_jpeg_encode_req_t *req,
                            void                        *out_buf,
                            size_t                       out_cap,
                            size_t                      *out_len)
{
	ARG_UNUSED(state);
	ARG_UNUSED(req);
	ARG_UNUSED(out_buf);
	ARG_UNUSED(out_cap);
	ARG_UNUSED(out_len);
	/* Reaching this at all is the thing under test -- the return value
	 * just needs to be distinguishable from ALP_ERR_OUT_OF_RANGE, the
	 * status the dispatcher would have returned had the exemption
	 * regressed. Mirrors zephyr_stub.c's own documented behaviour. */
	return ALP_ERR_NOT_IMPLEMENTED;
}

static void _close(alp_jpeg_backend_state_t *state)
{
	ARG_UNUSED(state);
}

static const alp_jpeg_ops_t _ops = {
	.open   = _open,
	.encode = _encode,
	.close  = _close,
};

ALP_BACKEND_REGISTER(jpeg,
                     unit_zero_caps,
                     {
                         .silicon_ref = "*",
                         .vendor      = "alp/testing",
                         .base_caps   = 0u,
                         .priority    = 255u,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

ZTEST(jpeg_zero_caps_exempt, test_zero_caps_backend_is_exempt_from_out_of_range)
{
	alp_jpeg_config_t cfg = ALP_JPEG_CONFIG_DEFAULT;
	alp_jpeg_t       *h   = alp_jpeg_open(&cfg);
	zassert_not_null(h, "open must succeed");

	alp_jpeg_caps_t caps;
	zassert_equal(alp_jpeg_capabilities(h, &caps), ALP_OK);
	zassert_equal(caps.max_width, 0, "test double advertises no bound");
	zassert_equal(caps.max_height, 0, "test double advertises no bound");

	uint8_t               out[16];
	size_t                out_len = 0;
	alp_jpeg_encode_req_t req     = { .width     = 60000,
		                              .height    = 60000,
		                              .format    = ALP_PIXFMT_YUV420_PLANAR,
		                              .subsample = ALP_JPEG_SUBSAMPLE_420,
		                              .quality   = 75 };
	/* Far beyond any real backend's advertised max_width/max_height --
	 * if the 0-caps exemption regressed, this comes back
	 * ALP_ERR_OUT_OF_RANGE instead of reaching _encode() above. */
	zassert_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len),
	              ALP_ERR_NOT_IMPLEMENTED,
	              "0-caps backend must still be reached, not rejected as OUT_OF_RANGE");

	alp_jpeg_close(h);
}

ZTEST_SUITE(jpeg_zero_caps_exempt, NULL, NULL, NULL, NULL, NULL);
