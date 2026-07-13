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
 * Five scenarios, each the deterministic form of an acceptance-criteria
 * bullet:
 *   1. concurrent open never duplicates a slot (1-slot pool) and honours
 *      capacity (N-slot pool: exactly N winners, the rest get NULL).
 *   2. close never recycles a slot beneath an in-flight op -- the op,
 *      while entered, always observes live "backend state", never the
 *      poison a racing close writes after begin_close drains.
 *   3. concurrent double-close tears the handle down exactly once.
 *   4. the sleep-poll alp_handle_begin_close_blocking() (issue #629's
 *      generalisation of rpc_dispatch.c's _rpc_drain(), GHSA-xhm8) never
 *      recycles a slot beneath a COUNTED op that genuinely blocks (mirrors
 *      ble/mqtt/wifi/camera/mproc/usb's connect()/publish()/capture()/
 *      send()/lock()/read()/write() timeout_ms ops) -- the case a
 *      lifecycle-byte-only check (the pre-generalisation fallback those
 *      dispatchers used) would have missed, since a racing close would
 *      have read active_ops == 0 (the op was never counted there) and
 *      won its CAS while the op was still in flight. A second
 *      barrier (`entered`, see fake_blocking_op()) forces the op to
 *      have already been counted before close's CAS runs each round,
 *      so this deterministically exercises the drain-wait path
 *      instead of racing the two threads' entry/CAS off one shared
 *      start barrier (which let a mutated, uncounted op miss the bug
 *      most rounds).
 *   5. the same drain-wait property, proven again against the specific
 *      call shape src/audio_dispatch.c's alp_audio_in_read()/
 *      alp_audio_out_write() and src/i2s_dispatch.c's alp_i2s_write()/
 *      alp_i2s_read() use: a pure NULL/len==0 param-check bail AHEAD of
 *      the counted gate (fake_audio_style_op()) -- these four were the
 *      blocking ops issue #629's first pass missed (they kept the
 *      lifecycle-byte-only fallback #4 replaced everywhere else), so
 *      this is their dedicated regression. A companion non-racy check
 *      confirms the param-check bail itself never touches active_ops.
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
#include <time.h>

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

/* ------------------------------------------------------------------ */
/* Part 4: sleep-poll close never recycles a slot beneath a counted    */
/* BLOCKING op (issue #629 blocking-op generalisation).                */
/* ------------------------------------------------------------------ */

/* Fewer, longer rounds than the short-sync races above: each round
 * genuinely sleeps to stand in for a caller's timeout_ms round-trip
 * (a real link-layer/broker/transfer wait in the production
 * dispatchers), so this stays a bounded, fast test rather than
 * RACE_ITERATIONS iterations of a multi-ms sleep. */
#define BLOCKING_RACE_ITERATIONS 200

/* Same shape as fake_op() above, but the op is held open across a
 * short real sleep -- standing in for the caller-supplied timeout_ms a
 * genuinely blocking call (alp_ble_connect/alp_mqtt_connect/
 * alp_camera_capture/alp_mbox_send/alp_hwsem_lock/
 * alp_usb_device_read|write) can take. Counted via
 * alp_handle_op_enter/leave, exactly like those dispatchers convert to
 * for issue #629, so alp_handle_begin_close_blocking()'s drain must
 * wait for it -- a lifecycle-byte-only check would not have counted
 * it at all, letting a racing close's CAS win immediately.
 *
 * `entered` is a SECOND barrier (distinct from the round's start/sync
 * barrier) that this function releases right after the entry gate
 * succeeds and BEFORE the sleep: without it, the op and close threads
 * are both simply released off the same start barrier and race
 * freely, so close's single CAS routinely wins before this op's gate
 * even runs -- the op then reads CLOSING and backs out without ever
 * being counted, and the drain-wait path never gets exercised
 * (measured: the bug this scenario exists to catch was missed 4/5
 * runs). Waiting for `entered` on the close side (see
 * blocking_close_race_close_thread() below) instead makes the
 * ordering deterministic: the op is always counted-and-mid-sleep
 * before close's CAS runs, every round. */
static bool fake_blocking_op(struct fake_handle *h, bool *saw_poison, pthread_barrier_t *entered)
{
	bool counted = alp_handle_op_enter(&h->lifecycle, &h->active_ops);
	/* Release the close thread now regardless of `counted`: in this
	 * test's protocol the handle is always freshly OPEN at round start
	 * (close waits for this signal before touching it), so entry always
	 * succeeds in practice -- but never leave the close thread parked on
	 * `entered` forever if that ever changes. */
	pthread_barrier_wait(entered);
	if (!counted) {
		return false;
	}
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 2000000L }; /* 2 ms "round-trip" */
	nanosleep(&ts, NULL);
	if (h->backend_state != BACKEND_MAGIC) {
		*saw_poison = true;
	}
	alp_handle_op_leave(&h->active_ops);
	return true;
}

/* Sleep-poll close (issue #629 generalisation of rpc_dispatch.c's
 * _rpc_begin_close()/_rpc_drain(), GHSA-xhm8): CAS OPEN->CLOSING, then
 * drain active_ops via alp_handle_begin_close_blocking() (sleeps
 * between polls -- see src/common/alp_slot_claim.c) instead of the
 * busy-spin alp_handle_begin_close(). */
static bool fake_close_blocking(struct fake_handle *h)
{
	if (!alp_handle_begin_close_blocking(&h->lifecycle, &h->active_ops)) {
		return false;
	}
	h->backend_state = 0u; /* poison: any op still reading this is a UAF */
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_UNOPENED);
	alp_slot_release(&h->in_use);
	return true;
}

struct blocking_close_race_ctx {
	pthread_barrier_t   barrier;
	pthread_barrier_t   entered; /* op-counted-and-about-to-sleep signal */
	struct fake_handle *pool;
	struct fake_handle *h;
	bool                op_ran[BLOCKING_RACE_ITERATIONS];
	bool                reopen_ok[BLOCKING_RACE_ITERATIONS];
	bool                saw_poison; /* must stay false: the UAF signal */
};

static void *blocking_close_race_op_thread(void *arg)
{
	struct blocking_close_race_ctx *ctx = arg;
	for (int i = 0; i < BLOCKING_RACE_ITERATIONS; i++) {
		pthread_barrier_wait(&ctx->barrier);
		ctx->op_ran[i] = fake_blocking_op(ctx->h, &ctx->saw_poison, &ctx->entered);
		pthread_barrier_wait(&ctx->barrier);
	}
	return NULL;
}

static void *blocking_close_race_close_thread(void *arg)
{
	struct blocking_close_race_ctx *ctx = arg;
	for (int i = 0; i < BLOCKING_RACE_ITERATIONS; i++) {
		pthread_barrier_wait(&ctx->barrier);
		/* Wait for the op thread to have run its entry gate (and, when
		 * counted, be sitting in its sleep with active_ops already
		 * incremented) before issuing the CAS -- see fake_blocking_op()'s
		 * doc comment: without this second barrier, this thread's CAS
		 * routinely wins the race off the shared start barrier before
		 * the op's gate even runs, and this scenario never exercises the
		 * drain-wait path it exists to prove. */
		pthread_barrier_wait(&ctx->entered);
		fake_close_blocking(ctx->h);
		pthread_barrier_wait(&ctx->barrier);
		/* Re-open for the next round (op thread parked at the barrier). */
		ctx->h            = fake_open(ctx->pool, 1);
		ctx->reopen_ok[i] = (ctx->h != NULL);
	}
	return NULL;
}

static void test_blocking_close_never_recycles_under_counted_blocking_op(void)
{
	struct fake_handle              pool[1] = { 0 };
	struct blocking_close_race_ctx *ctx     = calloc(1, sizeof(*ctx));
	pthread_t                       t_op, t_close;

	ALP_ASSERT_TRUE(ctx != NULL);
	ctx->pool = pool;
	ctx->h    = fake_open(pool, 1);
	ALP_ASSERT_TRUE(ctx->h != NULL);

	ALP_ASSERT_EQ_INT(pthread_barrier_init(&ctx->barrier, NULL, 2), 0);
	ALP_ASSERT_EQ_INT(pthread_barrier_init(&ctx->entered, NULL, 2), 0);
	ALP_ASSERT_EQ_INT(pthread_create(&t_op, NULL, blocking_close_race_op_thread, ctx), 0);
	ALP_ASSERT_EQ_INT(pthread_create(&t_close, NULL, blocking_close_race_close_thread, ctx), 0);
	ALP_ASSERT_EQ_INT(pthread_join(t_op, NULL), 0);
	ALP_ASSERT_EQ_INT(pthread_join(t_close, NULL), 0);
	pthread_barrier_destroy(&ctx->barrier);
	pthread_barrier_destroy(&ctx->entered);

	/* The `entered` barrier guarantees the op is always counted (its
	 * alp_handle_op_enter() has already run) and mid-sleep by the time
	 * close's CAS fires, so begin_close_blocking() is deterministically
	 * forced onto its sleep-poll drain-wait path every round -- it must
	 * wait for the op's alp_handle_op_leave() before it may poison
	 * backend_state. Never the poison -- the property a lifecycle-byte-
	 * only fallback (uncounted) would NOT have held, since close's CAS
	 * would win immediately against active_ops == 0 while the op was
	 * still mid-sleep, poisoning state out from underneath it. */
	ALP_ASSERT_TRUE(!ctx->saw_poison);
	for (int i = 0; i < BLOCKING_RACE_ITERATIONS; i++) {
		ALP_ASSERT_TRUE(ctx->reopen_ok[i]);
	}
	free(ctx);
}

/* ------------------------------------------------------------------ */
/* Part 5: audio/i2s-style op -- a pure param-check bail AHEAD of the  */
/* counted blocking gate -- vs the sleep-poll close (issue #629        */
/* follow-up: src/audio_dispatch.c's alp_audio_in_read()/              */
/* alp_audio_out_write() and src/i2s_dispatch.c's alp_i2s_write()/     */
/* alp_i2s_read() were the 4 blocking ops the first #629 pass missed   */
/* -- they gated on alp_lifecycle_get() != OPEN only, UNcounted, so    */
/* their close()s used the busy-spin alp_handle_begin_close() and      */
/* never waited for one of these to drain).                            */
/* ------------------------------------------------------------------ */

/*
 * Mirrors the exact shape those four ops now use: `len == 0` stands in
 * for their `buf == NULL || frames == 0` (i2s: `block == NULL || bytes
 * == 0u`) pre-gate param check, which must bail WITHOUT touching the
 * handle at all -- no lifecycle read, no active_ops increment -- ahead
 * of the counted alp_handle_op_enter()/alp_handle_op_leave() gate that
 * lets alp_handle_begin_close_blocking() drain a genuinely blocking
 * transfer instead of racing it (see fake_blocking_op() above for the
 * `entered`-barrier rationale, reused here unchanged).
 *
 * Mutation-tested (issue #629 follow-up): temporarily replacing the
 * `alp_handle_op_enter(...)` call below with the pre-fix
 * `alp_lifecycle_get(&h->lifecycle) == ALP_HANDLE_LC_OPEN` gate (i.e.
 * skipping the counter entirely, exactly what audio_dispatch.c/
 * i2s_dispatch.c did before this follow-up) makes
 * test_close_never_recycles_under_counted_audio_style_op() below fail
 * every run (saw_poison trips) since the sleep-poll close's drain has
 * nothing to wait on; restoring the counted gate makes it pass every
 * run.
 */
static bool
fake_audio_style_op(struct fake_handle *h, size_t len, bool *saw_poison, pthread_barrier_t *entered)
{
	if (len == 0) {
		return false; /* pure param check before the gate: no h deref at all */
	}
	bool counted = alp_handle_op_enter(&h->lifecycle, &h->active_ops);
	pthread_barrier_wait(entered);
	if (!counted) {
		return false;
	}
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 2000000L }; /* 2 ms "transfer" */
	nanosleep(&ts, NULL);
	if (h->backend_state != BACKEND_MAGIC) {
		*saw_poison = true;
	}
	alp_handle_op_leave(&h->active_ops);
	return true;
}

struct audio_style_race_ctx {
	pthread_barrier_t   barrier;
	pthread_barrier_t   entered;
	struct fake_handle *pool;
	struct fake_handle *h;
	bool                op_ran[BLOCKING_RACE_ITERATIONS];
	bool                reopen_ok[BLOCKING_RACE_ITERATIONS];
	bool                saw_poison;
};

static void *audio_style_race_op_thread(void *arg)
{
	struct audio_style_race_ctx *ctx = arg;
	for (int i = 0; i < BLOCKING_RACE_ITERATIONS; i++) {
		pthread_barrier_wait(&ctx->barrier);
		ctx->op_ran[i] = fake_audio_style_op(
		    ctx->h, 1u /* nonzero: always proceeds */, &ctx->saw_poison, &ctx->entered);
		pthread_barrier_wait(&ctx->barrier);
	}
	return NULL;
}

static void *audio_style_race_close_thread(void *arg)
{
	struct audio_style_race_ctx *ctx = arg;
	for (int i = 0; i < BLOCKING_RACE_ITERATIONS; i++) {
		pthread_barrier_wait(&ctx->barrier);
		/* See blocking_close_race_close_thread() above: wait for the op
		 * to be counted-and-mid-sleep before issuing the CAS, or this
		 * thread's CAS routinely wins off the shared start barrier
		 * before the op's gate even runs. */
		pthread_barrier_wait(&ctx->entered);
		fake_close_blocking(ctx->h);
		pthread_barrier_wait(&ctx->barrier);
		ctx->h            = fake_open(ctx->pool, 1);
		ctx->reopen_ok[i] = (ctx->h != NULL);
	}
	return NULL;
}

static void test_close_never_recycles_under_counted_audio_style_op(void)
{
	struct fake_handle           pool[1] = { 0 };
	struct audio_style_race_ctx *ctx     = calloc(1, sizeof(*ctx));
	pthread_t                    t_op, t_close;

	ALP_ASSERT_TRUE(ctx != NULL);
	ctx->pool = pool;
	ctx->h    = fake_open(pool, 1);
	ALP_ASSERT_TRUE(ctx->h != NULL);

	ALP_ASSERT_EQ_INT(pthread_barrier_init(&ctx->barrier, NULL, 2), 0);
	ALP_ASSERT_EQ_INT(pthread_barrier_init(&ctx->entered, NULL, 2), 0);
	ALP_ASSERT_EQ_INT(pthread_create(&t_op, NULL, audio_style_race_op_thread, ctx), 0);
	ALP_ASSERT_EQ_INT(pthread_create(&t_close, NULL, audio_style_race_close_thread, ctx), 0);
	ALP_ASSERT_EQ_INT(pthread_join(t_op, NULL), 0);
	ALP_ASSERT_EQ_INT(pthread_join(t_close, NULL), 0);
	pthread_barrier_destroy(&ctx->barrier);
	pthread_barrier_destroy(&ctx->entered);

	/* Same property as Part 4, proven again against the audio/i2s call
	 * shape specifically: never the poison. */
	ALP_ASSERT_TRUE(!ctx->saw_poison);
	for (int i = 0; i < BLOCKING_RACE_ITERATIONS; i++) {
		ALP_ASSERT_TRUE(ctx->reopen_ok[i]);
	}
	free(ctx);
}

/* Non-racy companion check: the param-check bail (len == 0) must never
 * touch the handle -- active_ops stays 0 and the lifecycle byte stays
 * whatever it already was, so a close() racing a rejected call never
 * has anything to drain for it. */
static void test_audio_style_op_param_bail_never_touches_handle(void)
{
	struct fake_handle h = { 0 };
	pthread_barrier_t  unused_entered;
	bool               saw_poison = false;

	ALP_ASSERT_EQ_INT(pthread_barrier_init(&unused_entered, NULL, 1), 0);
	alp_lifecycle_set(&h.lifecycle, ALP_HANDLE_LC_OPEN);
	ALP_ASSERT_TRUE(!fake_audio_style_op(&h, 0u, &saw_poison, &unused_entered));
	ALP_ASSERT_EQ_INT((int)h.active_ops, 0);
	ALP_ASSERT_TRUE(!saw_poison);
	pthread_barrier_destroy(&unused_entered);
}

int main(void)
{
	test_concurrent_open_never_duplicates_one_slot();
	test_capacity_is_never_oversubscribed();
	test_close_never_recycles_under_active_op();
	test_double_close_tears_down_exactly_once();
	test_blocking_close_never_recycles_under_counted_blocking_op();
	test_close_never_recycles_under_counted_audio_style_op();
	test_audio_style_op_param_bail_never_touches_handle();
	ALP_TEST_SUMMARY();
}
