/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression test for issue #594: the non-Zephyr `alp_delay_us` /
 * `alp_delay_ms` fallback (then in src/common/stub_backend.c, now
 * split out into src/common/stub/stub_core.c -- see #673) used to
 * return far earlier than requested (an under-calibrated busy-loop
 * measured ~0.6 ms for a requested 100 ms).  Exercises both the
 * ALP_OS=yocto and ALP_OS=baremetal plain-CMake builds -- both link
 * src/common/stub/stub_core.c's delay fallback (neither has a real
 * vendor HAL delay override today), and on a Linux CI host (the only
 * host these plain-CMake builds run on) that fallback is the same
 * clock_nanosleep(CLOCK_MONOTONIC) path in both configurations.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto     -DALP_BUILD_TESTS=ON   (or baremetal)
 *   cmake --build build --target alp_test_delay
 *   ctest --test-dir build -R alp_test_delay
 */

#include <stdint.h>

#include "alp/peripheral.h"

#include "test_assert.h"

#if defined(__linux__)
#include <time.h>

/* Returns b - a in whole microseconds (both CLOCK_MONOTONIC samples). */
static int64_t z_elapsed_us(const struct timespec *a, const struct timespec *b)
{
	int64_t sec_diff  = (int64_t)b->tv_sec - (int64_t)a->tv_sec;
	int64_t nsec_diff = (int64_t)b->tv_nsec - (int64_t)a->tv_nsec;
	return sec_diff * 1000000 + nsec_diff / 1000;
}

static void test_delay_us_elapses_at_least_requested(void)
{
	const uint32_t requested_us = 20000u; /* 20 ms -- comfortably above sched noise */

	struct timespec before, after;
	clock_gettime(CLOCK_MONOTONIC, &before);
	alp_delay_us(requested_us);
	clock_gettime(CLOCK_MONOTONIC, &after);

	int64_t elapsed = z_elapsed_us(&before, &after);
	if (elapsed < (int64_t)requested_us) {
		ALP_TEST_FAIL(
		    "alp_delay_us(%u) returned after only %lld us", requested_us, (long long)elapsed);
	} else {
		ALP_TEST_PASS();
	}
}

static void test_delay_ms_elapses_at_least_requested(void)
{
	const uint32_t requested_ms = 50u;

	struct timespec before, after;
	clock_gettime(CLOCK_MONOTONIC, &before);
	alp_delay_ms(requested_ms);
	clock_gettime(CLOCK_MONOTONIC, &after);

	int64_t elapsed_us = z_elapsed_us(&before, &after);
	if (elapsed_us < (int64_t)requested_ms * 1000) {
		ALP_TEST_FAIL(
		    "alp_delay_ms(%u) returned after only %lld us", requested_ms, (long long)elapsed_us);
	} else {
		ALP_TEST_PASS();
	}
}
#endif /* __linux__ */

static void test_delay_zero_is_noop(void)
{
	/* Must not hang or crash; nothing to measure. */
	alp_delay_us(0u);
	alp_delay_ms(0u);
	ALP_TEST_PASS();
}

int main(void)
{
#if defined(__linux__)
	test_delay_us_elapses_at_least_requested();
	test_delay_ms_elapses_at_least_requested();
#endif
	test_delay_zero_is_noop();

	ALP_TEST_SUMMARY();
}
