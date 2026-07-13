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

alp_status_t cc3501e_set_event_callback(cc3501e_t *ctx, cc3501e_event_cb_t cb, void *user)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	ctx->event_cb   = cb;
	ctx->event_user = user;
	return ALP_OK;
}

alp_status_t cc3501e_poll_events(cc3501e_t *ctx)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	/* No sink registered -> don't drain: leave the events queued in the firmware
	 * ring so they aren't lost before a callback is attached (the firmware drains
	 * on every GET_PENDING_EVENTS, so a poll with no cb would silently discard). */
	if (ctx->event_cb == NULL) return ALP_OK;

	/* Drain the firmware event ring (CMD_GET_PENDING_EVENTS, opcode 0x05).  The
	 * reply DATA is a packed list of { evt_opcode(1) | len(1) | payload[len] }
	 * entries; walk them and hand each to the registered callback.  A single
	 * request (fast, non-blocking firmware handler); a transient bridge-down IO
	 * simply surfaces to the caller, which retries on its next poll cycle. */
	static uint8_t evt_buf[ALP_CC3501E_MAX_PAYLOAD];
	size_t         got = 0;
	alp_status_t   s   = cc3501e_request(ctx,
	                                     ALP_CC3501E_CMD_GET_PENDING_EVENTS,
	                                     NULL,
	                                     0,
	                                     evt_buf,
	                                     sizeof(evt_buf),
	                                     &got,
	                                     CC3501E_REQ_TMO_MS);
	if (s != ALP_OK) return s;

	size_t off = 0;
	while (off + ALP_CC3501E_EVENT_HDR_BYTES <= got) {
		const uint8_t opcode = evt_buf[off];
		const uint8_t len    = evt_buf[off + 1u];
		if (off + ALP_CC3501E_EVENT_HDR_BYTES + (size_t)len > got) {
			break; /* truncated trailing entry -- stop cleanly */
		}
		ctx->event_cb(opcode, &evt_buf[off + ALP_CC3501E_EVENT_HDR_BYTES], len, ctx->event_user);
		off += ALP_CC3501E_EVENT_HDR_BYTES + (size_t)len;
	}
	return ALP_OK;
}

void cc3501e_deinit(cc3501e_t *ctx)
{
	if (ctx == NULL) return;
	ctx->initialised = false;
	ctx->bus         = NULL;
}
