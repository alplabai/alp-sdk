/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression for the overlapping-invoke semantics <alp/inference.h>
 * documents for alp_inference_last_invoke_latency_us(): two threads
 * calling alp_inference_invoke() concurrently on the SAME handle is a
 * real interleaving this handle's op-counting permits (active_ops is a
 * drain counter, not a mutex), and the stored value is last-STORE-wins,
 * not largest-duration-wins or largest-t1-wins.
 *
 * PR #1541 review (also-fix item): the atomic store/load this design
 * relies on had no concurrent-writer test anywhere -- the sibling
 * tests/yocto/inference_latency.c says so explicitly ("Single-threaded
 * -- no race under test here"), and tests/yocto/
 * inference_invoke_close_race.c only races invoke() against close(),
 * never invoke() against another invoke(). This file closes that gap.
 *
 * The test makes the interleaving DETERMINISTIC instead of relying on
 * scheduler luck (see alp-lab:writing-race-safe-dispatch-handlers'
 * "prove it" guidance): thread SLOW sleeps a long, fixed duration in
 * its fake backend invoke, then signals g_slow_done; thread FAST
 * sleeps a short, fixed duration and then BLOCKS on g_slow_done before
 * returning, so FAST's dispatcher-level atomic store is guaranteed to
 * execute strictly after SLOW's, even though FAST's own intended
 * "compute" duration is the smaller of the two. If the implementation
 * instead kept whichever value were LARGER (or whichever call started
 * first), the final read would come back as SLOW's (large) duration;
 * this test asserts it is FAST's instead, proving "last store", not
 * "largest value" or "first caller", wins.
 *
 * Each thread reads back its own just-stored value immediately after
 * its own alp_inference_invoke() call returns and before the other
 * thread's store can possibly have landed (SLOW reads before signalling
 * g_slow_done; FAST reads after unblocking, and by then SLOW has long
 * since finished) -- so both baseline readings are race-free by
 * construction, and only the FINAL post-join read exercises the actual
 * concurrent-store race this file is testing.
 *
 * #includes src/yocto/inference_yocto.c directly (same technique as
 * inference_invoke_close_race.c) through a fake DEEPX_DXM1 backend --
 * deliberately NOT linked against alp::sdk, same ODR rationale as that
 * file.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_inference_invoke_overlap_race
 *   ctest --test-dir build -R alp_test_inference_invoke_overlap_race
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#include "test_assert.h"

#define ALP_SDK_USE_DEEPX_DXM1 1
#include "../../src/yocto/inference_yocto.c"

static atomic_int g_slow_done;
/* Which fake-invoke call is currently running: FAST's backend body uses
 * this (not a fixed sleep duration alone) to decide when SLOW's backend
 * body has entered its long sleep, so FAST genuinely overlaps SLOW
 * in-flight rather than merely running after it starts. */
static atomic_int g_slow_entered;

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

/* Which thread's "invoke" is currently executing -- swapped in by each
 * thread before calling alp_inference_invoke() so the single shared fake
 * backend function can tell SLOW's body from FAST's without a second
 * ops table. */
static _Thread_local int t_is_slow;

alp_status_t alp_inference_deepx_invoke(struct alp_inference *h)
{
	(void)h;
	if (t_is_slow) {
		atomic_store(&g_slow_entered, 1);
		struct timespec slow_ts = { .tv_sec = 0, .tv_nsec = 40000000L }; /* 40ms */
		nanosleep(&slow_ts, NULL);
		return ALP_OK; /* caller stores, then signals g_slow_done */
	}
	/* FAST: wait for SLOW to have genuinely entered its own invoke (real
	 * overlap, not "FAST ran entirely after SLOW"), do a short sleep of
	 * its own, then block until SLOW's store has landed -- guaranteeing
	 * FAST's store lands strictly after SLOW's regardless of either
	 * sleep length. */
	while (!atomic_load(&g_slow_entered)) {
		struct timespec tick = { .tv_sec = 0, .tv_nsec = 100000L }; /* 100us poll */
		nanosleep(&tick, NULL);
	}
	struct timespec fast_ts = { .tv_sec = 0, .tv_nsec = 2000000L }; /* 2ms */
	nanosleep(&fast_ts, NULL);
	while (!atomic_load(&g_slow_done)) {
		struct timespec tick = { .tv_sec = 0, .tv_nsec = 100000L }; /* 100us poll */
		nanosleep(&tick, NULL);
	}
	return ALP_OK;
}

void alp_inference_deepx_close(struct alp_inference *h)
{
	h->be_state = NULL;
}

typedef struct {
	alp_inference_t *h;
	uint64_t         own_us; /* filled by the thread from its own immediate read */
} thread_arg_t;

static void *slow_thread(void *arg)
{
	thread_arg_t *a = arg;
	t_is_slow       = 1;
	ALP_ASSERT_EQ_INT(alp_inference_invoke(a->h), ALP_OK);
	/* Race-free: FAST is still blocked in its own backend body waiting
	 * on g_slow_done, which has not been set yet -- no store from FAST
	 * can have landed before this read. */
	ALP_ASSERT_EQ_INT(alp_inference_last_invoke_latency_us(a->h, &a->own_us), ALP_OK);
	atomic_store(&g_slow_done, 1);
	return NULL;
}

static void *fast_thread(void *arg)
{
	thread_arg_t *a = arg;
	t_is_slow       = 0;
	ALP_ASSERT_EQ_INT(alp_inference_invoke(a->h), ALP_OK);
	/* Race-free: SLOW has already returned and stored (that is exactly
	 * what unblocked this thread's fake backend body above) -- this
	 * read observes FAST's own just-landed store. */
	ALP_ASSERT_EQ_INT(alp_inference_last_invoke_latency_us(a->h, &a->own_us), ALP_OK);
	return NULL;
}

static void test_overlapping_invoke_is_last_store_wins(void)
{
	static const uint8_t   model[16] = { 0xDE, 0xAD, 0xBE, 0xEF };
	alp_inference_config_t cfg       = {
		.model_data = model,
		.model_size = sizeof(model),
		.backend    = ALP_INFERENCE_BACKEND_DEEPX_DXM1,
	};
	alp_inference_t *h = alp_inference_open(&cfg);
	ALP_ASSERT_TRUE(h != NULL);

	atomic_store(&g_slow_done, 0);
	atomic_store(&g_slow_entered, 0);

	thread_arg_t slow_arg = { .h = h, .own_us = 0u };
	thread_arg_t fast_arg = { .h = h, .own_us = 0u };

	pthread_t t_slow, t_fast;
	ALP_ASSERT_EQ_INT(pthread_create(&t_slow, NULL, slow_thread, &slow_arg), 0);
	ALP_ASSERT_EQ_INT(pthread_create(&t_fast, NULL, fast_thread, &fast_arg), 0);
	ALP_ASSERT_EQ_INT(pthread_join(t_slow, NULL), 0);
	ALP_ASSERT_EQ_INT(pthread_join(t_fast, NULL), 0);

	/* SLOW's own reading (~40ms) must be clearly larger than FAST's
	 * (~2ms) -- sanity-checks that the two calls really measured
	 * different, distinguishable durations. */
	ALP_ASSERT_TRUE(slow_arg.own_us > fast_arg.own_us);

	/* The property under test: after BOTH calls have completed, the
	 * handle reports FAST's value (the LAST store, by construction),
	 * not SLOW's (the larger duration, and the call that entered
	 * first). */
	uint64_t final_us = 0u;
	ALP_ASSERT_EQ_INT(alp_inference_last_invoke_latency_us(h, &final_us), ALP_OK);
	ALP_ASSERT_EQ_INT(final_us, fast_arg.own_us);
	ALP_ASSERT_TRUE(final_us != slow_arg.own_us);

	alp_inference_close(h);
}

int main(void)
{
	test_overlapping_invoke_is_last_store_wins();
	ALP_TEST_SUMMARY();
}
