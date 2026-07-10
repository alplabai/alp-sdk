/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for issue #629: the static handle-pool
 * allocators used an unlocked "if (!slot.in_use) { ...; in_use =
 * true; }" -- the exact reproduction quoted in the issue is a
 * two-pthread barrier stress against the default ONE-slot GPU2D
 * pool ("iterations=200000 duplicate successes=10": both concurrent
 * alp_gpu2d_open() calls succeeded and, because capacity is one,
 * necessarily returned the SAME handle address).
 *
 * test_concurrent_open_never_duplicates_a_slot below reproduces that
 * exact scenario deterministically -- two barriers per iteration
 * align both threads' alp_gpu2d_open() call as tightly as the kernel
 * allows, then let both threads observe EACH OTHER's result (the
 * barrier is itself a full memory synchronization point, so no
 * additional locking is needed to compare across threads) before
 * closing whatever they won and racing the next iteration.  Run
 * against the pre-#629 allocator this fails immediately (duplicate
 * count > 0, frequently with the same pointer in both slots); against
 * the alp_slot_try_claim()-based allocator it must stay at zero.
 *
 * test_close_races_blocked_op_without_uaf below reproduces the second
 * half of #629 -- alp_gpu2d_close() racing a concurrent
 * alp_gpu2d_fill_rect() on the SAME handle must never free the slot
 * out from under the in-flight op (which the sw_fallback backend
 * would otherwise touch after teardown -- a use-after-free the
 * fill_rect return code alone can't prove, but a crash/ASan trip
 * would).  The generic alp_handle_op_enter/leave/begin_close guard
 * (src/common/alp_slot_claim.h) makes this deterministic: close()
 * blocks until the racing op has left before it tears the handle
 * down, so this test's success criterion is simply "many concurrent
 * op-vs-close iterations complete without a crash and every return
 * code is one of the documented values."
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_gpu2d_slot_race
 *   ctest --test-dir build -R alp_test_gpu2d_slot_race
 */

#include <pthread.h>
#include <stdint.h>

#include "alp/gpu2d.h"
#include "alp/peripheral.h"

#include "test_assert.h"

#define RACE_ITERATIONS 4000

/* ------------------------------------------------------------------ */
/* Part 1: concurrent open() must never duplicate a slot.             */
/* ------------------------------------------------------------------ */

struct open_race_ctx {
	pthread_barrier_t start_barrier; /* release both opens together */
	pthread_barrier_t done_barrier;  /* both results visible to both */
	alp_gpu2d_t      *result_a;
	alp_gpu2d_t      *result_b;
};

struct open_race_out {
	int duplicates; /* iterations where both threads won a handle */
	int same_ptr;   /* of those, iterations where the pointer was identical */
};

static void *open_racer_a(void *arg)
{
	struct open_race_ctx *ctx = (struct open_race_ctx *)arg;
	struct open_race_out *out = malloc(sizeof(*out));
	out->duplicates           = 0;
	out->same_ptr             = 0;

	for (int i = 0; i < RACE_ITERATIONS; i++) {
		pthread_barrier_wait(&ctx->start_barrier);
		ctx->result_a = alp_gpu2d_open();
		pthread_barrier_wait(&ctx->done_barrier);

		/* Both result_a and result_b are visible here: the barrier
		 * above is a full synchronization point for every waiter. */
		if (ctx->result_a != NULL && ctx->result_b != NULL) {
			out->duplicates++;
			if (ctx->result_a == ctx->result_b) {
				out->same_ptr++;
			}
		}
		if (ctx->result_a != NULL) {
			alp_gpu2d_close(ctx->result_a);
		}
		/* Barrier C: don't start the next round's open() until B's
		 * close (below) has also happened, or the pool might still
		 * look "free" from A's perspective while B is mid-teardown
		 * of a handle A never touches -- harmless either way, but
		 * this keeps each round's before/after state deterministic
		 * for the assertions above. */
		pthread_barrier_wait(&ctx->start_barrier);
	}
	return out;
}

static void *open_racer_b(void *arg)
{
	struct open_race_ctx *ctx = (struct open_race_ctx *)arg;

	for (int i = 0; i < RACE_ITERATIONS; i++) {
		pthread_barrier_wait(&ctx->start_barrier);
		ctx->result_b = alp_gpu2d_open();
		pthread_barrier_wait(&ctx->done_barrier);

		if (ctx->result_b != NULL) {
			alp_gpu2d_close(ctx->result_b);
		}
		pthread_barrier_wait(&ctx->start_barrier);
	}
	return NULL;
}

static void test_concurrent_open_never_duplicates_a_slot(void)
{
	struct open_race_ctx  ctx = { 0 };
	pthread_t             t_a, t_b;
	struct open_race_out *out = NULL;

	ALP_ASSERT_EQ_INT(pthread_barrier_init(&ctx.start_barrier, NULL, 2), 0);
	ALP_ASSERT_EQ_INT(pthread_barrier_init(&ctx.done_barrier, NULL, 2), 0);

	ALP_ASSERT_EQ_INT(pthread_create(&t_a, NULL, open_racer_a, &ctx), 0);
	ALP_ASSERT_EQ_INT(pthread_create(&t_b, NULL, open_racer_b, &ctx), 0);

	void *ret_a = NULL;
	ALP_ASSERT_EQ_INT(pthread_join(t_a, &ret_a), 0);
	ALP_ASSERT_EQ_INT(pthread_join(t_b, NULL), 0);
	out = (struct open_race_out *)ret_a;

	pthread_barrier_destroy(&ctx.start_barrier);
	pthread_barrier_destroy(&ctx.done_barrier);

	/* Issue #629's own reproduction: with the default 1-slot pool,
	 * two concurrent opens must never BOTH succeed -- and if they
	 * ever did, they'd necessarily hand out the identical address
	 * (capacity is one). */
	ALP_ASSERT_EQ_INT(out->duplicates, 0);
	ALP_ASSERT_EQ_INT(out->same_ptr, 0);
	free(out);
}

/* ------------------------------------------------------------------ */
/* Part 2: close() must never free a slot an op is using.             */
/* ------------------------------------------------------------------ */

struct close_race_ctx {
	pthread_barrier_t   barrier;
	alp_gpu2d_t        *h;
	alp_gpu2d_surface_t surf;
	uint8_t             pixels[4 * 4 * 4]; /* 4x4 ARGB8888 */
	/* Per-iteration outcomes -- each written by exactly one thread and
	 * read only after both threads are joined, so (like
	 * last_error_threading.c's rationale) the shared ALP_ASSERT_*
	 * pass/fail counters in test_assert.h never get incremented
	 * concurrently by both racer threads. */
	alp_status_t op_rc[RACE_ITERATIONS];
	bool         reopen_ok[RACE_ITERATIONS];
};

static void *close_race_op_thread(void *arg)
{
	struct close_race_ctx *ctx = (struct close_race_ctx *)arg;

	for (int i = 0; i < RACE_ITERATIONS; i++) {
		pthread_barrier_wait(&ctx->barrier);
		/* Either this returns OK (op won the race against close),
		 * or NOT_READY (close won) -- both are documented, safe
		 * outcomes.  What must NEVER happen is a crash from
		 * touching backend state close() already tore down; that
		 * is the ASan/TSan-visible failure mode this test exists
		 * to exercise, not something the return code alone proves. */
		ctx->op_rc[i] = alp_gpu2d_fill_rect(ctx->h, &ctx->surf, 0, 0, 1, 1, 0xFFFFFFFFu);
		pthread_barrier_wait(&ctx->barrier);
	}
	return NULL;
}

static void *close_race_close_thread(void *arg)
{
	struct close_race_ctx *ctx = (struct close_race_ctx *)arg;

	for (int i = 0; i < RACE_ITERATIONS; i++) {
		pthread_barrier_wait(&ctx->barrier);
		alp_gpu2d_close(ctx->h); /* racing the op thread's fill_rect */
		pthread_barrier_wait(&ctx->barrier);
		/* Re-open for the next round (single-threaded here -- the
		 * op thread is blocked at the barrier). */
		ctx->h            = alp_gpu2d_open();
		ctx->reopen_ok[i] = (ctx->h != NULL);
	}
	return NULL;
}

static void test_close_races_blocked_op_without_uaf(void)
{
	struct close_race_ctx *ctx = calloc(1, sizeof(*ctx));
	pthread_t              t_op, t_close;

	ALP_ASSERT_TRUE(ctx != NULL);
	ctx->surf.base         = ctx->pixels;
	ctx->surf.width        = 4;
	ctx->surf.height       = 4;
	ctx->surf.stride_bytes = 4 * 4;
	ctx->surf.format       = ALP_GPU2D_FMT_ARGB8888;

	ctx->h = alp_gpu2d_open();
	ALP_ASSERT_TRUE(ctx->h != NULL);

	ALP_ASSERT_EQ_INT(pthread_barrier_init(&ctx->barrier, NULL, 2), 0);
	ALP_ASSERT_EQ_INT(pthread_create(&t_op, NULL, close_race_op_thread, ctx), 0);
	ALP_ASSERT_EQ_INT(pthread_create(&t_close, NULL, close_race_close_thread, ctx), 0);
	ALP_ASSERT_EQ_INT(pthread_join(t_op, NULL), 0);
	ALP_ASSERT_EQ_INT(pthread_join(t_close, NULL), 0);
	pthread_barrier_destroy(&ctx->barrier);

	/* Now that both threads are joined, checking every recorded
	 * per-iteration outcome is race-free. */
	for (int i = 0; i < RACE_ITERATIONS; i++) {
		ALP_ASSERT_TRUE(ctx->op_rc[i] == ALP_OK || ctx->op_rc[i] == ALP_ERR_NOT_READY);
		ALP_ASSERT_TRUE(ctx->reopen_ok[i]);
	}

	if (ctx->h != NULL) {
		alp_gpu2d_close(ctx->h);
	}
	free(ctx);
}

int main(void)
{
	test_concurrent_open_never_duplicates_a_slot();
	test_close_races_blocked_op_without_uaf();
	ALP_TEST_SUMMARY();
}
