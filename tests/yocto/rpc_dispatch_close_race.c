/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Dispatcher-path regression for GHSA-xhm8-7f87-93q5: drives the REAL
 * public alp_rpc_open()/alp_rpc_close()/alp_rpc_subscribe()/
 * alp_rpc_send()/alp_rpc_call() surface (src/rpc_dispatch.c), not a
 * hand-built backend struct.  rpc_yocto_self_close.c already covers
 * the BACKEND layer (yocto_drv.c's y_shutdown()/y_destroy()) by
 * #including that file directly and driving its file-local
 * rpc_rx_main()/y_shutdown(); this file complements it by covering
 * src/rpc_dispatch.c's own single-owner CAS + active-op drain (struct
 * alp_rpc_channel's combined lifecycle+refcount `chan_word` -- see
 * src/backends/rpc/rpc_ops.h) through the ACTUAL alp_rpc_open() /
 * alp_rpc_close() entry points, so a regression to the plain
 * `!ch->in_use` TOCTOU an earlier followup review flagged is caught
 * even on a host with no OpenAMP/libmetal installed at all.
 *
 * This links against the real alp::sdk static library and lets the
 * registry pick whichever "rpc" backend is available on this host
 * (src/backends/rpc/sw_fallback.c on a bare CI runner -- see its own
 * header comment: open/subscribe/send/call all return synchronously
 * and shutdown()/destroy() are no-ops).  That is fine and deliberate:
 * the dispatcher's single-owner CAS + active-op drain live entirely
 * ABOVE the ops vtable, so they are exercised identically no matter
 * which backend services the calls underneath -- unlike
 * rpc_yocto_self_close.c, this file does not need (and does not
 * force) ALP_SDK_HAVE_OPENAMP_USERLAND.
 *
 * Three scenarios:
 *   1. test_concurrent_close_is_single_shot -- two threads race
 *      alp_rpc_close() on the SAME handle.  This dispatcher makes no
 *      thread-identity distinction between an "external" close and a
 *      channel's own "self" close the way the backend layer does (see
 *      rpc_yocto_self_close.c) -- both are just racing callers of
 *      alp_rpc_close(), which is exactly what struct alp_rpc_channel's
 *      single-owner CAS (OPEN -> CLOSING on `chan_word`) defends
 *      against.  Across many iterations: neither thread hangs or
 *      crashes, and a FRESH alp_rpc_open() after both return
 *      successfully reclaims a pool slot -- proving the CAS + drain
 *      released the slot back to `_alloc_rpc()` exactly once, not zero
 *      times (leaked forever) or in an inconsistent state (the
 *      recycle-hijack TOCTOU the plain `!ch->in_use` version of
 *      alp_rpc_close() had).
 *   2. test_send_vs_close_race -- one thread spins calling
 *      alp_rpc_send() while another concurrently closes the SAME
 *      handle; exercises alp_rpc_send()'s op-enter/leave guard racing
 *      alp_rpc_close()'s CAS + drain.
 *   3. test_recycle_race_stress -- N send/call threads race M
 *      close/reopen threads cycling a SMALL pool (so slot reuse is
 *      frequent, not incidental).  Every thread pins itself to a
 *      handle from a small shared array of currently-open channels and
 *      hammers alp_rpc_send()/alp_rpc_call() on it while the close/
 *      reopen threads cycle the SAME array slots; asserts every return
 *      code is one of the documented ones (no crash, no hang) across
 *      many iterations, then, after every thread has stopped, opens
 *      one final channel per pool slot to prove no slot was ever
 *      leaked (permanently stuck non-UNOPENED) or double-claimed.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_rpc_dispatch_close_race
 *   ctest --test-dir build -R alp_test_rpc_dispatch_close_race
 * Under ASan/UBSan/TSan: see the *_asan/*_tsan CTest variants
 * registered next to this target in this directory's CMakeLists.txt.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#include <alp/rpc.h>

/* Internal-only: peeks at struct alp_rpc_channel's combined
 * lifecycle+refcount `chan_word` for the extra sanity assertions in
 * test_recycle_race_stress() below (count never underflows, lifecycle
 * bits always one of the three valid values).  Safe to pull into this
 * TU: it is the SAME header src/rpc_dispatch.c itself includes, so
 * `struct alp_rpc_channel` is one consistent definition, not an ODR
 * split -- this file still talks to the dispatcher exclusively through
 * the public alp_rpc_* entry points; this header only lets it read
 * (never write) the internal state those entry points already
 * serialise. */
#include "../../src/backends/rpc/rpc_ops.h"

#include "test_assert.h"

#define RACE_ITERATIONS      500
#define SEND_RACE_ITERATIONS 50

static void on_msg(const void *payload, size_t len, void *user)
{
	(void)payload;
	(void)len;
	(void)user;
}

/* ------------------------------------------------------------------ */
/* 1. Two threads race alp_rpc_close() on the same handle.             */
/* ------------------------------------------------------------------ */

static alp_rpc_channel_t *g_race_ch;

static void *closer_thread(void *arg)
{
	(void)arg;
	alp_rpc_close(g_race_ch);
	return NULL;
}

static void test_concurrent_close_is_single_shot(void)
{
	for (int i = 0; i < RACE_ITERATIONS; ++i) {
		alp_rpc_config_t cfg = ALP_RPC_CONFIG_DEFAULT("dispatch_race");
		g_race_ch            = alp_rpc_open(&cfg);
		ALP_ASSERT_TRUE(g_race_ch != NULL);

		pthread_t t1, t2;
		ALP_ASSERT_EQ_INT(pthread_create(&t1, NULL, closer_thread, NULL), 0);
		ALP_ASSERT_EQ_INT(pthread_create(&t2, NULL, closer_thread, NULL), 0);
		ALP_ASSERT_EQ_INT(pthread_join(t1, NULL), 0);
		ALP_ASSERT_EQ_INT(pthread_join(t2, NULL), 0);

		/* The slot must be cleanly reclaimable -- proves the CAS +
         * drain released it exactly once, regardless of which racing
         * thread won the CAS. */
		alp_rpc_channel_t *reopened = alp_rpc_open(&cfg);
		ALP_ASSERT_TRUE(reopened != NULL);
		alp_rpc_close(reopened);
	}
}

/* ------------------------------------------------------------------ */
/* 2. alp_rpc_send() racing alp_rpc_close() on the same handle.        */
/* ------------------------------------------------------------------ */

static alp_rpc_channel_t *g_send_race_ch;
static atomic_int         g_send_race_stop;
static atomic_int         g_send_race_bad_rc;

static void *sender_thread(void *arg)
{
	(void)arg;
	uint8_t byte = 0xAAu;
	while (!atomic_load(&g_send_race_stop)) {
		/* ALP_OK (accepted before the close won) or ALP_ERR_NOT_READY
         * (the close already won) are both correct outcomes here --
         * what this test actually proves is the absence of a
         * crash/hang/UAF under ASan/TSan while this races
         * alp_rpc_close() below, not which specific status comes back.
         *
         * Deliberately NOT an ALP_ASSERT_* call in this background
         * thread: test_assert.h's pass/fail counters are plain,
         * unsynchronised globals, so asserting here would itself race
         * the main thread's own ALP_ASSERT_* calls (e.g. right after
         * pthread_create() below) on those counters -- a TEST-HARNESS
         * data race, not anything about the RPC dispatcher this test
         * is actually exercising.  Record the outcome and let the
         * JOINING thread (ordered-after this thread's exit by
         * pthread_join()) do the one ALP_ASSERT_TRUE below. */
		alp_status_t rc = alp_rpc_send(g_send_race_ch, "ping", &byte, sizeof byte);
		if (!(rc == ALP_OK || rc == ALP_ERR_NOT_READY)) {
			atomic_store(&g_send_race_bad_rc, 1);
		}
	}
	return NULL;
}

static void test_send_vs_close_race(void)
{
	for (int i = 0; i < SEND_RACE_ITERATIONS; ++i) {
		alp_rpc_config_t cfg = ALP_RPC_CONFIG_DEFAULT("dispatch_send_race");
		g_send_race_ch       = alp_rpc_open(&cfg);
		ALP_ASSERT_TRUE(g_send_race_ch != NULL);
		ALP_ASSERT_EQ_INT(alp_rpc_subscribe(g_send_race_ch, "ping", on_msg, NULL), ALP_OK);

		atomic_store(&g_send_race_stop, 0);
		atomic_store(&g_send_race_bad_rc, 0);
		pthread_t sender;
		ALP_ASSERT_EQ_INT(pthread_create(&sender, NULL, sender_thread, NULL), 0);

		/* Let the sender spin a little before racing the close so the
         * two threads actually overlap instead of running back to back. */
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000L };
		nanosleep(&ts, NULL);

		alp_rpc_close(g_send_race_ch);
		atomic_store(&g_send_race_stop, 1);
		ALP_ASSERT_EQ_INT(pthread_join(sender, NULL), 0);
		ALP_ASSERT_TRUE(atomic_load(&g_send_race_bad_rc) == 0);
	}
}

/* ------------------------------------------------------------------ */
/* 3. Recycle-race stress: N send/call threads vs M close/reopen       */
/*    threads cycling a small (default-sized, 2-slot) pool.            */
/* ------------------------------------------------------------------ */

#define RECYCLE_POOL_SIZE    2 /* matches CONFIG_ALP_SDK_MAX_RPC_HANDLES's default */
#define RECYCLE_SEND_THREADS 3
#define RECYCLE_DURATION_MS  300

static _Atomic(alp_rpc_channel_t *) g_recycle_slots[RECYCLE_POOL_SIZE];
static atomic_int                   g_recycle_stop;
static atomic_int                   g_recycle_bad_rc;
static atomic_int                   g_recycle_bad_word;

/* Best-effort sanity check on the internal combined word: the
 * lifecycle bits must always be one of the three valid states, and
 * the count bits must never look like an underflowed wraparound
 * (bounded generously above the actual number of racing threads).
 * Racy-by-nature (no synchronisation with the dispatcher's own
 * atomics beyond the load itself) -- this is a best-effort invariant
 * check, not a correctness proof; ASan/TSan is what actually proves
 * memory-safety/data-race-freedom for this test. */
static void sanity_check_chan_word(const alp_rpc_channel_t *ch)
{
	/* Matches the dispatcher's own access discipline (see
     * src/rpc_dispatch.c): `chan_word` is only ever touched through
     * __atomic_* builtins there, so a plain load here would itself be
     * a TEST-HARNESS data race under ThreadSanitizer, not a production
     * one -- RELAXED is enough since this is a best-effort snapshot,
     * not a synchronisation point. */
	uint32_t word = __atomic_load_n(&ch->chan_word, __ATOMIC_RELAXED);
	uint32_t lc   = word & ALP_RPC_CHAN_LC_MASK;
	uint32_t cnt  = word & ALP_RPC_CHAN_CNT_MASK;
	if (lc != ALP_RPC_CHAN_LC_UNOPENED && lc != ALP_RPC_CHAN_LC_OPEN &&
	    lc != ALP_RPC_CHAN_LC_CLOSING) {
		atomic_store(&g_recycle_bad_word, 1);
	}
	if (cnt > 1000u) {
		atomic_store(&g_recycle_bad_word, 1);
	}
}

static void *recycle_sender_thread(void *arg)
{
	(void)arg;
	uint8_t byte = 0x55u;
	while (!atomic_load(&g_recycle_stop)) {
		for (int i = 0; i < RECYCLE_POOL_SIZE; ++i) {
			alp_rpc_channel_t *ch = atomic_load(&g_recycle_slots[i]);
			if (ch == NULL) continue;
			sanity_check_chan_word(ch);

			alp_status_t rc = alp_rpc_send(ch, "ping", &byte, sizeof byte);
			if (!(rc == ALP_OK || rc == ALP_ERR_NOT_READY || rc == ALP_ERR_NOSUPPORT ||
			      rc == ALP_ERR_BUSY || rc == ALP_ERR_IO)) {
				atomic_store(&g_recycle_bad_rc, 1);
			}

			uint8_t      resp[8];
			size_t       resp_len = sizeof resp;
			alp_status_t rc2 =
			    alp_rpc_call(ch, "ping", &byte, sizeof byte, resp, &resp_len, 2u /*ms*/);
			if (!(rc2 == ALP_OK || rc2 == ALP_ERR_NOT_READY || rc2 == ALP_ERR_TIMEOUT ||
			      rc2 == ALP_ERR_NOSUPPORT || rc2 == ALP_ERR_NOMEM || rc2 == ALP_ERR_IO)) {
				atomic_store(&g_recycle_bad_rc, 1);
			}
		}
	}
	return NULL;
}

static void *recycle_closer_thread(void *arg)
{
	int slot = *(int *)arg;
	while (!atomic_load(&g_recycle_stop)) {
		alp_rpc_channel_t *ch = atomic_load(&g_recycle_slots[slot]);
		if (ch != NULL) {
			/* Un-publish first so sender threads stop picking this
             * slot's (about-to-be-stale) pointer before this thread
             * races alp_rpc_close() against them -- exactly the
             * send-vs-close race test_send_vs_close_race() above
             * already proves safe, now compounded with a concurrent
             * reopen cycling the SAME pool slot. */
			atomic_store(&g_recycle_slots[slot], NULL);
			alp_rpc_close(ch);
		}
		alp_rpc_config_t   cfg      = ALP_RPC_CONFIG_DEFAULT("recycle_stress");
		alp_rpc_channel_t *reopened = alp_rpc_open(&cfg);
		if (reopened != NULL) {
			atomic_store(&g_recycle_slots[slot], reopened);
		}
		/* NULL (ALP_ERR_NOMEM) is a legitimate transient outcome if
         * the OTHER closer thread's slot hasn't finished closing yet
         * -- retry on the next loop iteration. */
	}
	return NULL;
}

static void test_recycle_race_stress(void)
{
	atomic_store(&g_recycle_stop, 0);
	atomic_store(&g_recycle_bad_rc, 0);
	atomic_store(&g_recycle_bad_word, 0);

	for (int i = 0; i < RECYCLE_POOL_SIZE; ++i) {
		alp_rpc_config_t   cfg = ALP_RPC_CONFIG_DEFAULT("recycle_stress");
		alp_rpc_channel_t *h   = alp_rpc_open(&cfg);
		ALP_ASSERT_TRUE(h != NULL);
		atomic_store(&g_recycle_slots[i], h);
	}

	pthread_t senders[RECYCLE_SEND_THREADS];
	for (int i = 0; i < RECYCLE_SEND_THREADS; ++i) {
		ALP_ASSERT_EQ_INT(pthread_create(&senders[i], NULL, recycle_sender_thread, NULL), 0);
	}

	pthread_t closers[RECYCLE_POOL_SIZE];
	int       closer_slot[RECYCLE_POOL_SIZE];
	for (int i = 0; i < RECYCLE_POOL_SIZE; ++i) {
		closer_slot[i] = i;
		ALP_ASSERT_EQ_INT(pthread_create(&closers[i], NULL, recycle_closer_thread, &closer_slot[i]),
		                  0);
	}

	struct timespec ts = { .tv_sec = 0, .tv_nsec = RECYCLE_DURATION_MS * 1000000L };
	nanosleep(&ts, NULL);
	atomic_store(&g_recycle_stop, 1);

	for (int i = 0; i < RECYCLE_SEND_THREADS; ++i) {
		ALP_ASSERT_EQ_INT(pthread_join(senders[i], NULL), 0);
	}
	for (int i = 0; i < RECYCLE_POOL_SIZE; ++i) {
		ALP_ASSERT_EQ_INT(pthread_join(closers[i], NULL), 0);
	}

	ALP_ASSERT_TRUE(atomic_load(&g_recycle_bad_rc) == 0);
	ALP_ASSERT_TRUE(atomic_load(&g_recycle_bad_word) == 0);

	/* Prove no slot was ever leaked (permanently stuck non-UNOPENED)
     * or double-claimed: close whatever the stress loop left behind,
     * then re-open exactly RECYCLE_POOL_SIZE fresh channels -- every
     * one of them must succeed, which is only possible if every slot
     * cleanly made it back to UNOPENED. */
	for (int i = 0; i < RECYCLE_POOL_SIZE; ++i) {
		alp_rpc_channel_t *ch = atomic_load(&g_recycle_slots[i]);
		if (ch != NULL) alp_rpc_close(ch);
	}
	alp_rpc_channel_t *final_handles[RECYCLE_POOL_SIZE];
	for (int i = 0; i < RECYCLE_POOL_SIZE; ++i) {
		alp_rpc_config_t cfg = ALP_RPC_CONFIG_DEFAULT("recycle_stress_final");
		final_handles[i]     = alp_rpc_open(&cfg);
		ALP_ASSERT_TRUE(final_handles[i] != NULL);
	}
	for (int i = 0; i < RECYCLE_POOL_SIZE; ++i) {
		alp_rpc_close(final_handles[i]);
	}
}

int main(void)
{
	test_concurrent_close_is_single_shot();
	test_send_vs_close_race();
	test_recycle_race_stress();
	ALP_TEST_SUMMARY();
}
