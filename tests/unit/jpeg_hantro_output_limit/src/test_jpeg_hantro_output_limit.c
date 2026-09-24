/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Review of the camera-mjpeg-stream example (#2265) found that
 * jpeg_start_encode() (zephyr/drivers/video/jpeg_hantro_vc9000e.c) pointed
 * the SWREG8 output stream base at buf->buffer + header_size, but programmed
 * the SWREG9 size limit with the full buf->size -- the HW could then write
 * up to header_size bytes past the end of the caller's output buffer. The
 * fix lives in the pure, dependency-free jpeg_hantro_vc9000e_output_limit()
 * helper (jpeg_hantro_vc9000e_limit.h) so it's host/native_sim
 * unit-testable without the jpeg0 hardware or a devicetree.
 */
#include <errno.h>

#include <zephyr/ztest.h>

#include "jpeg_hantro_vc9000e_limit.h"

ZTEST_SUITE(jpeg_hantro_output_limit, NULL, NULL, NULL, NULL, NULL);

ZTEST(jpeg_hantro_output_limit, test_limit_is_size_minus_header)
{
	uint32_t limit = 0;

	/* Kconfig default header size (623) against a realistic output
	 * capacity.
	 */
	zassert_equal(jpeg_hantro_vc9000e_output_limit(4096, 623, &limit), 0);
	zassert_equal(limit, 4096 - 623);
}

ZTEST(jpeg_hantro_output_limit, test_buffer_exactly_header_size_rejected)
{
	uint32_t limit = 0xdeadbeef;

	/* buf_size == header_size leaves zero bytes for compressed data --
	 * reject, and leave limit_out untouched (not zeroed, not wrapped).
	 */
	zassert_equal(jpeg_hantro_vc9000e_output_limit(623, 623, &limit), -ENOBUFS);
	zassert_equal(limit, 0xdeadbeef, "limit_out must be untouched on error");
}

ZTEST(jpeg_hantro_output_limit, test_buffer_smaller_than_header_rejected)
{
	uint32_t limit = 0xdeadbeef;

	/* The regression case: a buffer smaller than the header. Pre-fix,
	 * buf_size - header_size wrapped to a huge uint32_t and was written
	 * straight to SWREG9, re-opening the overrun this guard exists to
	 * close.
	 */
	zassert_equal(jpeg_hantro_vc9000e_output_limit(100, 623, &limit), -ENOBUFS);
	zassert_equal(limit, 0xdeadbeef, "limit_out must be untouched on error");
}

ZTEST(jpeg_hantro_output_limit, test_one_byte_of_payload_room_accepted)
{
	uint32_t limit = 0;

	/* Boundary: exactly one byte past the header is the smallest
	 * accepted buffer.
	 */
	zassert_equal(jpeg_hantro_vc9000e_output_limit(624, 623, &limit), 0);
	zassert_equal(limit, 1);
}
