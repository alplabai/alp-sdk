/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for issue #627: alp_last_error() must be truly
 * thread-local, and a successful open() must clear only the calling
 * thread's slot.
 *
 * Reproduces the exact interleaving from the issue report -- one
 * thread's failing alp_mqtt_open(NULL) must not be erased by another
 * thread's concurrent, successful alp_gpu2d_open() -- deterministically,
 * via two synchronization barriers rather than timing luck:
 *
 *   1. "fail" thread calls alp_mqtt_open(NULL), records ALP_ERR_INVAL.
 *   2. Both threads rendezvous at barrier #1 (fail-thread's write has
 *      already happened; success-thread hasn't started yet).
 *   3. "success" thread calls alp_gpu2d_open() (real sw_fallback
 *      backend on the plain-CMake Yocto build), which clears ITS OWN
 *      last-error slot to ALP_OK.
 *   4. Both threads rendezvous at barrier #2 (the clear has already
 *      happened).
 *   5. "fail" thread re-reads alp_last_error() -- pre-#627 this could
 *      observe ALP_OK (the other thread's clear); post-fix it MUST
 *      still read ALP_ERR_INVAL.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_last_error_threading
 *   ctest --test-dir build -R alp_test_last_error_threading
 */

#include <pthread.h>
#include <stdint.h>

#include "alp/gpu2d.h"
#include "alp/iot.h"
#include "alp/peripheral.h"

#include "test_assert.h"

static pthread_barrier_t g_barrier;

/* Per-thread outcomes -- written by exactly one thread each, read only
 * after both threads have been joined, so the ALP_ASSERT_* macros
 * (which touch shared, non-atomic pass/fail counters) never run
 * concurrently with each other. */
struct fail_thread_result {
	alp_mqtt_t  *opened;     /* must be NULL */
	alp_status_t err_before; /* right after the failing open() */
	alp_status_t err_after;  /* after the other thread's success+clear */
};

struct success_thread_result {
	alp_gpu2d_t *opened;    /* must be non-NULL: real sw_fallback backend */
	alp_status_t err_after; /* this thread's own slot right after opening */
};

static void *fail_thread_main(void *arg)
{
	struct fail_thread_result *r = (struct fail_thread_result *)arg;

	r->opened     = alp_mqtt_open(NULL); /* NULL cfg -> ALP_ERR_INVAL */
	r->err_before = alp_last_error();

	pthread_barrier_wait(&g_barrier); /* #1: our write is visible before
	                                    * the other thread's open() runs */
	pthread_barrier_wait(&g_barrier); /* #2: the other thread has now
	                                    * cleared ITS OWN slot */

	r->err_after = alp_last_error();
	return NULL;
}

static void *success_thread_main(void *arg)
{
	struct success_thread_result *r = (struct success_thread_result *)arg;

	pthread_barrier_wait(&g_barrier); /* #1: wait for the other thread's
	                                    * failure to be recorded first */

	r->opened    = alp_gpu2d_open(); /* clears THIS thread's slot on success */
	r->err_after = alp_last_error();

	pthread_barrier_wait(&g_barrier); /* #2: tell the other thread it's
	                                    * safe to re-check its own slot */
	return NULL;
}

static void test_concurrent_failure_and_success_dont_clobber(void)
{
	struct fail_thread_result    fr = { 0 };
	struct success_thread_result sr = { 0 };
	pthread_t                    t_fail, t_success;

	ALP_ASSERT_EQ_INT(pthread_barrier_init(&g_barrier, NULL, 2), 0);
	ALP_ASSERT_EQ_INT(pthread_create(&t_fail, NULL, fail_thread_main, &fr), 0);
	ALP_ASSERT_EQ_INT(pthread_create(&t_success, NULL, success_thread_main, &sr), 0);
	ALP_ASSERT_EQ_INT(pthread_join(t_fail, NULL), 0);
	ALP_ASSERT_EQ_INT(pthread_join(t_success, NULL), 0);
	pthread_barrier_destroy(&g_barrier);

	/* The failing open on thread A behaved as documented ... */
	ALP_ASSERT_NULL(fr.opened);
	ALP_ASSERT_EQ_INT(fr.err_before, ALP_ERR_INVAL);

	/* ... and the successful open on thread B cleared only ITS OWN
	 * slot, not thread A's. */
	ALP_ASSERT_TRUE(sr.opened != NULL);
	ALP_ASSERT_EQ_INT(sr.err_after, ALP_OK);

	/* The crux of #627: thread A's diagnostic must survive thread B's
	 * concurrent, successful clear. */
	ALP_ASSERT_EQ_INT(fr.err_after, ALP_ERR_INVAL);

	if (sr.opened != NULL) {
		alp_gpu2d_close(sr.opened);
	}
}

struct fresh_thread_result {
	alp_status_t initial;
};

static void *fresh_thread_main(void *arg)
{
	struct fresh_thread_result *r = (struct fresh_thread_result *)arg;

	/* A brand-new thread that has never called any alp_*_open must
	 * see ALP_OK -- the zero-initialised thread-local slot, not a
	 * stale value left by whichever thread last ran on this stack. */
	r->initial = alp_last_error();
	return NULL;
}

static void test_fresh_thread_initial_state_is_ok(void)
{
	struct fresh_thread_result r = { -999 };
	pthread_t                  t;

	ALP_ASSERT_EQ_INT(pthread_create(&t, NULL, fresh_thread_main, &r), 0);
	ALP_ASSERT_EQ_INT(pthread_join(t, NULL), 0);
	ALP_ASSERT_EQ_INT(r.initial, ALP_OK);
}

int main(void)
{
	test_concurrent_failure_and_success_dont_clobber();
	test_fresh_thread_initial_state_is_ok();
	ALP_TEST_SUMMARY();
}
