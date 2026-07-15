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
 * @warning Payload lifetime: the @c payload pointer the callback receives
 *          points into @p ctx's OWN internal decode buffer and is valid ONLY
 *          for the duration of that one callback invocation -- copy anything
 *          you need to keep before returning. This call is NOT reentrant on
 *          the SAME @p ctx: calling it again on this ctx from inside the
 *          callback returns @ref ALP_ERR_BUSY immediately rather than
 *          racing/aliasing the buffer the outer call is still walking
 *          (issue #740). Two DIFFERENT @p ctx instances never share storage
 *          and may be polled concurrently with no coordination.
 *
 * @warning NOT a thread-safe mutex: @c evt_busy is a plain (non-atomic)
 *          test-then-set @c bool, not a compare-and-swap or an
 *          interrupt-masked critical section. It reliably rejects
 *          same-call-stack reentrancy (the callback documented above, or an
 *          ISR whose handler runs to completion before the interrupted
 *          thread resumes -- the single-core-M55/single-core-A55 case this
 *          driver targets). It does NOT provide mutual exclusion against a
 *          genuinely preemptive second caller on a DIFFERENT thread/core
 *          racing this same ctx: two callers can both observe
 *          @c evt_busy==false before either sets it, and both proceed into
 *          @c evt_buf. An application that polls the SAME @p ctx from more
 *          than one thread must serialize those calls itself (e.g. a mutex
 *          around cc3501e_poll_events()) -- this is exactly what the SDK's
 *          own in-tree caller does: companion_drain_events() (src/zephyr/
 *          console/alp_console_companion.c) wraps every cc3501e_poll_events()
 *          call in @c k_mutex_lock(&companion_bus_lock, K_FOREVER), which is
 *          what makes the CONFIG_ALP_SDK_CC3501E_EVENT_IRQ workqueue coexisting
 *          with the timer-poll thread safe.
 *
 * @param ctx  Initialised driver context.
 * @return ALP_OK once the queue was drained + dispatched (even with zero
 *         events); ALP_ERR_NOT_READY if @p ctx is not initialised;
 *         @ref ALP_ERR_BUSY if this ctx is already draining (reentrant call);
 *         the mapped firmware/link error (e.g. ALP_ERR_IO if the bridge was
 *         briefly down) otherwise -- the caller simply retries on the next
 *         poll.
 */
alp_status_t cc3501e_poll_events(cc3501e_t *ctx);

/** Free internal state.  Does not close the SPI bus -- caller owns it. */
void cc3501e_deinit(cc3501e_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_CC3501E_EVENTS_H */
