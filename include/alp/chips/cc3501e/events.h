/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file events.h
 * @brief CC3501E async-event callback registration + polling, and deinit.
 *
 * The low-level request primitive (@ref cc3501e_request) lives in
 * `<alp/chips/cc3501e/core.h>` alongside the driver context it operates
 * on; this subheader carries the event-callback registration + the
 * host-side event poll that drains the firmware's queued async events
 * through it, plus @ref cc3501e_deinit.
 */

#ifndef ALP_CHIPS_CC3501E_EVENTS_H
#define ALP_CHIPS_CC3501E_EVENTS_H

#include "alp/chips/cc3501e/core.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register or replace the async-event callback.  Pass cb=NULL to detach. */
alp_status_t cc3501e_set_event_callback(cc3501e_t *ctx, cc3501e_event_cb_t cb, void *user);

/**
 * @brief Poll the firmware for queued async events and dispatch them to the
 *        registered callback (CMD_GET_PENDING_EVENTS, opcode 0x05).
 *
 * This is the PRIMARY, benchable async-event mechanism on the current HW rev:
 * the CC35 GPIO17 -> Alif P2_6 attention line is a bodge NOT routed on the stock
 * EVK, so there is no interrupt to push events -- the host POLLS instead.  Each
 * call sends one GET_PENDING_EVENTS request, decodes the packed reply (a list of
 * @ref alp_cc3501e_event_entry_t { evt_opcode | len | payload[len] }), and
 * invokes the @ref cc3501e_set_event_callback callback once per queued event
 * (with the EVT_* opcode + its payload).  The firmware drains the ring as it
 * replies, so each event is delivered exactly once.
 *
 * Call it periodically (e.g. from a low-rate app thread; the SDK console runs a
 * ~500 ms poll when a companion is registered).  A no-op returning ALP_OK when
 * no callback is registered (the events stay queued in the firmware until a
 * callback is attached).  On the opt-in interrupt path
 * (CONFIG_ALP_SDK_CC3501E_EVENT_IRQ) the P2_6 edge ISR schedules this call from
 * a workqueue instead of / alongside the timer poll.
 *
 * @param ctx  Initialised driver context.
 * @return ALP_OK once the queue was drained + dispatched (even with zero
 *         events); ALP_ERR_NOT_READY if @p ctx is not initialised; the mapped
 *         firmware/link error (e.g. ALP_ERR_IO if the bridge was briefly down)
 *         otherwise -- the caller simply retries on the next poll.
 */
alp_status_t cc3501e_poll_events(cc3501e_t *ctx);

/** Free internal state.  Does not close the SPI bus -- caller owns it. */
void cc3501e_deinit(cc3501e_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_CC3501E_EVENTS_H */
