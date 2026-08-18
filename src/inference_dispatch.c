/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Inference class dispatcher.  Owns the public alp_inference_*
 * API surface and routes through the backend registry mechanism
 * shipped in Slice 0 (PR #17).
 *
 * The handle struct layout (struct alp_inference) lives in
 * src/backends/inference/inference_ops.h so per-backend .c / .cpp
 * files can reach the fields without duplicating the layout.
 *
 * Backend-pin semantics
 *   - ALP_INFERENCE_BACKEND_AUTO -> alp_backend_select() walks the
 *                                   registry and picks by priority.
 *   - Any non-AUTO selector       -> the dispatcher first calls
 *                                   alp_backend_select() to find the
 *                                   silicon-bound choice, then asks
 *                                   the backend's `open()` to honour
 *                                   the caller's preference; the open
 *                                   hook may return NOSUPPORT if the
 *                                   pinned variant doesn't match what
 *                                   that backend can serve (e.g. CPU-
 *                                   pinned model on an Ethos-U-only
 *                                   build).
 *
 * The customer-visible enum (CPU / ETHOS_U / DRPAI / DEEPX_DX) in
 * <alp/inference.h> is forwarded through the dispatcher into
 * state.backend_id so the picked backend sees the original intent.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* This dispatcher is Zephyr-registry-only -- zephyr/CMakeLists.txt is
 * the sole place that compiles src/inference_dispatch.c (the Yocto
 * side owns its own dispatcher, src/yocto/inference_yocto.c, with its
 * own POSIX clock_gettime() timing below), so pulling in
 * <zephyr/kernel.h> unconditionally here is safe -- unlike
 * src/rpc_dispatch.c, which compiles into both OSes and gates the
 * same need behind __ZEPHYR__. k_cycle_get_64()/k_cycle_get_32()
 * bracket ops->invoke() for alp_inference_last_invoke_latency_us()
 * below -- see alp_inference_invoke()'s comment for which one runs on
 * a given target and why. */
#include <zephyr/kernel.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/inference.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "alp_slot_claim.h"
#include "backends/inference/inference_ops.h"

ALP_BACKEND_DEFINE_CLASS(inference);

/* Reuse the existing TLS-backed last-error mechanism from
 * src/zephyr/last_error.c.  Forward-declared here to avoid
 * pulling in the broader handles.h header. */
#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_MAX_INFERENCE_HANDLES
#define CONFIG_ALP_SDK_MAX_INFERENCE_HANDLES 2
#endif

static struct alp_inference _pool[CONFIG_ALP_SDK_MAX_INFERENCE_HANDLES];

static struct alp_inference *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_INFERENCE_HANDLES; ++i) {
		/* Atomic claim: only the winner of the flag flip may touch
		 * the slot's other fields (in_use is the struct's last
		 * member, so zero everything before it -- including
		 * lifecycle/active_ops, parking a fresh slot at UNOPENED). */
		if (alp_slot_try_claim(&_pool[i].in_use)) {
			memset(&_pool[i], 0, offsetof(struct alp_inference, in_use));
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_inference *h)
{
	alp_slot_release(&h->in_use);
}

alp_inference_t *alp_inference_open(const alp_inference_config_t *cfg)
{
	alp_z_clear_last_error();

	if (cfg == NULL || cfg->model_data == NULL || cfg->model_size == 0u) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}

	const alp_backend_t *be = alp_backend_select("inference", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_inference_ops_t *ops = (const alp_inference_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_inference *h = _alloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	h->state.backend_id     = cfg->backend;
	alp_capabilities_t caps = { .flags = be->base_caps };
	alp_status_t       rc   = ops->open(cfg, &h->state, &caps);
	if (rc != ALP_OK) {
		_free(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	/* _alloc()'s claim-time memset zeroed this to 0, not the "no
	 * sample yet" sentinel -- set it explicitly before any op can run.
	 * Plain store: lifecycle is still UNOPENED here, so no concurrent
	 * op/reader can observe this handle yet (see the struct's field
	 * comment for why every OTHER access to this field is atomic).
	 * UINT32_MAX, not UINT64_MAX -- the field is a uint32_t (see the
	 * struct's field comment: no libatomic on Cortex-M). */
	h->last_invoke_latency_us = UINT32_MAX;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
}

size_t alp_inference_num_inputs(alp_inference_t *inf)
{
	/* Gate on the lifecycle byte, not a plain in_use read -- see
	 * alp_slot_claim.h's op_enter/leave doc comment (issue #629). */
	if (inf == NULL || !alp_handle_op_enter(&inf->lifecycle, &inf->active_ops)) return 0u;
	size_t n = (inf->state.ops == NULL || inf->state.ops->num_inputs == NULL)
	               ? 0u
	               : inf->state.ops->num_inputs(&inf->state);
	alp_handle_op_leave(&inf->active_ops);
	return n;
}

size_t alp_inference_num_outputs(alp_inference_t *inf)
{
	if (inf == NULL || !alp_handle_op_enter(&inf->lifecycle, &inf->active_ops)) return 0u;
	size_t n = (inf->state.ops == NULL || inf->state.ops->num_outputs == NULL)
	               ? 0u
	               : inf->state.ops->num_outputs(&inf->state);
	alp_handle_op_leave(&inf->active_ops);
	return n;
}

alp_status_t
alp_inference_get_input(alp_inference_t *inf, size_t index, alp_inference_tensor_t *out)
{
	if (out == NULL) return ALP_ERR_INVAL; /* param check before enter */
	*out = (alp_inference_tensor_t){ 0 };
	if (inf == NULL || !alp_handle_op_enter(&inf->lifecycle, &inf->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc = (inf->state.ops == NULL || inf->state.ops->get_input == NULL)
	                      ? ALP_ERR_NOT_IMPLEMENTED
	                      : inf->state.ops->get_input(&inf->state, index, out);
	alp_handle_op_leave(&inf->active_ops);
	return rc;
}

alp_status_t
alp_inference_get_output(alp_inference_t *inf, size_t index, alp_inference_tensor_t *out)
{
	if (out == NULL) return ALP_ERR_INVAL; /* param check before enter */
	*out = (alp_inference_tensor_t){ 0 };
	if (inf == NULL || !alp_handle_op_enter(&inf->lifecycle, &inf->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc = (inf->state.ops == NULL || inf->state.ops->get_output == NULL)
	                      ? ALP_ERR_NOT_IMPLEMENTED
	                      : inf->state.ops->get_output(&inf->state, index, out);
	alp_handle_op_leave(&inf->active_ops);
	return rc;
}

/* #629 NOTE: invoke() runs the backend's synchronous model executor
 * (TFLM interpreter, optionally with the Ethos-U op resolver) directly
 * on the caller's thread -- bounded by model compute time (never an
 * open-ended external wait the way alp_rpc_call()'s timeout_ms can
 * be), so it does not meet this ticket's "blocks unboundedly" bar for
 * the rpc-style sleep-drain treatment and is bracketed like every
 * other op below. Its duration is NOT "a handful of instructions"
 * the way a close() racing invoke() might once have implied a long
 * spin; that concern is now moot regardless of duration --
 * alp_handle_begin_close_blocking() (src/common/alp_slot_claim.c)
 * sleep-polls rather than spins (issue #1114), so a close() racing a
 * large-model invoke() sleeps, it does not peg the closing thread's
 * core. */
alp_status_t alp_inference_invoke(alp_inference_t *inf)
{
	if (inf == NULL || !alp_handle_op_enter(&inf->lifecycle, &inf->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	/* Bracket the backend's synchronous executor with the cycle counter
	 * -- this is what makes alp_inference_last_invoke_latency_us()
	 * below work identically for every backend without any of them
	 * opting in: every shipped invoke() (TFLM/Ethos-U interpreter,
	 * DRP-AI Run(), DEEPX engine->Run()) blocks this thread until the
	 * result lands, so a dispatcher-level host timestamp IS the
	 * backend's execution time, not an approximation of it.
	 *
	 * 64-bit vs 32-bit cycle capture: a plain k_cycle_get_32() wraps
	 * modulo 2^32 cycles -- hypothetically, at the AEN801 M55's
	 * CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC=400000000, that would be
	 * ~10.74 s, so an invoke longer than that would report a wrapped,
	 * too-small duration (the "subtracting two uint32_t counts is
	 * correct modulo 2^32" property this used to rely on is only true
	 * across a SINGLE wrap, and doesn't help if the counter wraps
	 * because the delta itself is >= 2^32 cycles). That 400 MHz + plain
	 * 32-bit-counter combination never actually ships, though: Zephyr's
	 * CORTEX_M_SYSTICK_64BIT_CYCLE_COUNTER Kconfig (which
	 * CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER depends on for a plain
	 * Cortex-M SysTick target) defaults to `y` once
	 * CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC exceeds 60 MHz, so the AEN801
	 * M55 (400 MHz) selects k_cycle_get_64() below under stock Kconfig,
	 * same as every other target with an ARM architected/GIC timer
	 * (which selects it unconditionally). The k_cycle_get_32() fallback
	 * only actually compiles in on a target at or below 60 MHz left at
	 * Kconfig defaults, where the wrap ceiling is correspondingly >=
	 * ~71.58 s, not ~10.74 s (see the header's
	 * alp_inference_last_invoke_latency_us() doc for the exact numbers
	 * and for the more dangerous part of this: an unnoticed wrap reports
	 * ALP_OK with a plausible-looking too-small value, not an error).
	 * Either way k_cyc_to_us_near64() does the cycles->us
	 * conversion at 64-bit precision with round-nearest (not the
	 * floor-then-truncate-to-32-bit an app-level k_cycle_get_32()
	 * harness would do), so a very fast invoke rounds sub-us to 0
	 * rather than truncating with a bias. */
#if defined(CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER)
	uint64_t t0 = k_cycle_get_64();
#else
	uint32_t t0 = k_cycle_get_32();
#endif
	alp_status_t rc = (inf->state.ops == NULL || inf->state.ops->invoke == NULL)
	                      ? ALP_ERR_NOT_IMPLEMENTED
	                      : inf->state.ops->invoke(&inf->state);
#if defined(CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER)
	uint64_t t1 = k_cycle_get_64();
#else
	uint32_t t1 = k_cycle_get_32();
#endif
	if (rc == ALP_OK) {
		uint64_t delta_cycles = (uint64_t)(t1 - t0); /* correct across a single wrap */
		uint64_t us64         = k_cyc_to_us_near64(delta_cycles);
		/* last_invoke_latency_us is uint32_t (no libatomic on
		 * Cortex-M -- see the struct's field comment), so saturate
		 * rather than silently truncate/wrap a duration that
		 * exceeds the field's ~71.58-minute ceiling. Clamp the
		 * CEILING to UINT32_MAX - 1, not UINT32_MAX: UINT32_MAX
		 * itself is reserved as the "no successful invoke yet"
		 * sentinel below alp_inference_open() sets and
		 * alp_inference_last_invoke_latency_us() checks for, so a
		 * duration that actually hits the raw UINT32_MAX ceiling
		 * must be pushed one below it -- otherwise it would collide
		 * with the sentinel and this call's readers would see
		 * ALP_ERR_NOT_READY (and never get *out_us at all) instead
		 * of the documented saturated value. */
		uint32_t us = (us64 >= UINT32_MAX) ? (UINT32_MAX - 1u) : (uint32_t)us64;
		/* Concurrent invokes on one handle are a real interleaving
		 * (active_ops is a drain counter, not a mutex -- see the
		 * struct's field comment), so this write races a concurrent
		 * alp_inference_last_invoke_latency_us() reader (or another
		 * invoke()) on a genuinely multi-threaded caller: atomic
		 * store, not a plain assignment. A naturally-aligned
		 * uint32_t store is lock-free on every Cortex-M this SDK
		 * targets -- no libatomic call, unlike the uint64_t this
		 * field used to be. */
		__atomic_store_n(&inf->last_invoke_latency_us, us, __ATOMIC_RELEASE);
	}
	alp_handle_op_leave(&inf->active_ops);
	return rc;
}

alp_status_t alp_inference_last_invoke_latency_us(alp_inference_t *inf, uint64_t *out_us)
{
	/* out_us-NULL check stays FIRST, ahead of the handle check -- same
	 * "param check before enter" convention as get_input()/get_output()
	 * above (a pure param check with no handle-state deref may run
	 * before op_enter; see alp-lab:writing-race-safe-dispatch-handlers
	 * rule 2) and the order <alp/inference.h> documents (ALP_ERR_INVAL
	 * listed before ALP_ERR_NOT_READY). MAJOR 2: the Yocto dispatcher
	 * (src/yocto/inference_yocto.c) used to check the handle first,
	 * so (NULL, NULL) and (closed, NULL) returned ALP_ERR_NOT_READY
	 * there instead of ALP_ERR_INVAL -- fixed to match this ordering,
	 * not the other way, since this is the ordering every other op in
	 * this file already uses. */
	if (out_us == NULL) return ALP_ERR_INVAL; /* param check before enter */
	if (inf == NULL || !alp_handle_op_enter(&inf->lifecycle, &inf->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	/* last_invoke_latency_us is uint32_t (no libatomic on Cortex-M --
	 * see the struct's field comment); widen to the public uint64_t
	 * out_us after the sentinel check below. */
	uint32_t us = __atomic_load_n(&inf->last_invoke_latency_us, __ATOMIC_ACQUIRE);
	alp_handle_op_leave(&inf->active_ops);
	if (us == UINT32_MAX) return ALP_ERR_NOT_READY; /* no successful invoke yet */
	*out_us = (uint64_t)us;
	return ALP_OK;
}

void alp_inference_close(alp_inference_t *inf)
{
	if (inf == NULL) return;
	/* begin_close CAS OPEN->CLOSING then drains (sleep-poll, issue
	 * #1114) until every op that entered before the CAS has left
	 * (issue #629). Idempotent: a second/never-opened close no-ops. */
	if (!alp_handle_begin_close_blocking(&inf->lifecycle, &inf->active_ops)) return;
	if (inf->state.ops != NULL && inf->state.ops->close != NULL) {
		inf->state.ops->close(&inf->state);
	}
	alp_lifecycle_set(&inf->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free(inf);
}

const alp_capabilities_t *alp_inference_capabilities(const alp_inference_t *inf)
{
	/* No in_use read here: in_use is now claimed/released atomically in
	 * _alloc/_free, so mixing it with a plain read is the exact atomic-vs-
	 * plain data race #629 forbids. Match every other class's cap getter --
	 * return the cached pointer; the caller owns not racing its own close. */
	return (inf != NULL) ? &inf->cached_caps : NULL;
}
