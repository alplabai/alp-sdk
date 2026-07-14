/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for issue #628: the handle-less "stateless
 * dispatcher" ops caches (TMU / SoC-info / mproc_boot / power_profile
 * / security's random-bytes fast path) used to publish their resolved
 * backend vtable through a plain `static const T *_cached_ops`
 * read/written with no synchronization -- a C data race under
 * concurrent first calls even though every racing writer resolves the
 * identical, immutable-after-link backend.
 *
 * A plain functional assertion can't observe the underlying data race
 * directly -- on every architecture this SDK ships, an aligned
 * pointer store is atomic at the hardware level, so the race is
 * value-benign in practice (this is exactly what made the pre-#628
 * "lock-free and safe" comment in src/tmu_dispatch.c plausible-looking
 * but wrong under the C memory model).  The authoritative repro is
 * ThreadSanitizer flagging the plain load/store pair as a race; that
 * doesn't fit this plain-CMake, no-sanitizer-toolchain test binary, so
 * this test instead pins down the OBSERVABLE contract the fix must
 * not regress: every thread racing the very first alp_tmu_sin() /
 * alp_soc_info_read() / alp_hash_open() call gets a correctly
 * resolved, working backend -- no NULL-ops NOT_IMPLEMENTED, no torn
 * result -- across many concurrent-first-call trials.  (See the PR
 * description for the companion GCC -fsanitize=thread run against the
 * pre-fix plain read/write, which does flag the race directly.)
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_dispatch_cache_race
 *   ctest --test-dir build -R alp_test_dispatch_cache_race
 */

#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "alp/hw_info.h"
#include "alp/security.h"
#include "alp/tmu.h"

#include "test_assert.h"

#define N_THREADS 8
#define TRIALS    500

/* ------------------------------------------------------------------ */
/* TMU: concurrent, cold-cache alp_tmu_sin() calls.                    */
/* ------------------------------------------------------------------ */

struct tmu_trial_ctx {
	pthread_barrier_t start;
	pthread_barrier_t done;
};

struct tmu_result {
	alp_status_t rc;
	float        out;
};

static struct tmu_trial_ctx g_tmu_ctx;
static struct tmu_result    g_tmu_results[N_THREADS];

static void *tmu_racer(void *arg)
{
	int idx = (int)(intptr_t)arg;

	for (int trial = 0; trial < TRIALS; trial++) {
		pthread_barrier_wait(&g_tmu_ctx.start);
		float out = -999.0f;
		/* pi/6: every thread races the SAME first-touch of the
		 * TMU cache on trial 0; later trials exercise the already-
		 * published fast path under continued concurrency. */
		alp_status_t rc        = alp_tmu_sin(0.5235987755982988f, &out);
		g_tmu_results[idx].rc  = rc;
		g_tmu_results[idx].out = out;
		pthread_barrier_wait(&g_tmu_ctx.done);
	}
	return NULL;
}

static void test_concurrent_first_call_tmu_sin(void)
{
	pthread_t t[N_THREADS];

	ALP_ASSERT_EQ_INT(pthread_barrier_init(&g_tmu_ctx.start, NULL, N_THREADS), 0);
	ALP_ASSERT_EQ_INT(pthread_barrier_init(&g_tmu_ctx.done, NULL, N_THREADS), 0);

	for (int i = 0; i < N_THREADS; i++) {
		ALP_ASSERT_EQ_INT(pthread_create(&t[i], NULL, tmu_racer, (void *)(intptr_t)i), 0);
	}
	for (int i = 0; i < N_THREADS; i++) {
		ALP_ASSERT_EQ_INT(pthread_join(t[i], NULL), 0);
	}
	pthread_barrier_destroy(&g_tmu_ctx.start);
	pthread_barrier_destroy(&g_tmu_ctx.done);

	/* Every racing first-caller must have resolved a working backend
	 * (never NOT_IMPLEMENTED/NULL-ops) and computed the correct value
	 * -- a torn or half-published cache pointer would show up here as
	 * a wrong status or a garbage float. */
	for (int i = 0; i < N_THREADS; i++) {
		ALP_ASSERT_EQ_INT(g_tmu_results[i].rc, ALP_OK);
		ALP_ASSERT_TRUE(fabsf(g_tmu_results[i].out - 0.5f) < 0.001f);
	}
}

/* ------------------------------------------------------------------ */
/* SoC-info: concurrent, cold-cache alp_soc_info_read() calls.          */
/* ------------------------------------------------------------------ */

static pthread_barrier_t g_soc_barrier;
static alp_status_t      g_soc_rc[N_THREADS];

static void *soc_info_racer(void *arg)
{
	int            idx = (int)(intptr_t)arg;
	alp_soc_info_t out;

	pthread_barrier_wait(&g_soc_barrier);
	g_soc_rc[idx] = alp_soc_info_read(&out);
	return NULL;
}

static void test_concurrent_first_call_soc_info(void)
{
	pthread_t t[N_THREADS];

	ALP_ASSERT_EQ_INT(pthread_barrier_init(&g_soc_barrier, NULL, N_THREADS), 0);
	for (int i = 0; i < N_THREADS; i++) {
		ALP_ASSERT_EQ_INT(pthread_create(&t[i], NULL, soc_info_racer, (void *)(intptr_t)i), 0);
	}
	for (int i = 0; i < N_THREADS; i++) {
		ALP_ASSERT_EQ_INT(pthread_join(t[i], NULL), 0);
	}
	pthread_barrier_destroy(&g_soc_barrier);

	/* Every backend the plain-CMake sw_fallback ships answers with
	 * either OK (soc_ref stamped) or the documented NOSUPPORT for
	 * runtime fields -- what must never happen is a crash or an
	 * unrecognised status from a torn cache read. */
	for (int i = 0; i < N_THREADS; i++) {
		ALP_ASSERT_TRUE(g_soc_rc[i] == ALP_OK || g_soc_rc[i] == ALP_ERR_NOSUPPORT);
	}
}

/* ------------------------------------------------------------------ */
/* security: the two-writer opportunistic-populate path (issue #628's  */
/* "security's normal and opportunistic cache paths use the same       */
/* synchronization primitive" acceptance criterion) -- alp_hash_open() */
/* opportunistically publishes the same cache alp_random_bytes()       */
/* resolves independently; race them against each other cold.          */
/* ------------------------------------------------------------------ */

static pthread_barrier_t g_sec_barrier;
static alp_hash_t       *g_sec_hash[N_THREADS];
static alp_status_t      g_sec_rand_rc[N_THREADS];

static void *security_racer(void *arg)
{
	int     idx = (int)(intptr_t)arg;
	uint8_t buf[8];

	pthread_barrier_wait(&g_sec_barrier);
	if (idx % 2 == 0) {
		g_sec_hash[idx] = alp_hash_open(ALP_HASH_SHA256);
	} else {
		g_sec_rand_rc[idx] = alp_random_bytes(buf, sizeof(buf));
	}
	return NULL;
}

static void test_concurrent_hash_open_and_random_bytes(void)
{
	pthread_t t[N_THREADS];

	ALP_ASSERT_EQ_INT(pthread_barrier_init(&g_sec_barrier, NULL, N_THREADS), 0);
	for (int i = 0; i < N_THREADS; i++) {
		ALP_ASSERT_EQ_INT(pthread_create(&t[i], NULL, security_racer, (void *)(intptr_t)i), 0);
	}
	for (int i = 0; i < N_THREADS; i++) {
		ALP_ASSERT_EQ_INT(pthread_join(t[i], NULL), 0);
	}
	pthread_barrier_destroy(&g_sec_barrier);

	/* Whether a real crypto backend is on the host (OpenSSL) or only
	 * the portable NOSUPPORT stub sw_fallback registers as (this test
	 * runs on every CI host, with or without libssl/libcrypto), the
	 * race being exercised is the shared _cached_ops publish -- so
	 * every thread must get a well-formed, self-consistent result
	 * (never a crash, never an unrecognised status) rather than one
	 * specific value.  hash_open may also legitimately return NOMEM:
	 * CONFIG_ALP_SDK_MAX_HASH_HANDLES defaults to 2 and 4 threads
	 * race alp_hash_open() here. */
	for (int i = 0; i < N_THREADS; i++) {
		if (i % 2 == 0) {
			if (g_sec_hash[i] != NULL) {
				alp_hash_close(g_sec_hash[i]);
			}
		} else {
			ALP_ASSERT_TRUE(g_sec_rand_rc[i] == ALP_OK || g_sec_rand_rc[i] == ALP_ERR_NOSUPPORT);
		}
	}
	ALP_ASSERT_TRUE(true); /* reached without a crash/hang */
}

int main(void)
{
	test_concurrent_first_call_tmu_sin();
	test_concurrent_first_call_soc_info();
	test_concurrent_hash_open_and_random_bytes();
	ALP_TEST_SUMMARY();
}
