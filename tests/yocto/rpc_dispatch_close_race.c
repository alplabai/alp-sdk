/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Dispatcher-path regression for GHSA-xhm8-7f87-93q5 defect 2: drives
 * the REAL public alp_rpc_open()/alp_rpc_close()/alp_rpc_send() surface
 * (src/rpc_dispatch.c), not a hand-built backend struct.
 * rpc_yocto_self_close.c already covers the BACKEND layer (defect 1) by
 * #including src/backends/rpc/yocto_drv.c directly and driving its
 * file-local rpc_rx_main()/y_close(); this file complements it by
 * covering src/rpc_dispatch.c's own lifecycle CAS (struct
 * alp_rpc_channel's `lifecycle` / `active_ops` / `in_use` -- see
 * src/backends/rpc/rpc_ops.h) through the ACTUAL alp_rpc_open() /
 * alp_rpc_close() entry points, so a regression to the plain
 * `!ch->in_use` TOCTOU this advisory's followup review flagged is
 * caught even on a host with no OpenAMP/libmetal installed at all.
 *
 * This links against the real alp::sdk static library and lets the
 * registry pick whichever "rpc" backend is available on this host
 * (src/backends/rpc/sw_fallback.c on a bare CI runner -- see its own
 * header comment: open/subscribe/send all return ALP_OK synchronously
 * and close is a no-op).  That is fine and deliberate: defect 2 lives
 * entirely in the dispatcher, ABOVE the ops vtable, so it is exercised
 * identically no matter which backend services the calls underneath --
 * unlike rpc_yocto_self_close.c, this file does not need (and does not
 * force) ALP_SDK_HAVE_OPENAMP_USERLAND.
 *
 * Two scenarios:
 *   1. test_concurrent_close_is_single_shot -- two threads race
 *      alp_rpc_close() on the SAME handle.  This dispatcher makes no
 *      thread-identity distinction between an "external" close and a
 *      channel's own "self" close the way the backend layer does (see
 *      rpc_yocto_self_close.c) -- both are just racing callers of
 *      alp_rpc_close(), which is exactly what struct alp_rpc_channel's
 *      lifecycle CAS (OPEN -> CLOSING) defends against.  Across many
 *      iterations: neither thread hangs or crashes, and a FRESH
 *      alp_rpc_open() after both return successfully reclaims a pool
 *      slot -- proving the CAS + active_ops drain released the slot
 *      back to `_alloc_rpc()` exactly once, not zero times (leaked
 *      forever) or in an inconsistent state (the recycle-hijack TOCTOU
 *      the plain `!ch->in_use` version of alp_rpc_close() had).
 *   2. test_send_vs_close_race -- one thread spins calling
 *      alp_rpc_send() while another concurrently closes the SAME
 *      handle; exercises alp_rpc_send()'s alp_handle_op_enter()/
 *      leave() guard racing alp_rpc_close()'s CAS + active_ops drain.
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

int main(void)
{
	test_concurrent_close_is_single_shot();
	test_send_vs_close_race();
	ALP_TEST_SUMMARY();
}
