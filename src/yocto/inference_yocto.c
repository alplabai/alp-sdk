/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Yocto / Linux-user-space backend for <alp/inference.h>.
 *
 * Mirrors the role of src/zephyr/inference_zephyr.c: owns the
 * public-API glue (handle pool, cfg validation, backend dispatch)
 * and delegates the actual neural-network execution to a per-backend
 * helper compiled in only when the matching ALP_SDK_USE_<BACKEND>
 * CMake option is on.
 *
 * Backend dispatch (Yocto / V2N-M1 priority)
 *   ALP_INFERENCE_BACKEND_AUTO     -> picks the first available real
 *                                     backend in this order: DEEPX_DX,
 *                                     ETHOS_U, DRPAI, CPU.  DEEPX_DX
 *                                     comes first on V2N-M1 builds
 *                                     because that's the SoM's reason
 *                                     for shipping a companion NPU; CPU
 *                                     is deliberately LAST -- an
 *                                     NPU-bearing SoM must never
 *                                     silently fall to CPU under AUTO.
 *   ALP_INFERENCE_BACKEND_DEEPX_DXM1 -> DEEPX DX-M1 (proprietary).  Real
 *                                     dispatch lands in v0.4 once
 *                                     deepx-dxm1-host-sdk is on the
 *                                     sysroot; v0.3 wires routing only.
 *   ALP_INFERENCE_BACKEND_ETHOS_U  -> reserved for the i.MX 93 Yocto
 *                                     path (task #14).  Adapter slot
 *                                     stays empty until that lands.
 *   ALP_INFERENCE_BACKEND_DRPAI    -> reserved for the V2N Yocto path
 *                                     (task #14 sibling).
 *   ALP_INFERENCE_BACKEND_CPU      -> ONNX Runtime (own-recipe upstream
 *                                     pin, see metadata/libraries/onnxruntime.yaml)
 *                                     on the A55s, wired in
 *                                     src/yocto/inference_ort.cpp when
 *                                     ALP_SDK_USE_ORT_CPU is on.  The
 *                                     portable CPU floor -- lowest
 *                                     priority under AUTO, never
 *                                     displaces an NPU backend.
 *
 * This file overrides the inference stubs that src/common/stub_backend.c
 * would otherwise provide (the ALP_VENDOR_OVERRIDES_INFERENCE macro
 * gates them out of the link).  It is compiled unconditionally for the
 * Yocto OS backend so the SDK keeps a single owner of the
 * alp_inference_* symbols on Linux user-space, whether or not any
 * NPU adapter is enabled in this build.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "alp/inference.h"

#include "alp_internal.h"
#include "common/alp_slot_claim.h"
#include "inference_handle_internal.h"

#ifndef ALP_SDK_MAX_INFERENCE_HANDLES
#define ALP_SDK_MAX_INFERENCE_HANDLES 2
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* struct alp_inference is declared in inference_handle_internal.h, shared
 * with the per-backend files (inference_ort.cpp/inference_deepx.cpp/
 * inference_drpai.cpp) -- see that header's doc comment for the
 * in_use/lifecycle/active_ops ordering invariants (issue #1115 round-2
 * dev review, issue #629) and the static_assert pinning be_state's
 * offset (issue #1257). */

static struct alp_inference g_inference_pool[ALP_SDK_MAX_INFERENCE_HANDLES];

/* issue #1115 round-2 dev review: claim atomically instead of the
 * previous plain check-then-set scan. */
static struct alp_inference *pool_acquire(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(g_inference_pool); ++i) {
		if (alp_slot_try_claim(&g_inference_pool[i].in_use)) {
			memset(&g_inference_pool[i], 0, offsetof(struct alp_inference, in_use));
			return &g_inference_pool[i];
		}
	}
	return NULL;
}

static void pool_release(struct alp_inference *h)
{
	if (h != NULL) alp_slot_release(&h->in_use);
}

/* ------------------------------------------------------------------ */
/* Per-backend hook declarations                                       */
/*                                                                     */
/* Each backend's source file (compiled in only when its CMake option */
/* is on) provides these symbols.  Empty when no backend is on -- the */
/* dispatcher returns NOSUPPORT through the default switch arm.       */
/* ------------------------------------------------------------------ */

#if defined(ALP_SDK_USE_DEEPX_DXM1)
alp_status_t alp_inference_deepx_open(struct alp_inference *h, const alp_inference_config_t *cfg);
size_t       alp_inference_deepx_num_inputs(struct alp_inference *h);
size_t       alp_inference_deepx_num_outputs(struct alp_inference *h);
alp_status_t
alp_inference_deepx_get_input(struct alp_inference *h, size_t index, alp_inference_tensor_t *out);
alp_status_t
alp_inference_deepx_get_output(struct alp_inference *h, size_t index, alp_inference_tensor_t *out);
alp_status_t alp_inference_deepx_invoke(struct alp_inference *h);
void         alp_inference_deepx_close(struct alp_inference *h);
#endif

#if defined(ALP_SDK_USE_DRPAI_V2N)
alp_status_t alp_inference_drpai_open(struct alp_inference *h, const alp_inference_config_t *cfg);
size_t       alp_inference_drpai_num_inputs(struct alp_inference *h);
size_t       alp_inference_drpai_num_outputs(struct alp_inference *h);
alp_status_t
alp_inference_drpai_get_input(struct alp_inference *h, size_t index, alp_inference_tensor_t *out);
alp_status_t
alp_inference_drpai_get_output(struct alp_inference *h, size_t index, alp_inference_tensor_t *out);
alp_status_t alp_inference_drpai_invoke(struct alp_inference *h);
void         alp_inference_drpai_close(struct alp_inference *h);
#endif

#if defined(ALP_SDK_USE_ORT_CPU)
alp_status_t alp_inference_ort_open(struct alp_inference *h, const alp_inference_config_t *cfg);
size_t       alp_inference_ort_num_inputs(struct alp_inference *h);
size_t       alp_inference_ort_num_outputs(struct alp_inference *h);
alp_status_t
alp_inference_ort_get_input(struct alp_inference *h, size_t index, alp_inference_tensor_t *out);
alp_status_t
alp_inference_ort_get_output(struct alp_inference *h, size_t index, alp_inference_tensor_t *out);
alp_status_t alp_inference_ort_invoke(struct alp_inference *h);
void         alp_inference_ort_close(struct alp_inference *h);
#endif

/* ------------------------------------------------------------------ */
/* Auto-select policy                                                  */
/*                                                                     */
/* On Yocto the prevailing reason for choosing AUTO is "use the NPU   */
/* on this SoM."  DEEPX_DX wins first on V2N-M1.  Ethos-U / DRP-AI    */
/* slots land alongside task #14.  CPU (ONNX Runtime) is last: an    */
/* NPU-bearing SoM must never silently fall to the CPU floor.         */
/* ------------------------------------------------------------------ */

static alp_inference_backend_t resolve_auto(void)
{
#if defined(ALP_SDK_USE_DEEPX_DXM1)
	/* DEEPX DX-M1 wins first on V2N-M1: the companion NPU is the SoM's
     * reason for shipping. */
	return ALP_INFERENCE_BACKEND_DEEPX_DXM1;
#elif defined(ALP_SDK_USE_DRPAI_V2N)
	/* Plain V2N (no DX-M1): the on-SoC DRP-AI3 is the NPU. */
	return ALP_INFERENCE_BACKEND_DRPAI;
#elif defined(ALP_SDK_USE_ORT_CPU)
	/* No NPU compiled in: the A55s run the model on CPU via ONNX Runtime.
	 * Deliberately LAST -- an NPU-bearing SoM must never silently fall to
	 * CPU under AUTO, because that is a 10-100x throughput cliff the caller
	 * did not ask for. */
	return ALP_INFERENCE_BACKEND_CPU;
#else
	return ALP_INFERENCE_BACKEND_AUTO; /* signals "nothing available" */
#endif
}

/* ------------------------------------------------------------------ */
/* Last-error stamping                                                 */
/*                                                                     */
/* The dispatcher calls alp_internal_set_last_error (declared in       */
/* src/common/alp_internal.h) so failures here are visible through the */
/* public alp_last_error() reader -- the same thread-local slot the    */
/* peripheral stubs in stub_backend.c write to, regardless of vendor   */
/* overrides (that slot has exactly one owner now -- see               */
/* alp_internal.h).  A successful open() clears it so a later caller   */
/* on this thread doesn't see a stale earlier failure.                 */
/* ------------------------------------------------------------------ */

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

alp_inference_t *alp_inference_open(const alp_inference_config_t *cfg)
{
	if (cfg == NULL || cfg->model_data == NULL || cfg->model_size == 0) {
		alp_internal_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}

	alp_inference_backend_t backend = cfg->backend;
	if (backend == ALP_INFERENCE_BACKEND_AUTO) {
		backend = resolve_auto();
		if (backend == ALP_INFERENCE_BACKEND_AUTO) {
			/* Nothing compiled in.  Honour the v0.1 contract: surface
             * is shipped so apps link cleanly; the runtime answer is
             * NOSUPPORT until a backend lands. */
			alp_internal_set_last_error(ALP_ERR_NOSUPPORT);
			return NULL;
		}
	}

	struct alp_inference *h = pool_acquire();
	if (h == NULL) {
		alp_internal_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend = backend;
	/* pool_acquire()'s claim-time memset zeroed this to 0, not the "no
	 * sample yet" sentinel -- set it before any op can run.  Plain
	 * store: lifecycle is still UNOPENED here, so no concurrent op/
	 * reader can observe this handle yet (see the struct's field
	 * comment for why every OTHER access to this field is atomic). */
	h->last_invoke_latency_us = UINT64_MAX;

	alp_status_t rc = ALP_ERR_NOSUPPORT;
	switch (backend) {
#if defined(ALP_SDK_USE_DEEPX_DXM1)
	case ALP_INFERENCE_BACKEND_DEEPX_DXM1:
		rc = alp_inference_deepx_open(h, cfg);
		break;
#endif
#if defined(ALP_SDK_USE_DRPAI_V2N)
	case ALP_INFERENCE_BACKEND_DRPAI:
		rc = alp_inference_drpai_open(h, cfg);
		break;
#endif
#if defined(ALP_SDK_USE_ORT_CPU)
	case ALP_INFERENCE_BACKEND_CPU:
		rc = alp_inference_ort_open(h, cfg);
		break;
#endif
	default:
		rc = ALP_ERR_NOSUPPORT;
		break;
	}

	if (rc != ALP_OK) {
		alp_internal_set_last_error(rc);
		pool_release(h);
		return NULL;
	}
	alp_internal_set_last_error(ALP_OK);
	/* Write lifecycle=OPEN LAST, after backend init has fully
	 * succeeded -- only now may a concurrent op/close see this handle
	 * as live (issue #629). */
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
}

size_t alp_inference_num_inputs(alp_inference_t *inf)
{
	/* Gate on the lifecycle byte via op_enter/leave, not a plain in_use
	 * read -- see the struct's doc comment above (issue #629). */
	if (inf == NULL || !alp_handle_op_enter(&inf->lifecycle, &inf->active_ops)) return 0u;
	size_t n;
	switch (inf->backend) {
#if defined(ALP_SDK_USE_DEEPX_DXM1)
	case ALP_INFERENCE_BACKEND_DEEPX_DXM1:
		n = alp_inference_deepx_num_inputs(inf);
		break;
#endif
#if defined(ALP_SDK_USE_DRPAI_V2N)
	case ALP_INFERENCE_BACKEND_DRPAI:
		n = alp_inference_drpai_num_inputs(inf);
		break;
#endif
#if defined(ALP_SDK_USE_ORT_CPU)
	case ALP_INFERENCE_BACKEND_CPU:
		n = alp_inference_ort_num_inputs(inf);
		break;
#endif
	default:
		n = 0u;
		break;
	}
	alp_handle_op_leave(&inf->active_ops);
	return n;
}

size_t alp_inference_num_outputs(alp_inference_t *inf)
{
	if (inf == NULL || !alp_handle_op_enter(&inf->lifecycle, &inf->active_ops)) return 0u;
	size_t n;
	switch (inf->backend) {
#if defined(ALP_SDK_USE_DEEPX_DXM1)
	case ALP_INFERENCE_BACKEND_DEEPX_DXM1:
		n = alp_inference_deepx_num_outputs(inf);
		break;
#endif
#if defined(ALP_SDK_USE_DRPAI_V2N)
	case ALP_INFERENCE_BACKEND_DRPAI:
		n = alp_inference_drpai_num_outputs(inf);
		break;
#endif
#if defined(ALP_SDK_USE_ORT_CPU)
	case ALP_INFERENCE_BACKEND_CPU:
		n = alp_inference_ort_num_outputs(inf);
		break;
#endif
	default:
		n = 0u;
		break;
	}
	alp_handle_op_leave(&inf->active_ops);
	return n;
}

alp_status_t
alp_inference_get_input(alp_inference_t *inf, size_t index, alp_inference_tensor_t *out)
{
	/* NULL-handle check (and its ALP_ERR_NOT_READY return) stays FIRST,
	 * ahead of the out-NULL check -- matches the pre-existing
	 * tests/yocto/inference_dispatcher.c contract
	 * (test_get_input_null_out_returns_invalid expects NOT_READY when
	 * both inf and out are NULL). */
	if (inf == NULL || !alp_handle_op_enter(&inf->lifecycle, &inf->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	if (out == NULL) {
		alp_handle_op_leave(&inf->active_ops);
		return ALP_ERR_INVAL;
	}
	*out = (alp_inference_tensor_t){ 0 };
#if !defined(ALP_SDK_USE_DEEPX_DXM1) && !defined(ALP_SDK_USE_DRPAI_V2N) && \
    !defined(ALP_SDK_USE_ORT_CPU)
	/* No real inference backend compiled in -- every case below drops out,
	 * leaving `index` unused (issue #634). */
	(void)index;
#endif
	alp_status_t rc;
	switch (inf->backend) {
#if defined(ALP_SDK_USE_DEEPX_DXM1)
	case ALP_INFERENCE_BACKEND_DEEPX_DXM1:
		rc = alp_inference_deepx_get_input(inf, index, out);
		break;
#endif
#if defined(ALP_SDK_USE_DRPAI_V2N)
	case ALP_INFERENCE_BACKEND_DRPAI:
		rc = alp_inference_drpai_get_input(inf, index, out);
		break;
#endif
#if defined(ALP_SDK_USE_ORT_CPU)
	case ALP_INFERENCE_BACKEND_CPU:
		rc = alp_inference_ort_get_input(inf, index, out);
		break;
#endif
	default:
		rc = ALP_ERR_NOSUPPORT;
		break;
	}
	alp_handle_op_leave(&inf->active_ops);
	return rc;
}

alp_status_t
alp_inference_get_output(alp_inference_t *inf, size_t index, alp_inference_tensor_t *out)
{
	if (inf == NULL || !alp_handle_op_enter(&inf->lifecycle, &inf->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	if (out == NULL) {
		alp_handle_op_leave(&inf->active_ops);
		return ALP_ERR_INVAL;
	}
	*out = (alp_inference_tensor_t){ 0 };
#if !defined(ALP_SDK_USE_DEEPX_DXM1) && !defined(ALP_SDK_USE_DRPAI_V2N) && \
    !defined(ALP_SDK_USE_ORT_CPU)
	/* No real inference backend compiled in -- every case below drops out,
	 * leaving `index` unused (issue #634). */
	(void)index;
#endif
	alp_status_t rc;
	switch (inf->backend) {
#if defined(ALP_SDK_USE_DEEPX_DXM1)
	case ALP_INFERENCE_BACKEND_DEEPX_DXM1:
		rc = alp_inference_deepx_get_output(inf, index, out);
		break;
#endif
#if defined(ALP_SDK_USE_DRPAI_V2N)
	case ALP_INFERENCE_BACKEND_DRPAI:
		rc = alp_inference_drpai_get_output(inf, index, out);
		break;
#endif
#if defined(ALP_SDK_USE_ORT_CPU)
	case ALP_INFERENCE_BACKEND_CPU:
		rc = alp_inference_ort_get_output(inf, index, out);
		break;
#endif
	default:
		rc = ALP_ERR_NOSUPPORT;
		break;
	}
	alp_handle_op_leave(&inf->active_ops);
	return rc;
}

/* #629 NOTE: invoke() runs the backend's synchronous model executor
 * (ORT api->Run(), DEEPX engine, DRP-AI runtime) directly on the
 * caller's thread -- bounded by model compute time, not an open-ended
 * external wait -- so it is bracketed with the same op_enter/op_leave
 * as every other op below, exactly like src/inference_dispatch.c's
 * alp_inference_invoke(). alp_inference_close()'s drain
 * (alp_handle_begin_close_blocking()) sleep-polls rather than spins, so
 * a close() racing a large-model invoke() sleeps -- it does not peg the
 * closing thread's core, and it does not race the backend teardown
 * against this op's still-running read of be_state. */
alp_status_t alp_inference_invoke(alp_inference_t *inf)
{
	if (inf == NULL || !alp_handle_op_enter(&inf->lifecycle, &inf->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	/* Bracket the backend's synchronous executor with CLOCK_MONOTONIC
	 * -- this is what makes alp_inference_last_invoke_latency_us()
	 * below work identically for every backend without any of them
	 * opting in: ORT's api->Run(), the DEEPX engine->Run(), and the
	 * DRP-AI runtime.Run() all block this thread until the result
	 * lands, so a dispatcher-level host timestamp IS the backend's
	 * execution time, not an approximation of it. */
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	alp_status_t rc;
	switch (inf->backend) {
#if defined(ALP_SDK_USE_DEEPX_DXM1)
	case ALP_INFERENCE_BACKEND_DEEPX_DXM1:
		rc = alp_inference_deepx_invoke(inf);
		break;
#endif
#if defined(ALP_SDK_USE_DRPAI_V2N)
	case ALP_INFERENCE_BACKEND_DRPAI:
		rc = alp_inference_drpai_invoke(inf);
		break;
#endif
#if defined(ALP_SDK_USE_ORT_CPU)
	case ALP_INFERENCE_BACKEND_CPU:
		rc = alp_inference_ort_invoke(inf);
		break;
#endif
	default:
		rc = ALP_ERR_NOSUPPORT;
		break;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	if (rc == ALP_OK) {
		/* Round-nearest microseconds from a nanosecond delta -- never
		 * floors a genuinely nonzero duration to a truncation-biased 0,
		 * and stays in int64_t/uint64_t throughout so neither a very
		 * fast invoke (sub-us) nor a very slow one (32-bit us overflow)
		 * corrupts the stored value. */
		int64_t delta_ns = ((int64_t)t1.tv_sec - (int64_t)t0.tv_sec) * 1000000000LL +
		                   ((int64_t)t1.tv_nsec - (int64_t)t0.tv_nsec);
		if (delta_ns < 0) delta_ns = 0; /* CLOCK_MONOTONIC never regresses; guard anyway */
		uint64_t us = (uint64_t)((delta_ns + 500) / 1000);
		/* Concurrent invokes on one handle are a real interleaving
		 * (active_ops is a drain counter, not a mutex -- see the
		 * struct's field comment), so this write races a concurrent
		 * alp_inference_last_invoke_latency_us() reader (or another
		 * invoke()) on a genuinely multi-threaded caller: atomic
		 * store, not a plain assignment. */
		__atomic_store_n(&inf->last_invoke_latency_us, us, __ATOMIC_RELEASE);
	}
	alp_handle_op_leave(&inf->active_ops);
	return rc;
}

alp_status_t alp_inference_last_invoke_latency_us(alp_inference_t *inf, uint64_t *out_us)
{
	/* out_us-NULL check stays FIRST, ahead of the handle check.
	 *
	 * This used to check inf first, matching this file's
	 * get_input()/get_output() convention above -- but that convention
	 * is itself pinned by an existing test
	 * (tests/yocto/inference_dispatcher.c's
	 * test_get_input_null_out_returns_invalid expects NOT_READY for
	 * (NULL, NULL)), which this brand-new accessor has no equivalent
	 * of. Checking inf first meant (NULL, NULL) and (closed, NULL) both
	 * returned ALP_ERR_NOT_READY here, while the Zephyr dispatcher
	 * (src/inference_dispatch.c) -- checking out_us first, like every
	 * OTHER op in that file -- returned ALP_ERR_INVAL for the same two
	 * calls: the same public function disagreeing with itself across
	 * OSes (MAJOR 2). <alp/inference.h> lists ALP_ERR_INVAL before
	 * ALP_ERR_NOT_READY with no handle-state qualifier, so out_us-first
	 * is the order the header actually describes -- this function moves
	 * to match Zephyr and the header rather than this file's other two
	 * ops. */
	if (out_us == NULL) return ALP_ERR_INVAL;
	if (inf == NULL || !alp_handle_op_enter(&inf->lifecycle, &inf->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	uint64_t us = __atomic_load_n(&inf->last_invoke_latency_us, __ATOMIC_ACQUIRE);
	alp_handle_op_leave(&inf->active_ops);
	if (us == UINT64_MAX) return ALP_ERR_NOT_READY; /* no successful invoke yet */
	*out_us = us;
	return ALP_OK;
}

void alp_inference_close(alp_inference_t *inf)
{
	if (inf == NULL) return;
	/* begin_close_blocking() CASes OPEN->CLOSING (false = already
	 * closing/closed/never-opened -- matches every other void-close
	 * idempotency contract) then sleep-polls until every op that
	 * entered before the CAS has left (issue #629): this is what stops
	 * a concurrent alp_inference_invoke() (blocked in the backend's
	 * synchronous executor) from reading/writing a be_state this
	 * function is about to delete underneath it. */
	if (!alp_handle_begin_close_blocking(&inf->lifecycle, &inf->active_ops)) return;
	switch (inf->backend) {
#if defined(ALP_SDK_USE_DEEPX_DXM1)
	case ALP_INFERENCE_BACKEND_DEEPX_DXM1:
		alp_inference_deepx_close(inf);
		break;
#endif
#if defined(ALP_SDK_USE_DRPAI_V2N)
	case ALP_INFERENCE_BACKEND_DRPAI:
		alp_inference_drpai_close(inf);
		break;
#endif
#if defined(ALP_SDK_USE_ORT_CPU)
	case ALP_INFERENCE_BACKEND_CPU:
		alp_inference_ort_close(inf);
		break;
#endif
	default:
		break;
	}
	alp_lifecycle_set(&inf->lifecycle, ALP_HANDLE_LC_UNOPENED);
	pool_release(inf);
}
