/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RPC class dispatcher.  Owns the public <alp/rpc.h> surface
 * (framed RPC over OpenAMP / RPMsg) on top of the backend registry
 * mechanism shipped in Slice 0 (PR #17).
 *
 * Per design spec Section 4: ONE class registry covers the single
 * alp_rpc_channel_t surface.  The dispatcher copies the customer's
 * alp_rpc_config_t into the handle's backend state before
 * delegating to the backend's open(); every subsequent op walks
 * state->ops.  Subscription tables and the per-channel sync-call
 * slot live entirely behind state->be_data inside the backend.
 *
 * Slice 4c ships no vendor extensions for RPC: the Zephyr
 * ipc_service-backed backend covers every E1M-conformant SoM with
 * an OpenAMP / RPMsg-capable Zephyr build; the SW fallback covers
 * native_sim and trimmed images.  No second registry tier is
 * needed.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/rpc.h>
#include <alp/soc_caps.h>

#include "alp_slot_claim.h"
#include "backends/rpc/rpc_ops.h"

ALP_BACKEND_DEFINE_CLASS(rpc);
/* Pull the rpc registry section into a static-archive link (#368). */
ALP_BACKEND_ANCHOR(rpc);

#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_MAX_RPC_HANDLES
#define CONFIG_ALP_SDK_MAX_RPC_HANDLES 2
#endif

static struct alp_rpc_channel _rpc_pool[CONFIG_ALP_SDK_MAX_RPC_HANDLES];

static struct alp_rpc_channel *_alloc_rpc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_RPC_HANDLES; ++i) {
		/* Atomic claim (GHSA-xhm8-7f87-93q5 defect 2 / issue #629
         * pattern): CAS FREE -> OPEN on `in_use` so two concurrent
         * alp_rpc_open() calls can never both win the same slot.  Only
         * the winner may touch the slot's other fields -- in_use is
         * the struct's last member, so zero everything before it. */
		if (alp_slot_try_claim(&_rpc_pool[i].in_use)) {
			memset(&_rpc_pool[i], 0, offsetof(struct alp_rpc_channel, in_use));
			return &_rpc_pool[i];
		}
	}
	return NULL;
}

static void _free_rpc(struct alp_rpc_channel *h)
{
	alp_slot_release(&h->in_use);
}

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

alp_rpc_channel_t *alp_rpc_open(const alp_rpc_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL || cfg->name == NULL || cfg->name[0] == '\0') {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("rpc", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_rpc_ops_t *ops = (const alp_rpc_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_rpc_channel *h = _alloc_rpc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	h->state.cfg            = *cfg;
	alp_capabilities_t caps = { .flags = be->base_caps };
	alp_status_t       rc   = ops->open(cfg, &h->state, &caps);
	if (rc != ALP_OK) {
		_free_rpc(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	/* Only now, after a successful backend open(), does this handle
     * become visible to alp_rpc_close()/subscribe/send/call's
     * alp_handle_op_enter() gate -- see struct alp_rpc_channel's doc
     * comment. */
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
}

void alp_rpc_close(alp_rpc_channel_t *ch)
{
	if (ch == NULL) return;

	/* CAS OPEN -> CLOSING (GHSA-xhm8-7f87-93q5 defect 2 / issue #629's
     * state-machine pattern, but NOT via alp_handle_begin_close(): that
     * helper drains active_ops to zero BEFORE running the caller's
     * close body, which assumes every gated op is short (see its own
     * doc comment in src/common/alp_slot_claim.h) -- alp_rpc_call() is
     * explicitly NOT short (it can block up to `timeout_ms`, including
     * UINT32_MAX == forever), and the thing that unblocks a pending
     * call is the BACKEND's own close (y_close() in
     * src/backends/rpc/yocto_drv.c cancels + broadcasts any pending
     * call slot).  Draining first would deadlock: this function would
     * spin forever waiting for a call that can only be unblocked by the
     * ops->close() call this function hasn't reached yet.
     *
     * So the ordering here is deliberately CAS -> ops->close() ->
     * THEN drain -- ops->close() unblocks whatever is in flight FIRST,
     * so the post-close drain below is short by the time it runs.
     *
     * The CAS itself still gives single-shot close-vs-close: the loser
     * (a concurrent EXTERNAL alp_rpc_close() racing this channel's own
     * self-close callback -- the scenario this advisory is about, or
     * any other racing closer) sees `lifecycle` already CLOSING (or
     * UNOPENED) and returns immediately without ever dereferencing
     * `state.ops`.  And because the slot stays CLOSING (never
     * UNOPENED, never released back to `_alloc_rpc()`) for the *entire*
     * span from this CAS through the drain below, no concurrent
     * alp_rpc_open() can recycle this array slot -- and repurpose the
     * very `state.ops`/`state.be_data` an in-flight subscribe/send/call
     * is still dereferencing -- until every such op has left.  That is
     * the recycle-hijack TOCTOU this advisory's followup review flagged
     * in the plain `!ch->in_use` version of this function.
     *
     * Residual, NOT defended: `alp_rpc_channel_t` is a raw pointer with
     * no generation the caller carries across calls.  A caller that
     * calls alp_rpc_close() a SECOND time on a handle that has already
     * completed a full close (and whose slot has since been recycled
     * by an unrelated alp_rpc_open()) will legitimately win this same
     * CAS against the NEW occupant and tear IT down -- indistinguishable,
     * from the dispatcher's point of view, from a correct close of a
     * live handle. That is a use-after-close by the caller (the same
     * class of bug as any other double-free/use-after-free of a raw
     * handle) and is out of scope for a fix that keeps
     * `alp_rpc_channel_t` an opaque pointer per include/alp/rpc.h's
     * [ABI-STABLE] contract; see alp_rpc_close()'s doc comment there.
     * What IS fully defended -- the contract include/alp/rpc.h actually
     * makes -- is exactly the concurrent-close-of-a-still-live-handle
     * case above: two racing closers of the SAME open channel are
     * single-shot and UAF-free, whichever one is the CAS winner. */
	if (!alp_lifecycle_cas(&ch->lifecycle, ALP_HANDLE_LC_OPEN, ALP_HANDLE_LC_CLOSING)) return;

	if (ch->state.ops != NULL && ch->state.ops->close != NULL) {
		ch->state.ops->close(&ch->state);
	}

	/* Drain whatever was already inside alp_rpc_subscribe/unsubscribe/
     * send/call when the CAS above flipped to CLOSING -- ops->close()
     * just unblocked anything the backend could unblock, so this is
     * expected to clear in a handful of iterations, not a real wait. */
	while (__atomic_load_n(&ch->active_ops, __ATOMIC_ACQUIRE) != 0u) {
		/* Bounded spin -- see the ordering note above. */
	}

	alp_lifecycle_set(&ch->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free_rpc(ch);
}

/* ================================================================== */
/* Subscriptions                                                       */
/* ================================================================== */

alp_status_t
alp_rpc_subscribe(alp_rpc_channel_t *ch, const char *method, alp_rpc_method_cb_t cb, void *user)
{
	/* Gate on `lifecycle` (acquire-loaded inside alp_handle_op_enter()),
     * not a plain `in_use` bool read -- see struct alp_rpc_channel's
     * doc comment (GHSA-xhm8-7f87-93q5 defect 2). */
	if (ch == NULL || !alp_handle_op_enter(&ch->lifecycle, &ch->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (method == NULL || method[0] == '\0') {
		rc = ALP_ERR_INVAL;
	} else if (ch->state.ops == NULL || ch->state.ops->subscribe == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = ch->state.ops->subscribe(&ch->state, method, cb, user);
	}
	alp_handle_op_leave(&ch->active_ops);
	return rc;
}

alp_status_t alp_rpc_unsubscribe(alp_rpc_channel_t *ch, const char *method)
{
	if (ch == NULL || !alp_handle_op_enter(&ch->lifecycle, &ch->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (method == NULL || method[0] == '\0') {
		rc = ALP_ERR_INVAL;
	} else if (ch->state.ops == NULL || ch->state.ops->unsubscribe == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = ch->state.ops->unsubscribe(&ch->state, method);
	}
	alp_handle_op_leave(&ch->active_ops);
	return rc;
}

/* ================================================================== */
/* Send + call                                                         */
/* ================================================================== */

alp_status_t
alp_rpc_send(alp_rpc_channel_t *ch, const char *method, const void *payload, size_t len)
{
	if (ch == NULL || !alp_handle_op_enter(&ch->lifecycle, &ch->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (method == NULL || method[0] == '\0') {
		rc = ALP_ERR_INVAL;
	} else if (payload == NULL && len > 0) {
		rc = ALP_ERR_INVAL;
	} else if (ch->state.ops == NULL || ch->state.ops->send == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = ch->state.ops->send(&ch->state, method, payload, len);
	}
	alp_handle_op_leave(&ch->active_ops);
	return rc;
}

alp_status_t alp_rpc_call(alp_rpc_channel_t *ch,
                          const char        *method,
                          const void        *req,
                          size_t             req_len,
                          void              *resp,
                          size_t            *resp_len,
                          uint32_t           timeout_ms)
{
	if (ch == NULL || !alp_handle_op_enter(&ch->lifecycle, &ch->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (method == NULL || method[0] == '\0') {
		rc = ALP_ERR_INVAL;
	} else if (req == NULL && req_len > 0) {
		rc = ALP_ERR_INVAL;
	} else if (resp != NULL && resp_len == NULL) {
		rc = ALP_ERR_INVAL;
	} else if (ch->state.ops == NULL || ch->state.ops->call == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = ch->state.ops->call(&ch->state, method, req, req_len, resp, resp_len, timeout_ms);
	}
	alp_handle_op_leave(&ch->active_ops);
	return rc;
}

/* ================================================================== */
/* Capability getter                                                   */
/* ================================================================== */

const alp_capabilities_t *alp_rpc_capabilities(const alp_rpc_channel_t *ch)
{
	return (ch != NULL) ? &ch->cached_caps : NULL;
}
