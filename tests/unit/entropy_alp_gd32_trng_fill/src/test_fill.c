/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32_trng_fill(): the chunking + error policy of the V2N GD32 TRNG entropy
 * driver (#2951), against a stub bridge read.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "entropy_alp_gd32_trng_fill.h"

struct stub {
	size_t  calls;
	size_t  max_seen;
	size_t  fail_on_call; /* 1-based; 0 = never */
	int     fail_rc;
	uint8_t next;
};

static int stub_read(void *ctx, uint8_t *buf, size_t len)
{
	struct stub *s = ctx;

	s->calls++;
	if (len > s->max_seen) {
		s->max_seen = len;
	}
	if (s->fail_on_call != 0u && s->calls == s->fail_on_call) {
		return s->fail_rc;
	}
	for (size_t i = 0; i < len; i++) {
		buf[i] = s->next++;
	}
	return 0;
}

ZTEST(gd32_trng_fill, test_chunks_and_fills_whole_buffer)
{
	struct stub s = { .next = 1 };
	uint8_t     buf[70];

	memset(buf, 0, sizeof(buf));
	zassert_equal(gd32_trng_fill(stub_read, &s, buf, sizeof(buf), 32u), 0);
	zassert_equal(s.calls, 3u, "70 B at 32 B/chunk = 32+32+6");
	zassert_equal(s.max_seen, 32u);
	for (size_t i = 0; i < sizeof(buf); i++) {
		zassert_equal(buf[i], (uint8_t)(i + 1u), "byte %zu", i);
	}
}

ZTEST(gd32_trng_fill, test_zero_length_is_ok_and_reads_nothing)
{
	struct stub s = { 0 };
	uint8_t     b;

	zassert_equal(gd32_trng_fill(stub_read, &s, &b, 0u, 32u), 0);
	zassert_equal(s.calls, 0u);
}

ZTEST(gd32_trng_fill, test_mid_fill_failure_is_an_error_not_a_short_success)
{
	struct stub s = { .fail_on_call = 2u, .fail_rc = -EIO };
	uint8_t     buf[64];

	zassert_equal(gd32_trng_fill(stub_read, &s, buf, sizeof(buf), 32u), -EIO);
}

ZTEST(gd32_trng_fill, test_positive_rc_maps_to_eio)
{
	struct stub s = { .fail_on_call = 1u, .fail_rc = 7 };
	uint8_t     b[4];

	zassert_equal(gd32_trng_fill(stub_read, &s, b, sizeof(b), 32u), -EIO);
}

ZTEST(gd32_trng_fill, test_bad_args)
{
	uint8_t b[4];

	zassert_equal(gd32_trng_fill(NULL, NULL, b, 4u, 32u), -EINVAL);
	zassert_equal(gd32_trng_fill(stub_read, NULL, NULL, 4u, 32u), -EINVAL);
	zassert_equal(gd32_trng_fill(stub_read, NULL, b, 4u, 0u), -EINVAL);
}

ZTEST_SUITE(gd32_trng_fill, NULL, NULL, NULL, NULL, NULL);
