/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression for issue #629 on the Yocto inference dispatcher
 * (src/yocto/inference_yocto.c): alp_inference_invoke() and
 * alp_inference_close() used to both gate on a bare `in_use` check, so
 * a close() racing an in-flight invoke() (ORT api->Run(), the DEEPX
 * engine, the DRP-AI runtime -- hundreds of ms on an A55) could delete
 * be_state and release the pool slot while the invoke was still
 * reading/writing it -- a use-after-free.
 *
 * Drives the REAL public alp_inference_open()/alp_inference_invoke()/
 * alp_inference_close() by #including src/yocto/inference_yocto.c
 * directly into this TU (same technique as
 * tests/yocto/can_add_filter_close_race.c) through a fake
 * DEEPX_DXM1 backend defined below: ALP_SDK_USE_DEEPX_DXM1 is forced
 * on before the #include so the dispatcher's real switch statements
 * pick this file's alp_inference_deepx_*() stand-ins -- the real
 * dispatch/gating code (struct alp_inference, pool_acquire/release,
 * alp_handle_op_enter/leave, alp_handle_begin_close_blocking) runs
 * completely unmodified; only the backend "invoke" is faked, as a
 * poison check wrapped around a short sleep standing in for a real
 * model executor's hundreds-of-ms run.
 *
 * The `g_entered` barrier forces invoke()'s op_enter (and thus its
 * active_ops count) to have already happened before the racing
 * close() thread issues its begin_close_blocking() CAS -- without it,
 * the two threads racing off a single shared start would let close
 * win most rounds before invoke's gate even ran (see
 * tests/yocto/slot_claim_race.c's fake_blocking_op() doc comment for
 * the same rationale) -- so this is the deterministic form of the
 * interleaving THE_DEFECT describes, not a best-effort race.
 *
 * Mutation-tested (see this file's own header, not committed):
 * reverting alp_inference_invoke() to a bare `inf->in_use` check and
 * alp_inference_close() to `in_use = false` with no op-count drain
 * makes test_close_races_invoke_never_sees_poison() fail (poison
 * observed) every run; the fixed dispatcher passes every run.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_inference_invoke_close_race
 *   ctest --test-dir build -R alp_test_inference_invoke_close_race
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#include "test_assert.h"

#define ALP_SDK_USE_DEEPX_DXM1 1
#include "../../src/yocto/inference_yocto.c"

#define RACE_ITERATIONS 200
#define FAKE_MAGIC      0xA5A5A5A5u

typedef struct {
	volatile uint32_t magic;
} fake_be_t;

static pthread_barrier_t g_entered; /* invoke thread + closer thread */
static atomic_int        g_saw_poison;
static atomic_int        g_invoke_rc_ok;

alp_status_t alp_inference_deepx_open(struct alp_inference *h, const alp_inference_config_t *cfg)
{
	(void)cfg;
	static fake_be_t st;
	st.magic    = FAKE_MAGIC;
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

/* Stands in for a real backend's synchronous model executor (ORT
 * api->Run(), hundreds of ms on an A55): signals g_entered (so the
 * racing closer knows this op is already counted and mid-run) then
 * sleeps briefly before touching be_state again -- exactly the window
 * a close() without the op-enter/leave + blocking-drain fix could
 * recycle out from under it. */
alp_status_t alp_inference_deepx_invoke(struct alp_inference *h)
{
	fake_be_t *st = (fake_be_t *)h->be_state;
	pthread_barrier_wait(&g_entered);
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 5000000L }; /* 5ms "Run()" */
	nanosleep(&ts, NULL);
	if (st == NULL || st->magic != FAKE_MAGIC) {
		atomic_store(&g_saw_poison, 1);
		return ALP_ERR_IO;
	}
	return ALP_OK;
}

/* Poisons be_state -- never frees it (a function-local static, not a
 * heap allocation) so a UAF shows up as the poison check above
 * failing rather than relying on an ASan crash to catch it. */
void alp_inference_deepx_close(struct alp_inference *h)
{
	fake_be_t *st = (fake_be_t *)h->be_state;
	if (st != NULL) st->magic = 0u;
	h->be_state = NULL;
}

static void *invoke_thread(void *arg)
{
	alp_inference_t *h  = arg;
	alp_status_t     rc = alp_inference_invoke(h);
	/* ALP_OK (invoke ran to completion before close's drain could have
	 * raced it) or ALP_ERR_NOT_READY (close's CAS won before op_enter)
	 * are both correct outcomes -- the property under test is "no
	 * poison observed", not which one comes back. */
	if (rc == ALP_OK || rc == ALP_ERR_NOT_READY) {
		atomic_store(&g_invoke_rc_ok, 1);
	}
	return NULL;
}

static void *closer_thread(void *arg)
{
	alp_inference_t *h = arg;
	pthread_barrier_wait(&g_entered);
	alp_inference_close(h);
	return NULL;
}

static void test_close_races_invoke_never_sees_poison(void)
{
	static const uint8_t model[16] = { 0xDE, 0xAD, 0xBE, 0xEF };

	for (int i = 0; i < RACE_ITERATIONS; ++i) {
		ALP_ASSERT_EQ_INT(pthread_barrier_init(&g_entered, NULL, 2), 0);
		atomic_store(&g_saw_poison, 0);
		atomic_store(&g_invoke_rc_ok, 0);

		alp_inference_config_t cfg = {
			.model_data = model,
			.model_size = sizeof(model),
			.backend    = ALP_INFERENCE_BACKEND_DEEPX_DXM1,
		};
		alp_inference_t *h = alp_inference_open(&cfg);
		ALP_ASSERT_TRUE(h != NULL);

		pthread_t t_invoke, t_close;
		ALP_ASSERT_EQ_INT(pthread_create(&t_invoke, NULL, invoke_thread, h), 0);
		ALP_ASSERT_EQ_INT(pthread_create(&t_close, NULL, closer_thread, h), 0);
		ALP_ASSERT_EQ_INT(pthread_join(t_invoke, NULL), 0);
		ALP_ASSERT_EQ_INT(pthread_join(t_close, NULL), 0);
		pthread_barrier_destroy(&g_entered);

		ALP_ASSERT_TRUE(!atomic_load(&g_saw_poison));
		ALP_ASSERT_TRUE(atomic_load(&g_invoke_rc_ok));
	}
}

int main(void)
{
	test_close_races_invoke_never_sees_poison();
	ALP_TEST_SUMMARY();
}
