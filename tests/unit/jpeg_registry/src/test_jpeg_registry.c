/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/ztest.h>
#include <alp/jpeg.h>
#include <alp/backend.h>

ZTEST(jpeg_registry, test_class_has_a_backend)
{
	/* At least the stub registers for class "jpeg". */
	zassert_true(ALP_BACKEND_AVAILABLE(jpeg), "no jpeg backend linked");
}

ZTEST(jpeg_registry, test_open_then_close_stub)
{
	/* native_sim has no HW backend; open resolves the stub, whose
	 * encode returns ALP_ERR_NOT_IMPLEMENTED but open/caps still work. */
	alp_jpeg_config_t cfg = ALP_JPEG_CONFIG_DEFAULT;
	alp_jpeg_t       *h   = alp_jpeg_open(&cfg);
	zassert_not_null(h, "stub open must succeed");

	alp_jpeg_caps_t caps;
	zassert_equal(alp_jpeg_capabilities(h, &caps), ALP_OK);
	zassert_false(caps.hw_accelerated, "stub is not hw");

	uint8_t               out[16];
	size_t                out_len = 0;
	alp_jpeg_encode_req_t req     = {
		.width = 16, .height = 16, .subsample = ALP_JPEG_SUBSAMPLE_420, .quality = 75
	};
	zassert_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len), ALP_ERR_NOT_IMPLEMENTED);
	alp_jpeg_close(h);
}

ZTEST_SUITE(jpeg_registry, NULL, NULL, NULL, NULL, NULL);
