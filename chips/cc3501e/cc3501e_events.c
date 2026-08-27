/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * CC3501E async-event callback registration + polling, and deinit.  See
 * <alp/chips/cc3501e/events.h> for the public API.  The request/reply
 * framing primitive (cc3501e_request) these build on lives in
 * cc3501e_core.c.
 */

#include <stdint.h>

#include "cc3501e_internal.h"

alp_status_t cc3501e_add_event_callback(cc3501e_t *ctx, cc3501e_event_cb_t cb, void *user)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (cb == NULL) return ALP_ERR_INVAL;

	/* Idempotent: re-registering the SAME (cb, user) pair is a no-op rather
	 * than a second slot, so a caller that re-arms defensively (e.g. on
	 * reconnect) cannot end up receiving every event twice. */
	for (size_t i = 0; i < CC3501E_EVENT_SUBSCRIBERS; i++) {
		if (ctx->event_subs[i].cb == cb && ctx->event_subs[i].user == user) {
			return ALP_OK;
		}
	}
	for (size_t i = 0; i < CC3501E_EVENT_SUBSCRIBERS; i++) {
		if (ctx->event_subs[i].cb == NULL) {
			ctx->event_subs[i].user = user;
			ctx->event_subs[i].cb   = cb;
			return ALP_OK;
		}
	}
	return ALP_ERR_NOMEM;
}

alp_status_t cc3501e_remove_event_callback(cc3501e_t *ctx, cc3501e_event_cb_t cb, void *user)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (cb == NULL) return ALP_ERR_INVAL;

	for (size_t i = 0; i < CC3501E_EVENT_SUBSCRIBERS; i++) {
		if (ctx->event_subs[i].cb == cb && ctx->event_subs[i].user == user) {
			ctx->event_subs[i].cb   = NULL;
			ctx->event_subs[i].user = NULL;
			return ALP_OK;
		}
	}
	return ALP_ERR_NOT_FOUND;
}

alp_status_t cc3501e_poll_events(cc3501e_t *ctx)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	/* No sink registered -> don't drain: leave the events queued in the firmware
	 * ring so they aren't lost before a callback is attached (the firmware drains
	 * on every GET_PENDING_EVENTS, so a poll with no cb would silently discard). */
	bool have_sink = false;
	for (size_t i = 0; i < CC3501E_EVENT_SUBSCRIBERS; i++) {
		if (ctx->event_subs[i].cb != NULL) {
			have_sink = true;
			break;
		}
	}
	if (!have_sink) return ALP_OK;

	/* Explicit reentrancy guard (issue #740): the callback below is handed
	 * pointers INTO ctx->evt_buf, valid only for the duration of that one
	 * call (the callback must copy anything it needs to keep).  If the
	 * callback calls cc3501e_poll_events() again on THIS ctx before we
	 * return, a second drain would overwrite evt_buf out from under the walk
	 * still in progress here.  Reject the reentrant call instead of
	 * aliasing; the caller's next scheduled poll picks up whatever landed on
	 * the ring meanwhile.
	 *
	 * NOTE this is a plain test-then-set bool, not an atomic/CAS -- it
	 * reliably catches same-call-stack reentrancy (this callback, or an ISR
	 * that runs to completion before its interrupted thread resumes) but
	 * does NOT by itself serialize two truly concurrent callers on the SAME
	 * ctx from separate threads/cores (see <alp/chips/cc3501e/events.h>). */
	if (ctx->evt_busy) return ALP_ERR_BUSY;
	ctx->evt_busy = true;

	/* Drain the firmware event ring (CMD_GET_PENDING_EVENTS, opcode 0x05).  The
	 * reply DATA is a packed list of { evt_opcode(1) | len(1) | payload[len] }
	 * entries; walk them and hand each to the registered callback.  A single
	 * request (fast, non-blocking firmware handler); a transient bridge-down IO
	 * simply surfaces to the caller, which retries on its next poll cycle.
	 * evt_buf is per-context (cc3501e_t) storage, not a function-local
	 * `static` -- see cc3501e_t's evt_buf comment. */
	uint8_t     *evt_buf = ctx->evt_buf;
	size_t       got     = 0;
	alp_status_t s       = cc3501e_request(ctx,
	                                       ALP_CC3501E_CMD_GET_PENDING_EVENTS,
	                                       NULL,
	                                       0,
	                                       evt_buf,
	                                       sizeof(ctx->evt_buf),
	                                       &got,
	                                       CC3501E_REQ_TMO_MS);
	if (s != ALP_OK) {
		ctx->evt_busy = false;
		return s;
	}

	size_t off = 0;
	while (off + ALP_CC3501E_EVENT_HDR_BYTES <= got) {
		const uint8_t opcode = evt_buf[off];
		const uint8_t len    = evt_buf[off + 1u];
		if (off + ALP_CC3501E_EVENT_HDR_BYTES + (size_t)len > got) {
			break; /* truncated trailing entry -- stop cleanly */
		}
		/* NOTE: the callback payload pointer is only valid for this call --
		 * it points into ctx->evt_buf, which the NEXT poll (this ctx) will
		 * overwrite.  A callback that needs the bytes afterward must copy
		 * them before returning. */
		/* Fan out to every subscriber (issue #1723).  Snapshot each slot
		 * before invoking it: a callback is allowed to unregister itself,
		 * which would otherwise clear the slot mid-dispatch. */
		for (size_t i = 0; i < CC3501E_EVENT_SUBSCRIBERS; i++) {
			const cc3501e_event_cb_t cb   = ctx->event_subs[i].cb;
			void *const              user = ctx->event_subs[i].user;
			if (cb != NULL) {
				cb(opcode, &evt_buf[off + ALP_CC3501E_EVENT_HDR_BYTES], len, user);
			}
		}
		off += ALP_CC3501E_EVENT_HDR_BYTES + (size_t)len;
	}
	ctx->evt_busy = false;
	return ALP_OK;
}

void cc3501e_deinit(cc3501e_t *ctx)
{
	if (ctx == NULL) return;
	ctx->initialised = false;
	ctx->bus         = NULL;
}
