/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression for alp_inference_last_invoke_latency_us() on the Yocto
 * inference dispatcher (src/yocto/inference_yocto.c).
 *
 * Drives the REAL public alp_inference_open()/alp_inference_invoke()/
 * alp_inference_last_invoke_latency_us()/alp_inference_close() by
 * #including src/yocto/inference_yocto.c directly into this TU (same
 * technique as tests/yocto/inference_invoke_close_race.c) through a
 * fake DEEPX_DXM1 backend defined below: ALP_SDK_USE_DEEPX_DXM1 is
 * forced on before the #include so the dispatcher's real switch
 * statements -- including the clock_gettime() bracket this test
 * exists to cover -- pick this file's alp_inference_deepx_*()
 * stand-ins; only the backend "invoke" is faked, not the dispatcher
 * itself.
 *
 * Single-threaded -- no race under test here (that's
 * inference_invoke_close_race.c's job for invoke-vs-close, and
 * inference_invoke_overlap_race.c's for invoke-vs-invoke's last-store-
 * wins semantics), so no pthreads dependency.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_inference_latency
 *   ctest --test-dir build -R alp_test_inference_latency
 */

#include <stdint.h>
#include <time.h>

#include "test_assert.h"

#define ALP_SDK_USE_DEEPX_DXM1 1
#include "../../src/yocto/inference_yocto.c"

static int g_fail_next_invoke;

alp_status_t alp_inference_deepx_open(struct alp_inference *h, const alp_inference_config_t *cfg)
{
	(void)cfg;
	static int st;
	h->be_state = &st;
	return ALP_OK;
}

size_t alp_inference_deepx_num_inputs(struct alp_inference *h)
{
	(void)h;
	return 1u;
}

size_t alp_inference_deepx_num_outputs(struct alp_inference *h)
{
	(void)h;
	return 1u;
}

alp_status_t
alp_inference_deepx_get_input(struct alp_inference *h, size_t index, alp_inference_tensor_t *out)
{
	(void)h;
	(void)index;
	*out = (alp_inference_tensor_t){ 0 };
	return ALP_OK;
}

alp_status_t
alp_inference_deepx_get_output(struct alp_inference *h, size_t index, alp_inference_tensor_t *out)
{
	(void)h;
	(void)index;
	*out = (alp_inference_tensor_t){ 0 };
	return ALP_OK;
}

/* Stands in for a real backend's synchronous model executor.  A short
 * sleep gives the clock_gettime() bracket in alp_inference_invoke() a
 * measurable, nonzero window to report -- the property under test is
 * "a real duration got captured", not a specific magnitude. */
alp_status_t alp_inference_deepx_invoke(struct alp_inference *h)
{
	(void)h;
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 2000000L }; /* 2ms "Run()" */
	nanosleep(&ts, NULL);
	return g_fail_next_invoke ? ALP_ERR_IO : ALP_OK;
}

void alp_inference_deepx_close(struct alp_inference *h)
{
	h->be_state = NULL;
}

static alp_inference_t *open_fake_handle(void)
{
	static const uint8_t   model[16] = { 0xDE, 0xAD, 0xBE, 0xEF };
	alp_inference_config_t cfg       = {
		.model_data = model,
		.model_size = sizeof(model),
		.backend    = ALP_INFERENCE_BACKEND_DEEPX_DXM1,
	};
	return alp_inference_open(&cfg);
}

static void test_before_any_invoke_returns_not_ready(void)
{
	alp_inference_t *h = open_fake_handle();
	ALP_ASSERT_TRUE(h != NULL);

	uint64_t us = 0u;
	ALP_ASSERT_EQ_INT(alp_inference_last_invoke_latency_us(h, &us), ALP_ERR_NOT_READY);

	alp_inference_close(h);
}

static void test_null_out_returns_invalid(void)
{
	alp_inference_t *h = open_fake_handle();
	ALP_ASSERT_TRUE(h != NULL);

	ALP_ASSERT_EQ_INT(alp_inference_last_invoke_latency_us(h, NULL), ALP_ERR_INVAL);

	alp_inference_close(h);
}

/* MAJOR 2 (PR #1541 review): this accessor used to check the handle
 * first here, so (NULL, NULL) returned ALP_ERR_NOT_READY on this
 * (Yocto) dispatcher while the Zephyr dispatcher
 * (src/inference_dispatch.c) -- checking out_us first, like every
 * other op in that file -- returned ALP_ERR_INVAL for the identical
 * call: the same public function disagreeing with itself across OSes.
 * Both dispatchers now check out_us first, matching
 * <alp/inference.h>'s documented order (ALP_ERR_INVAL listed before
 * ALP_ERR_NOT_READY). */
static void test_null_handle_and_null_out_returns_invalid(void)
{
	ALP_ASSERT_EQ_INT(alp_inference_last_invoke_latency_us(NULL, NULL), ALP_ERR_INVAL);
}

/* Same MAJOR 2 fix, exercised against a CLOSED (not just NULL) handle --
 * out_us-NULL must win over the handle's closed state too. */
static void test_closed_handle_null_out_returns_invalid(void)
{
	alp_inference_t *h = open_fake_handle();
	ALP_ASSERT_TRUE(h != NULL);
	alp_inference_close(h);

	ALP_ASSERT_EQ_INT(alp_inference_last_invoke_latency_us(h, NULL), ALP_ERR_INVAL);
}

static void test_populated_after_successful_invoke(void)
{
	g_fail_next_invoke = 0;
	alp_inference_t *h = open_fake_handle();
	ALP_ASSERT_TRUE(h != NULL);

	ALP_ASSERT_EQ_INT(alp_inference_invoke(h), ALP_OK);

	uint64_t us = 0u;
	ALP_ASSERT_EQ_INT(alp_inference_last_invoke_latency_us(h, &us), ALP_OK);
	/* The fake invoke sleeps ~2ms -- assert a generous lower/upper
	 * band rather than an exact figure (host scheduling jitter), which
	 * still proves a REAL measurement was taken, not a stub 0. */
	ALP_ASSERT_TRUE(us >= 1000u);
	ALP_ASSERT_TRUE(us < 1000000u);

	alp_inference_close(h);
}

static void test_unchanged_after_failed_invoke(void)
{
	alp_inference_t *h = open_fake_handle();
	ALP_ASSERT_TRUE(h != NULL);

	g_fail_next_invoke = 0;
	ALP_ASSERT_EQ_INT(alp_inference_invoke(h), ALP_OK);
	uint64_t first_us = 0u;
	ALP_ASSERT_EQ_INT(alp_inference_last_invoke_latency_us(h, &first_us), ALP_OK);

	g_fail_next_invoke = 1;
	ALP_ASSERT_EQ_INT(alp_inference_invoke(h), ALP_ERR_IO);
	uint64_t second_us = 0u;
	ALP_ASSERT_EQ_INT(alp_inference_last_invoke_latency_us(h, &second_us), ALP_OK);
	ALP_ASSERT_EQ_INT(second_us, first_us);

	alp_inference_close(h);
}

int main(void)
{
	test_before_any_invoke_returns_not_ready();
	test_null_out_returns_invalid();
	test_null_handle_and_null_out_returns_invalid();
	test_closed_handle_null_out_returns_invalid();
	test_populated_after_successful_invoke();
	test_unchanged_after_failed_invoke();

	ALP_TEST_SUMMARY();
}
