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

/**
 * @brief Subscribe to async events from this companion.
 *
 * Events fan out to EVERY registered subscriber, because a context legitimately
 * has more than one consumer: the SDK's Zephyr console companion registers its
 * own callback on the shared context and polls it, and the application wants
 * the same events. This replaces an earlier single-callback slot in which the
 * last registration silently won -- the console registers from its init path,
 * after @c main() has set the application's callback, so the application
 * received nothing while every call still reported ALP_OK (issue #1723).
 *
 * Registration is idempotent per (@p cb, @p user) pair: registering the same
 * pair twice leaves one subscription, so a caller that re-arms defensively does
 * not start receiving each event twice.
 *
 * @param ctx   Initialised driver context.
 * @param cb    Callback to invoke once per queued event. Must not be NULL --
 *              use @ref cc3501e_remove_event_callback to unsubscribe.
 * @param user  Opaque pointer passed back to @p cb; also part of the identity
 *              of the subscription for removal.
 * @return ALP_OK on success (including a duplicate registration);
 *         ALP_ERR_NOT_READY if @p ctx is not initialised; ALP_ERR_INVAL if
 *         @p cb is NULL; ALP_ERR_NOMEM when all
 *         @ref CC3501E_EVENT_SUBSCRIBERS slots are taken -- the registration is
 *         REFUSED rather than displacing an existing subscriber.
 */
alp_status_t cc3501e_add_event_callback(cc3501e_t *ctx, cc3501e_event_cb_t cb, void *user);

/**
 * @brief Unsubscribe a callback previously added with
 *        @ref cc3501e_add_event_callback.
 *
 * The (@p cb, @p user) pair must match the registration exactly. A callback may
 * remove itself from inside a dispatch; the in-progress fan-out still completes
 * for the remaining subscribers of that event.
 *
 * @param ctx   Initialised driver context.
 * @param cb    The callback to remove.
 * @param user  The @c user pointer it was registered with.
 * @return ALP_OK if it was removed; ALP_ERR_NOT_READY if @p ctx is not
 *         initialised; ALP_ERR_INVAL if @p cb is NULL; ALP_ERR_NOT_FOUND if
 *         that pair was not subscribed.
 */
alp_status_t cc3501e_remove_event_callback(cc3501e_t *ctx, cc3501e_event_cb_t cb, void *user);

/**
 * @brief Poll the firmware for queued async events and dispatch them to the
 *        registered callback (CMD_GET_PENDING_EVENTS, opcode 0x05).
 *
 * This is the PRIMARY, benchable async-event mechanism on the current HW rev:
 * the CC35 GPIO17 -> Alif P2_6 attention line is a bodge NOT routed on the stock
 * EVK, so there is no interrupt to push events -- the host POLLS instead.  Each
 * call sends one GET_PENDING_EVENTS request, decodes the packed reply (a list of
 * @ref alp_cc3501e_event_entry_t { evt_opcode | len | payload[len] }), and
 * invokes EVERY callback registered with @ref cc3501e_add_event_callback once
 * per queued event (with the EVT_* opcode + its payload).  The firmware drains
 * the ring as it replies, so each event is read off the wire exactly once and
 * then fanned out to all subscribers.
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
 *          call in @c k_mutex_lock(&companion_events_lock, K_FOREVER), which
 *          is what makes the CONFIG_ALP_SDK_CC3501E_EVENT_IRQ workqueue
 *          coexisting with the timer-poll thread safe.  (This is separate
 *          from -- and still needed alongside -- @ref cc3501e_request's own
 *          internal transport lock added for issue #1116, which serialises
 *          only the single request/reply exchange inside each
 *          cc3501e_poll_events() call, not the evt_busy/evt_buf walk around
 *          it.)
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
