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

/* #629 NOTE (flagged for orchestrator review): invoke() runs the
 * backend's synchronous model executor (TFLM interpreter, optionally
 * with the Ethos-U op resolver) directly on the caller's thread --
 * bounded by model compute time (never an open-ended external wait
 * the way alp_rpc_call()'s timeout_ms can be), so it does not meet
 * this ticket's "blocks unboundedly" bar for the rpc-style sleep-
 * drain treatment and is bracketed like every other op below.
 * However, its duration is NOT "a handful of instructions" the way
 * alp_handle_begin_close()'s busy-spin assumes (src/common/
 * alp_slot_claim.h) -- a close() racing a large-model invoke() would
 * busy-spin the closing thread for the full inference latency.
 * Flagged rather than silently accepted; a future pass may want
 * invoke() on the rpc_dispatch.c sleep-poll drain instead of the
 * generic spin if that latency proves material. */
alp_status_t alp_inference_invoke(alp_inference_t *inf)
{
	if (inf == NULL || !alp_handle_op_enter(&inf->lifecycle, &inf->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc = (inf->state.ops == NULL || inf->state.ops->invoke == NULL)
	                      ? ALP_ERR_NOT_IMPLEMENTED
	                      : inf->state.ops->invoke(&inf->state);
	alp_handle_op_leave(&inf->active_ops);
	return rc;
}

void alp_inference_close(alp_inference_t *inf)
{
	if (inf == NULL) return;
	/* begin_close CAS OPEN->CLOSING then spins until every op that
	 * entered before the CAS has left (issue #629); see the invoke()
	 * flag above re: worst-case spin duration. Idempotent: a second/
	 * never-opened close no-ops. */
	if (!alp_handle_begin_close(&inf->lifecycle, &inf->active_ops)) return;
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
