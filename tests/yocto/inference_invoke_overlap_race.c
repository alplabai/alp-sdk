/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression for the overlapping-invoke semantics <alp/inference.h>
 * documents for alp_inference_last_invoke_latency_us(): two threads
 * calling alp_inference_invoke() on the SAME handle with no serializing
 * mutex between them (active_ops is a drain counter, not a mutex) is
 * real usage the handle permits, and the stored value is
 * last-STORE-wins, not largest-duration-wins or largest-t1-wins.
 *
 * PR #1541 review (also-fix item): the atomic store/load this design
 * relies on had no concurrent-writer test anywhere -- the sibling
 * tests/yocto/inference_latency.c says so explicitly ("Single-threaded
 * -- no race under test here"), and tests/yocto/
 * inference_invoke_close_race.c only races invoke() against close(),
 * never invoke() against another invoke(). This file closes that gap.
 *
 * Determinism, not scheduler luck (see
 * alp-lab:writing-race-safe-dispatch-handlers' "prove it" guidance):
 * thread SLOW calls alp_inference_invoke(), sleeps a long, fixed 40ms
 * in its fake backend body, reads back its own just-stored value, then
 * signals g_slow_done. Thread FAST does NOT call
 * alp_inference_invoke() at all until it has observed g_slow_done --
 * i.e. until SLOW's atomic store has already landed -- so FAST's own
 * store is guaranteed, by happens-before construction rather than by
 * timing, to execute strictly after SLOW's. Only once that gate
 * releases does FAST call alp_inference_invoke(), whose fake backend
 * body sleeps a short, fixed 2ms.
 *
 * Because the g_slow_done wait sits OUTSIDE
 * alp_inference_invoke()'s clock_gettime() bracket, none of that wait
 * folds into FAST's measured duration -- FAST's own reading comes back
 * genuinely small (~2ms), not inflated to SLOW's ~40ms. That
 * combination -- FAST's store is provably the LAST one, and FAST's own
 * duration is provably the SMALLER one -- is what lets the final read
 * discriminate "last store wins" from "largest value wins": a
 * last-store-wins dispatcher reports FAST's small value; a
 * largest-value-wins dispatcher would instead report SLOW's large
 * value, which the assertion below catches. (An earlier version of
 * this test put the g_slow_done wait INSIDE the fake backend body, so
 * it counted toward FAST's own measured duration -- FAST then measured
 * ~SLOW's total wait too, making the two readings nearly identical and
 * letting a largest-value-wins mutant survive most runs.)
 *
 * This test does not exercise true wall-clock overlap of the two
 * dispatcher-level invoke() calls -- FAST does not call
 * alp_inference_invoke() until SLOW's has already returned and stored.
 * What it exercises is two threads calling the same handle's invoke()
 * with no serializing mutex between them, which is exactly the
 * property <alp/inference.h> documents ("last-STORE-wins ... not
 * largest-duration- or largest-finish-time-wins"). Genuine wall-clock
 * overlap of a different op pair (invoke() racing a concurrent
 * close()) is covered separately by inference_invoke_close_race.c.
 *
 * Each thread reads back its own just-stored value immediately after
 * its own alp_inference_invoke() call returns: SLOW reads before
 * signalling g_slow_done (so no store from FAST -- which has not even
 * called alp_inference_invoke() yet -- can have landed first); FAST
 * reads right after its own call returns, and by construction SLOW's
 * store landed well before FAST's gate released. Both baseline
 * readings are therefore race-free by construction, and only the FINAL
 * post-join read exercises the actual last-store-wins race this file
 * is testing.
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

/* SLOW sleeps 40ms; FAST sleeps 2ms. FAST's caller (fast_thread(),
 * below) does not call alp_inference_invoke() -- and therefore never
 * reaches this function -- until it has already observed g_slow_done,
 * so unlike an earlier version of this test, nothing in here needs to
 * wait on SLOW: by the time this runs for FAST, SLOW's store has
 * already landed. */
alp_status_t alp_inference_deepx_invoke(struct alp_inference *h)
{
	(void)h;
	struct timespec ts;
	if (t_is_slow) {
		ts = (struct timespec){ .tv_sec = 0, .tv_nsec = 40000000L }; /* 40ms */
	} else {
		ts = (struct timespec){ .tv_sec = 0, .tv_nsec = 2000000L }; /* 2ms */
	}
	nanosleep(&ts, NULL);
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
	/* Race-free: FAST has not even called alp_inference_invoke() yet --
	 * it is still polling g_slow_done, which this thread has not set
	 * yet -- so no store from FAST can have landed before this read. */
	ALP_ASSERT_EQ_INT(alp_inference_last_invoke_latency_us(a->h, &a->own_us), ALP_OK);
	atomic_store(&g_slow_done, 1);
	return NULL;
}

static void *fast_thread(void *arg)
{
	thread_arg_t *a = arg;
	t_is_slow       = 0;
	/* Wait OUTSIDE alp_inference_invoke()'s clock_gettime() bracket for
	 * SLOW's store to have already landed -- see this file's header
	 * comment. This is what guarantees FAST's own store lands strictly
	 * after SLOW's while keeping the wait itself out of FAST's own
	 * measured duration. */
	while (!atomic_load(&g_slow_done)) {
		struct timespec tick = { .tv_sec = 0, .tv_nsec = 100000L }; /* 100us poll */
		nanosleep(&tick, NULL);
	}
	ALP_ASSERT_EQ_INT(alp_inference_invoke(a->h), ALP_OK);
	/* Race-free: SLOW's store landed before this thread's gate above
	 * released -- this read observes FAST's own just-landed store. */
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
