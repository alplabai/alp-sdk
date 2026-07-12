/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mechanism-level race coverage for issue #629: the shared handle-pool
 * primitive in src/common/alp_slot_claim.h. Every class dispatcher
 * (gpio/uart/i2c/spi/adc/dac/pwm/... -- 20+ classes) now claims, guards,
 * and closes its static pool through the SAME four helpers, so a barrier
 * stress on the primitive itself proves the property for all of them at
 * once (the per-class end-to-end gpu2d_slot_race.c test then confirms the
 * wiring on one representative real backend).
 *
 * A synthetic pool mirrors the exact dispatcher shape: a struct whose
 * last member is `in_use`, with `lifecycle`/`active_ops` before it,
 * claimed via alp_slot_try_claim() + offsetof-zeroing, marked OPEN after
 * "init", operated under alp_handle_op_enter/leave, and torn down under
 * alp_handle_begin_close.
 *
 * Three scenarios, each the deterministic form of an acceptance-criteria
 * bullet:
 *   1. concurrent open never duplicates a slot (1-slot pool) and honours
 *      capacity (N-slot pool: exactly N winners, the rest get NULL).
 *   2. close never recycles a slot beneath an in-flight op -- the op,
 *      while entered, always observes live "backend state", never the
 *      poison a racing close writes after begin_close drains.
 *   3. concurrent double-close tears the handle down exactly once.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_slot_claim_race
 *   ctest --test-dir build -R alp_test_slot_claim_race
 * Run under ThreadSanitizer (issue #629 acceptance) by configuring with
 *   -DCMAKE_C_FLAGS="-fsanitize=thread -g".
 */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "alp_slot_claim.h"

#include "test_assert.h"

#define RACE_ITERATIONS 20000

/* Synthetic handle -- same field discipline as a real dispatcher slot:
 * in_use is the LAST member so a claimant may zero everything before it. */
#define BACKEND_MAGIC 0xA5A5A5A5u
struct fake_handle {
	uint32_t backend_state; /* BACKEND_MAGIC while live, 0 after teardown */
	uint8_t  lifecycle;
	uint32_t active_ops;
	bool     in_use;
};

/* ---- the four operations, exactly as a dispatcher wires them ---- */

static struct fake_handle *pool_alloc(struct fake_handle *pool, size_t cap)
{
	for (size_t i = 0; i < cap; ++i) {
		if (alp_slot_try_claim(&pool[i].in_use)) {
			memset(&pool[i], 0, offsetof(struct fake_handle, in_use));
			return &pool[i];
		}
	}
	return NULL;
}

static struct fake_handle *fake_open(struct fake_handle *pool, size_t cap)
{
	struct fake_handle *h = pool_alloc(pool, cap);
	if (h == NULL) {
		return NULL;
	}
	h->backend_state = BACKEND_MAGIC; /* "backend init" */
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
}

/* Returns true if the op ran (handle was open), false if it was
 * closed/closing. While entered, backend_state must read BACKEND_MAGIC --
 * anything else means close recycled the slot beneath us. */
static bool fake_op(struct fake_handle *h, bool *saw_poison)
{
	if (!alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return false;
	}
	if (h->backend_state != BACKEND_MAGIC) {
		*saw_poison = true;
	}
	alp_handle_op_leave(&h->active_ops);
	return true;
}

/* Returns true if this caller won the close and tore the handle down. */
static bool fake_close(struct fake_handle *h)
{
	if (!alp_handle_begin_close(&h->lifecycle, &h->active_ops)) {
		return false;
	}
	h->backend_state = 0u; /* poison: any op still reading this is a UAF */
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_UNOPENED);
	alp_slot_release(&h->in_use);
	return true;
}

/* ------------------------------------------------------------------ */
/* Part 1: concurrent open never duplicates a slot; capacity holds.   */
/* ------------------------------------------------------------------ */

struct open_race_ctx {
	pthread_barrier_t   start;
	pthread_barrier_t   done;
	struct fake_handle *pool;
	size_t              cap;
	struct fake_handle *result_a;
	struct fake_handle *result_b;
};

struct open_race_out {
	int duplicates;
	int same_ptr;
};

static void *open_racer_a(void *arg)
{
	struct open_race_ctx *ctx = arg;
	struct open_race_out *out = calloc(1, sizeof(*out));

	for (int i = 0; i < RACE_ITERATIONS; i++) {
		pthread_barrier_wait(&ctx->start);
		ctx->result_a = fake_open(ctx->pool, ctx->cap);
		pthread_barrier_wait(&ctx->done);
		if (ctx->result_a != NULL && ctx->result_b != NULL) {
			out->duplicates++;
			if (ctx->result_a == ctx->result_b) {
				out->same_ptr++;
			}
		}
		if (ctx->result_a != NULL) {
			fake_close(ctx->result_a);
		}
		pthread_barrier_wait(&ctx->start);
	}
	return out;
}

static void *open_racer_b(void *arg)
{
	struct open_race_ctx *ctx = arg;

	for (int i = 0; i < RACE_ITERATIONS; i++) {
		pthread_barrier_wait(&ctx->start);
		ctx->result_b = fake_open(ctx->pool, ctx->cap);
		pthread_barrier_wait(&ctx->done);
		if (ctx->result_b != NULL) {
			fake_close(ctx->result_b);
		}
		pthread_barrier_wait(&ctx->start);
	}
	return NULL;
}

static void test_concurrent_open_never_duplicates_one_slot(void)
{
	struct fake_handle   pool[1] = { 0 };
	struct open_race_ctx ctx     = { .pool = pool, .cap = 1 };
	pthread_t            t_a, t_b;
	void                *ret_a = NULL;

	ALP_ASSERT_EQ_INT(pthread_barrier_init(&ctx.start, NULL, 2), 0);
	ALP_ASSERT_EQ_INT(pthread_barrier_init(&ctx.done, NULL, 2), 0);
	ALP_ASSERT_EQ_INT(pthread_create(&t_a, NULL, open_racer_a, &ctx), 0);
	ALP_ASSERT_EQ_INT(pthread_create(&t_b, NULL, open_racer_b, &ctx), 0);
	ALP_ASSERT_EQ_INT(pthread_join(t_a, &ret_a), 0);
	ALP_ASSERT_EQ_INT(pthread_join(t_b, NULL), 0);
	pthread_barrier_destroy(&ctx.start);
	pthread_barrier_destroy(&ctx.done);

	struct open_race_out *out = ret_a;
	/* Pre-#629 (plain check-then-set) this trips immediately; the atomic
	 * claim keeps both counters at zero. Capacity is one, so a duplicate
	 * would necessarily be the identical pointer. */
	ALP_ASSERT_EQ_INT(out->duplicates, 0);
	ALP_ASSERT_EQ_INT(out->same_ptr, 0);
	free(out);
}

/* N threads race a capacity-N pool: exactly N win distinct slots, the
 * (N+extra) losers all get NULL. Proves "capacity exhaustion returns
 * NOMEM" (here: NULL) with no over-subscription and no duplicate slot. */
#define CAP_POOL    4
#define CAP_THREADS 8

struct cap_ctx {
	pthread_barrier_t   start;
	struct fake_handle *pool;
	struct fake_handle *won[CAP_THREADS];
};

static void *cap_worker(void *arg)
{
	struct cap_ctx *ctx = ((void **)arg)[0];
	size_t          i   = (size_t)(uintptr_t)((void **)arg)[1];
	pthread_barrier_wait(&ctx->start);
	ctx->won[i] = pool_alloc(ctx->pool, CAP_POOL);
	return NULL;
}

static void test_capacity_is_never_oversubscribed(void)
{
	struct fake_handle pool[CAP_POOL] = { 0 };
	struct cap_ctx     ctx            = { .pool = pool };
	pthread_t          th[CAP_THREADS];
	void              *args[CAP_THREADS][2];

	ALP_ASSERT_EQ_INT(pthread_barrier_init(&ctx.start, NULL, CAP_THREADS), 0);
	for (size_t i = 0; i < CAP_THREADS; i++) {
		args[i][0] = &ctx;
		args[i][1] = (void *)(uintptr_t)i;
		ALP_ASSERT_EQ_INT(pthread_create(&th[i], NULL, cap_worker, args[i]), 0);
	}
	for (size_t i = 0; i < CAP_THREADS; i++) {
		ALP_ASSERT_EQ_INT(pthread_join(th[i], NULL), 0);
	}
	pthread_barrier_destroy(&ctx.start);

	int winners = 0;
	for (size_t i = 0; i < CAP_THREADS; i++) {
		if (ctx.won[i] != NULL) {
			winners++;
		}
	}
	ALP_ASSERT_EQ_INT(winners, CAP_POOL); /* exactly N, never more */

	/* No two winners share a slot address. */
	for (size_t i = 0; i < CAP_THREADS; i++) {
		for (size_t j = i + 1; j < CAP_THREADS; j++) {
			if (ctx.won[i] != NULL && ctx.won[j] != NULL) {
				ALP_ASSERT_TRUE(ctx.won[i] != ctx.won[j]);
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* Part 2: close never recycles a slot beneath an in-flight op.       */
/* ------------------------------------------------------------------ */

struct close_race_ctx {
	pthread_barrier_t   barrier;
	struct fake_handle *pool;
	struct fake_handle *h;
	bool                op_ran[RACE_ITERATIONS];
	bool                reopen_ok[RACE_ITERATIONS];
	bool                saw_poison; /* must stay false: the UAF signal */
};

static void *close_race_op_thread(void *arg)
{
	struct close_race_ctx *ctx = arg;
	for (int i = 0; i < RACE_ITERATIONS; i++) {
		pthread_barrier_wait(&ctx->barrier);
		ctx->op_ran[i] = fake_op(ctx->h, &ctx->saw_poison);
		pthread_barrier_wait(&ctx->barrier);
	}
	return NULL;
}

static void *close_race_close_thread(void *arg)
{
	struct close_race_ctx *ctx = arg;
	for (int i = 0; i < RACE_ITERATIONS; i++) {
		pthread_barrier_wait(&ctx->barrier);
		fake_close(ctx->h);
		pthread_barrier_wait(&ctx->barrier);
		/* Re-open for the next round (op thread parked at the barrier). */
		ctx->h            = fake_open(ctx->pool, 1);
		ctx->reopen_ok[i] = (ctx->h != NULL);
	}
	return NULL;
}

static void test_close_never_recycles_under_active_op(void)
{
	struct fake_handle     pool[1] = { 0 };
	struct close_race_ctx *ctx     = calloc(1, sizeof(*ctx));
	pthread_t              t_op, t_close;

	ALP_ASSERT_TRUE(ctx != NULL);
	ctx->pool = pool;
	ctx->h    = fake_open(pool, 1);
	ALP_ASSERT_TRUE(ctx->h != NULL);

	ALP_ASSERT_EQ_INT(pthread_barrier_init(&ctx->barrier, NULL, 2), 0);
	ALP_ASSERT_EQ_INT(pthread_create(&t_op, NULL, close_race_op_thread, ctx), 0);
	ALP_ASSERT_EQ_INT(pthread_create(&t_close, NULL, close_race_close_thread, ctx), 0);
	ALP_ASSERT_EQ_INT(pthread_join(t_op, NULL), 0);
	ALP_ASSERT_EQ_INT(pthread_join(t_close, NULL), 0);
	pthread_barrier_destroy(&ctx->barrier);

	/* The op that entered before close's CAS is drained by begin_close, so
	 * it always saw live backend state; the op that lost to the CAS backed
	 * out before touching state. Either way, never the poison. */
	ALP_ASSERT_TRUE(!ctx->saw_poison);
	for (int i = 0; i < RACE_ITERATIONS; i++) {
		ALP_ASSERT_TRUE(ctx->reopen_ok[i]);
	}
	free(ctx);
}

/* ------------------------------------------------------------------ */
/* Part 3: concurrent double-close tears down exactly once.           */
/* ------------------------------------------------------------------ */

struct dclose_ctx {
	pthread_barrier_t   start;
	struct fake_handle *pool;
	struct fake_handle *h;
	int                 won_a;
	int                 won_b;
};

static void *dclose_a(void *arg)
{
	struct dclose_ctx *ctx = arg;
	for (int i = 0; i < RACE_ITERATIONS; i++) {
		pthread_barrier_wait(&ctx->start);
		if (fake_close(ctx->h)) {
			ctx->won_a++;
		}
		pthread_barrier_wait(&ctx->start);
	}
	return NULL;
}

/* The b-thread also re-opens between rounds so every round has a live
 * handle for both closers to race. The re-open runs while the a-thread is
 * parked at the second barrier, so it is single-threaded. */
static void *dclose_b(void *arg)
{
	struct dclose_ctx *ctx = arg;
	for (int i = 0; i < RACE_ITERATIONS; i++) {
		pthread_barrier_wait(&ctx->start);
		if (fake_close(ctx->h)) {
			ctx->won_b++;
		}
		pthread_barrier_wait(&ctx->start);
		ctx->h = fake_open(ctx->pool, 1);
	}
	return NULL;
}

static void test_double_close_tears_down_exactly_once(void)
{
	struct fake_handle pool[1] = { 0 };
	struct dclose_ctx  ctx     = { .pool = pool };
	pthread_t          t_a, t_b;

	ctx.h = fake_open(pool, 1);
	ALP_ASSERT_TRUE(ctx.h != NULL);

	ALP_ASSERT_EQ_INT(pthread_barrier_init(&ctx.start, NULL, 2), 0);
	ALP_ASSERT_EQ_INT(pthread_create(&t_a, NULL, dclose_a, &ctx), 0);
	ALP_ASSERT_EQ_INT(pthread_create(&t_b, NULL, dclose_b, &ctx), 0);
	ALP_ASSERT_EQ_INT(pthread_join(t_a, NULL), 0);
	ALP_ASSERT_EQ_INT(pthread_join(t_b, NULL), 0);
	pthread_barrier_destroy(&ctx.start);

	/* Exactly one close won per round (the other saw CLOSING/UNOPENED and
	 * no-oped), so total teardowns == RACE_ITERATIONS. */
	ALP_ASSERT_EQ_INT(ctx.won_a + ctx.won_b, RACE_ITERATIONS);
}

int main(void)
{
	test_concurrent_open_never_duplicates_one_slot();
	test_capacity_is_never_oversubscribed();
	test_close_never_recycles_under_active_op();
	test_double_close_tears_down_exactly_once();
	ALP_TEST_SUMMARY();
}
