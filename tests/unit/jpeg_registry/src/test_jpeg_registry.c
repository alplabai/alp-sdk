/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

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
	/* native_sim has no HW backend and (as of Task 2) no HW-only class
	 * either: the software baseline backend (priority 50, "*") now wins
	 * arbitration over the NOT_IMPLEMENTED stub (priority 0) here same
	 * as everywhere else, so open()/caps() exercise a REAL backend.
	 * This test only cares about the open/encode/close LIFECYCLE guard,
	 * not the encode result, so it deliberately omits the y/u/v planes
	 * -- sw_baseline's own encode-success path is covered separately by
	 * test_sw_baseline_encodes_valid_jpeg below. */
	alp_jpeg_config_t cfg = ALP_JPEG_CONFIG_DEFAULT;
	alp_jpeg_t       *h   = alp_jpeg_open(&cfg);
	zassert_not_null(h, "open must succeed");

	alp_jpeg_caps_t caps;
	zassert_equal(alp_jpeg_capabilities(h, &caps), ALP_OK);
	zassert_false(caps.hw_accelerated, "software backend is not hw");
	zassert_equal(alp_jpeg_capabilities(NULL, &caps), ALP_ERR_INVAL);

	uint8_t               out[16];
	size_t                out_len = 0;
	alp_jpeg_encode_req_t req     = {
		.width = 16, .height = 16, .subsample = ALP_JPEG_SUBSAMPLE_420, .quality = 75
	};
	/* No y/u/v planes supplied -- sw_baseline rejects with INVAL. */
	zassert_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len), ALP_ERR_INVAL);

	alp_jpeg_close(h);
	zassert_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len),
	              ALP_ERR_NOT_READY,
	              "encode after close must be gated");
	alp_jpeg_close(h); /* idempotent -- must not fault */
}

ZTEST(jpeg_registry, test_sw_baseline_encodes_valid_jpeg)
{
	/* 16x16 solid mid-grey YUV420: Y=128, U=V=128. */
	static uint8_t y[16 * 16], u[8 * 8], v[8 * 8];
	memset(y, 128, sizeof(y));
	memset(u, 128, sizeof(u));
	memset(v, 128, sizeof(v));

	alp_jpeg_config_t cfg = ALP_JPEG_CONFIG_DEFAULT;
	alp_jpeg_t       *h   = alp_jpeg_open(&cfg);
	zassert_not_null(h);

	alp_jpeg_caps_t caps;
	alp_jpeg_capabilities(h, &caps);
	zassert_false(caps.hw_accelerated);
	zassert_true(caps.subsample_mask & (1u << ALP_JPEG_SUBSAMPLE_420));

	uint8_t               out[4096];
	size_t                out_len = 0;
	alp_jpeg_encode_req_t req     = {
		.width     = 16,
		.height    = 16,
		.subsample = ALP_JPEG_SUBSAMPLE_420,
		.quality   = 80,
		.y_plane   = y,
		.y_stride  = 16,
		.u_plane   = u,
		.u_stride  = 8,
		.v_plane   = v,
		.v_stride  = 8,
	};
	zassert_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len), ALP_OK);
	zassert_true(out_len > 4, "empty output");
	/* SOI ffd8 ... EOI ffd9 markers. */
	zassert_equal(out[0], 0xFF);
	zassert_equal(out[1], 0xD8);
	zassert_equal(out[out_len - 2], 0xFF);
	zassert_equal(out[out_len - 1], 0xD9);

	/* 4:2:2 not supported in software -> NOSUPPORT, not a silent resample. */
	req.subsample = ALP_JPEG_SUBSAMPLE_422;
	zassert_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len), ALP_ERR_NOSUPPORT);
	alp_jpeg_close(h);
}

ZTEST(jpeg_registry, test_sw_baseline_overflow_returns_nomem)
{
	/* Same 16x16 solid mid-grey YUV420 frame as
	 * test_sw_baseline_encodes_valid_jpeg, but into a deliberately
	 * too-small buffer -- locks down the tj_put_byte() `pos >= cap`
	 * guard -> toojpeg_encode_yuv420() returns (size_t)-1 ->
	 * ALP_ERR_NOMEM, the exact path an attacker-sized frame hits. */
	static uint8_t y[16 * 16], u[8 * 8], v[8 * 8];
	memset(y, 128, sizeof(y));
	memset(u, 128, sizeof(u));
	memset(v, 128, sizeof(v));

	alp_jpeg_config_t cfg = ALP_JPEG_CONFIG_DEFAULT;
	alp_jpeg_t       *h   = alp_jpeg_open(&cfg);
	zassert_not_null(h);

	uint8_t               out[8];
	size_t                out_len = 0;
	alp_jpeg_encode_req_t req     = {
		.width     = 16,
		.height    = 16,
		.subsample = ALP_JPEG_SUBSAMPLE_420,
		.quality   = 80,
		.y_plane   = y,
		.y_stride  = 16,
		.u_plane   = u,
		.u_stride  = 8,
		.v_plane   = v,
		.v_stride  = 8,
	};
	zassert_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len), ALP_ERR_NOMEM);
	alp_jpeg_close(h);
}

ZTEST(jpeg_registry, test_sw_baseline_encodes_mono_400)
{
	/* 8x8 solid mid-grey Y-only frame, u_plane/v_plane NULL -- the
	 * advertised ALP_JPEG_SUBSAMPLE_400 (mono) capability. */
	static uint8_t y[8 * 8];
	memset(y, 128, sizeof(y));

	alp_jpeg_config_t cfg = ALP_JPEG_CONFIG_DEFAULT;
	alp_jpeg_t       *h   = alp_jpeg_open(&cfg);
	zassert_not_null(h);

	uint8_t               out[4096];
	size_t                out_len = 0;
	alp_jpeg_encode_req_t req     = {
		.width     = 8,
		.height    = 8,
		.subsample = ALP_JPEG_SUBSAMPLE_400,
		.quality   = 80,
		.y_plane   = y,
		.y_stride  = 8,
		.u_plane   = NULL,
		.v_plane   = NULL,
	};
	zassert_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len), ALP_OK);
	zassert_true(out_len > 4, "empty output");
	zassert_equal(out[0], 0xFF);
	zassert_equal(out[1], 0xD8, "missing SOI");
	zassert_equal(out[out_len - 2], 0xFF);
	zassert_equal(out[out_len - 1], 0xD9, "missing EOI");

	alp_jpeg_close(h);
}

ZTEST(jpeg_registry, test_sw_baseline_marker_structure_well_ordered)
{
	/* Same 16x16 solid mid-grey 4:2:0 frame as
	 * test_sw_baseline_encodes_valid_jpeg. Without pulling in a decoder,
	 * assert the baseline-JPEG marker skeleton is present and in the
	 * right order between SOI and EOI: DQT, SOF0, DHT, SOS. Catches
	 * structural corruption (e.g. a wrong marker length miscounting
	 * bytes) that a bare SOI/EOI check would miss. */
	static uint8_t y[16 * 16], u[8 * 8], v[8 * 8];
	memset(y, 128, sizeof(y));
	memset(u, 128, sizeof(u));
	memset(v, 128, sizeof(v));

	alp_jpeg_config_t cfg = ALP_JPEG_CONFIG_DEFAULT;
	alp_jpeg_t       *h   = alp_jpeg_open(&cfg);
	zassert_not_null(h);

	uint8_t               out[4096];
	size_t                out_len = 0;
	alp_jpeg_encode_req_t req     = {
		.width     = 16,
		.height    = 16,
		.subsample = ALP_JPEG_SUBSAMPLE_420,
		.quality   = 80,
		.y_plane   = y,
		.y_stride  = 16,
		.u_plane   = u,
		.u_stride  = 8,
		.v_plane   = v,
		.v_stride  = 8,
	};
	zassert_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len), ALP_OK);
	zassert_equal(out[0], 0xFF);
	zassert_equal(out[1], 0xD8, "missing SOI");
	zassert_equal(out[out_len - 2], 0xFF);
	zassert_equal(out[out_len - 1], 0xD9, "missing EOI");

	static const uint8_t markers[] = { 0xDB, 0xC0, 0xC4, 0xDA };
	size_t               pos       = 2; /* past SOI */
	for (size_t m = 0; m < ARRAY_SIZE(markers); m++) {
		int found = 0;
		while (pos + 1 < out_len - 2) {
			if (out[pos] == 0xFF && out[pos + 1] == markers[m]) {
				found = 1;
				pos += 2;
				break;
			}
			pos++;
		}
		zassert_true(found, "marker 0xFF%02X not found in order", markers[m]);
	}
	alp_jpeg_close(h);
}

ZTEST_SUITE(jpeg_registry, NULL, NULL, NULL, NULL, NULL);
