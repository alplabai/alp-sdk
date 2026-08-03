/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mechanism-level regression for issue #1114's round-2 dev-review
 * finding: the first liveness fix for src/backends/security/zephyr_drv.c's
 * ensure_psa() elected exactly one initialiser via alp_slot_try_claim(),
 * but every OTHER thread waited with
 *
 *     while (!g_psa_inited) k_sleep(...);
 *
 * -- a flag the elected initialiser's FAILURE path never sets (it only
 * releases its claim and returns the error to itself). Every thread
 * that lost the race then hung in that loop FOREVER, a worse hang than
 * the pre-#1114 behaviour (which simply returned the error to each
 * caller). The fix re-attempts alp_slot_try_claim() inside the wait
 * loop instead of waiting on a flag the failure path never sets, so a
 * released claim lets a waiter retry the init itself.
 *
 * ensure_psa() itself is Zephyr + mbedtls-PSA specific and cannot run
 * host-side in this suite (no k_sleep, no PSA crypto init on a plain
 * CMake/pthread build) -- this test instead exercises the SAME
 * elect-once-init algorithm shape, byte-for-byte the same claim/wait
 * structure over the SAME shared alp_slot_try_claim()/alp_slot_release()/
 * alp_slot_sleep_tick() primitives ensure_psa() uses, with a fake
 * "init" function whose success/failure is test-controlled. Mirrors
 * the existing split in this directory: slot_claim_race.c mechanism-
 * proves the shared pool primitive generically; gpu2d_slot_race.c then
 * confirms the wiring on one representative real backend. The genuine
 * backend (ensure_psa()) needs Zephyr silicon/twister to exercise for
 * real; this file is the mechanism-level proof that the ALGORITHM
 * ensure_psa() now implements does not hang a losing waiter when the
 * elected initialiser fails.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_elect_once_init_race
 *   ctest --test-dir build -R alp_test_elect_once_init_race
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "alp_slot_claim.h"

#include "test_assert.h"

/* ---- fake "psa_crypto_init()" -- test-controlled success/failure ---- */

static bool     g_claimed;
static bool     g_inited;
static uint32_t g_fail_budget; /* number of claimant attempts that must fail
                                   before one is allowed to succeed */

/* Same increment-guarded-by-CAS-claim shape psa_crypto_init() sits
 * behind in ensure_psa() -- deterministically fails the first
 * g_fail_budget claimants, then succeeds. The short sleep mirrors a
 * real crypto-engine init taking actual wall-clock time (never
 * instant) -- without it, the elected thread can claim, fail, AND
 * release before the loser thread even reaches its own
 * alp_slot_try_claim() attempt, so the loser just wins the (by-then
 * free) claim itself instead of ever exercising the wait path this
 * test targets. */
static bool fake_crypto_init(void)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000L }; /* 50ms */
	nanosleep(&ts, NULL);
	if (g_fail_budget > 0u) {
		--g_fail_budget;
		return false;
	}
	return true;
}

/* The FIXED algorithm -- exactly the shape now in
 * src/backends/security/zephyr_drv.c's ensure_psa() (issue #1114
 * round-2 dev review): the wait loop re-attempts the claim instead of
 * only re-checking g_inited, so a failed initialiser's released claim
 * is picked up by a waiter instead of leaving it parked on a flag
 * nobody will ever set. */
static bool ensure_inited_fixed(void)
{
	for (;;) {
		if (__atomic_load_n(&g_inited, __ATOMIC_ACQUIRE)) {
			return true;
		}
		if (alp_slot_try_claim(&g_claimed)) {
			if (!fake_crypto_init()) {
				alp_slot_release(&g_claimed);
				return false;
			}
			__atomic_store_n(&g_inited, true, __ATOMIC_RELEASE);
			return true;
		}
		alp_slot_sleep_tick();
	}
}

/* ---- concurrent-open-with-a-failing-first-attempt scenario ---- */

struct racer_ctx {
	pthread_barrier_t start;
};

static bool g_a_done;
static bool g_b_done;
static bool g_a_result;
static bool g_b_result;

static void *racer_a(void *arg)
{
	struct racer_ctx *ctx = (struct racer_ctx *)arg;
	pthread_barrier_wait(&ctx->start);
	bool ok = ensure_inited_fixed();
	__atomic_store_n(&g_a_result, ok, __ATOMIC_RELEASE);
	__atomic_store_n(&g_a_done, true, __ATOMIC_RELEASE);
	return NULL;
}

static void *racer_b(void *arg)
{
	struct racer_ctx *ctx = (struct racer_ctx *)arg;
	pthread_barrier_wait(&ctx->start);
	bool ok = ensure_inited_fixed();
	__atomic_store_n(&g_b_result, ok, __ATOMIC_RELEASE);
	__atomic_store_n(&g_b_done, true, __ATOMIC_RELEASE);
	return NULL;
}

/* Two threads race ensure_inited_fixed(); whichever wins the claim
 * runs fake_crypto_init(), which is set to fail EXACTLY ONCE. That
 * reproduces the #1114 round-2 shape precisely: the loser waits, the
 * winner fails and releases its claim without ever setting g_inited.
 * A pre-fix (`while (!g_inited) sleep;`) waiter would hang here
 * forever. The FIX's contract is NOT "every caller succeeds" -- the
 * thread unlucky enough to be elected during the one transient failure
 * correctly gets the error back for ITS OWN call (matches the
 * pre-#1114 "callers simply get the error back" semantics, same as an
 * unraced caller would see). What the fix guarantees is (a) neither
 * thread hangs, and (b) the SYSTEM recovers -- the released claim gets
 * picked up by a retry (the other racer, if it was still waiting), so
 * g_inited does end up true and at least one of the two racers
 * observes success.
 *
 * The watchdog below bounds the wait instead of a plain pthread_join():
 * a hung thread (the pre-fix bug this test targets) must not hang the
 * whole CTest run -- it must fail this test cleanly instead. */
static void test_waiter_survives_a_failed_initialiser(void)
{
	g_claimed     = false;
	g_inited      = false;
	g_fail_budget = 1u;

	struct racer_ctx ctx;
	ALP_ASSERT_EQ_INT(pthread_barrier_init(&ctx.start, NULL, 2), 0);

	pthread_t ta, tb;
	ALP_ASSERT_EQ_INT(pthread_create(&ta, NULL, racer_a, &ctx), 0);
	ALP_ASSERT_EQ_INT(pthread_create(&tb, NULL, racer_b, &ctx), 0);
	/* Detach: on a timeout (mutation present) these threads are left
	 * spinning forever, but the test process exits right after
	 * reporting the failure, which reaps them anyway. */
	pthread_detach(ta);
	pthread_detach(tb);

	const int watchdog_iters = 2000; /* 2000 * 5ms = 10s ceiling */
	int       i              = 0;
	while (i < watchdog_iters && !(__atomic_load_n(&g_a_done, __ATOMIC_ACQUIRE) &&
	                               __atomic_load_n(&g_b_done, __ATOMIC_ACQUIRE))) {
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 5 * 1000 * 1000L }; /* 5ms */
		nanosleep(&ts, NULL);
		++i;
	}

	bool both_finished = __atomic_load_n(&g_a_done, __ATOMIC_ACQUIRE) &&
	                     __atomic_load_n(&g_b_done, __ATOMIC_ACQUIRE);
	ALP_ASSERT_TRUE(both_finished);
	if (!both_finished) {
		/* Mutation present (pre-fix wait-on-flag-only shape): at least
		 * one racer is hung. Skip the result checks below -- reading
		 * g_a_result/g_b_result of a thread that never finished isn't
		 * meaningful, and pthread_join() here would itself hang. */
		return;
	}

	/* The system recovered from the one transient failure: at least one
	 * racer (whichever retried the released claim) observed success,
	 * and the shared state is left fully initialised for every FUTURE
	 * caller -- exactly one of g_a_result/g_b_result may legitimately
	 * be false (whichever thread was elected during the single
	 * simulated failure gets that error back for its own call, same as
	 * an unraced caller would). */
	bool a_ok = __atomic_load_n(&g_a_result, __ATOMIC_ACQUIRE);
	bool b_ok = __atomic_load_n(&g_b_result, __ATOMIC_ACQUIRE);
	ALP_ASSERT_TRUE(a_ok || b_ok);
	ALP_ASSERT_TRUE(__atomic_load_n(&g_inited, __ATOMIC_ACQUIRE));

	pthread_barrier_destroy(&ctx.start);
}

/* Sanity check: a genuinely permanent failure (fake_crypto_init()
 * never succeeds) must still let every caller return promptly with an
 * error -- not hang -- matching the pre-#1114 "callers simply get the
 * error back" contract for the (rare) case the real backend's
 * psa_crypto_init() is broken outright. */
static void test_permanent_failure_does_not_hang(void)
{
	g_claimed     = false;
	g_inited      = false;
	g_fail_budget = UINT32_MAX;

	bool ok = ensure_inited_fixed();
	ALP_ASSERT_TRUE(!ok);
	ALP_ASSERT_TRUE(!__atomic_load_n(&g_inited, __ATOMIC_ACQUIRE));
	/* Claim was released on failure so a later, independent call can
	 * retry (matches "let a later caller retry the one-time init"). */
	ALP_ASSERT_TRUE(!__atomic_load_n(&g_claimed, __ATOMIC_ACQUIRE));
}

int main(void)
{
	test_waiter_survives_a_failed_initialiser();
	test_permanent_failure_does_not_hang();
	ALP_TEST_SUMMARY();
}
