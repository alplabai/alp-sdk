/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Lock-free slot claim/release for the static handle + sidecar pools
 * used by the class dispatchers (src/<class>_dispatch.c) and the
 * Zephyr backends.  The pools used to claim slots with an unlocked
 * check-then-set, so two threads opening concurrently could both win
 * the same slot; these helpers make the claim a single atomic
 * compare-exchange instead.
 *
 * Compiler-builtin atomics (GCC/Clang __atomic_*) rather than an OS
 * mutex so the same header serves every ALP_OS backend -- the
 * dispatcher TUs compile for Zephyr today and are written to stay
 * portable to the yocto/baremetal trees, which have no k_mutex.  The
 * SDK already requires GCC/Clang (the backend registry rides on
 * __attribute__((section))), so the builtins are always available.
 */

#ifndef ALP_COMMON_SLOT_CLAIM_H
#define ALP_COMMON_SLOT_CLAIM_H

#include <stdbool.h>
#include <stdint.h>

#include "alp_thread_token.h"

/**
 * @brief Atomically claim a pool slot guarded by @p in_use.
 *
 * Compare-exchange false -> true with acquire semantics: exactly one
 * concurrent caller wins a free slot.  The winner may then initialise
 * the slot's other fields -- losers never touch a slot whose flag they
 * failed to flip.
 *
 * @param[in,out] in_use  The slot's in-use flag.
 * @return true when this caller claimed the slot.
 */
static inline bool alp_slot_try_claim(bool *in_use)
{
	bool expected = false;
	return __atomic_compare_exchange_n(
	    in_use, &expected, true, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

/**
 * @brief Return a slot to its pool.
 *
 * Release-store so every field written while the slot was held is
 * visible to the next claimer before the flag reads free.
 *
 * @param[in,out] in_use  The slot's in-use flag.
 */
static inline void alp_slot_release(bool *in_use)
{
	__atomic_store_n(in_use, false, __ATOMIC_RELEASE);
}

/**
 * @brief Atomically step a one-byte handle lifecycle state machine.
 *
 * Compare-exchange @p from -> @p to.  The dispatchers use this to make
 * transitions like idle -> transfer-in-flight and idle -> closing
 * mutually exclusive, which is what turns "close while another thread
 * is blocked in a transceive" from a use-after-free into a clean
 * ALP_ERR_BUSY.
 *
 * @param[in,out] state  Lifecycle byte inside the handle.
 * @param[in]     from   Expected current state.
 * @param[in]     to     Desired next state.
 * @return true when the transition happened.
 */
static inline bool alp_lifecycle_cas(uint8_t *state, uint8_t from, uint8_t to)
{
	return __atomic_compare_exchange_n(state, &from, to, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/**
 * @brief Read a lifecycle byte with acquire semantics.
 *
 * @param[in] state  Lifecycle byte inside the handle.
 * @return Current state value.
 */
static inline uint8_t alp_lifecycle_get(const uint8_t *state)
{
	return __atomic_load_n(state, __ATOMIC_ACQUIRE);
}

/**
 * @brief Write a lifecycle byte with release semantics.
 *
 * @param[in,out] state  Lifecycle byte inside the handle.
 * @param[in]     value  New state value.
 */
static inline void alp_lifecycle_set(uint8_t *state, uint8_t value)
{
	__atomic_store_n(state, value, __ATOMIC_RELEASE);
}

/* --------------------------------------------------------------------
 * Generic open/op/close guard (issue #629).
 *
 * The I2C/SPI *target* handles above hand-roll a lifecycle CAS
 * (UNOPENED -> IDLE -> CLOSING) because their close() can afford to
 * return ALP_ERR_BUSY when a transfer is in flight -- alp_status_t is
 * their existing return type.  Every *controller*-mode class
 * (gpu2d/uart/gpio/can/i2c/spi) instead ships a `void alp_*_close()`
 * that predates this fix; changing that return type is an ABI break
 * this ticket does not take.  These three helpers give those classes
 * the same race-freedom with a "close blocks until idle" contract
 * instead of "close refuses while busy":
 *
 *   - alp_handle_op_enter()  -- call before touching backend state;
 *     false means the handle is closed/closing, caller must bail
 *     with NOT_READY without touching state.
 *   - alp_handle_op_leave()  -- call after, unconditionally, on every
 *     exit path (including the false branch is NOT needed there --
 *     op_enter already backed the counter out).
 *   - alp_handle_begin_close() -- CAS OPEN -> CLOSING (false = already
 *     closing/closed/never-opened, caller's close() is then a no-op,
 *     matching every existing void-close idempotency contract), then
 *     spins until every op that entered before the CAS has left.
 *
 * The count-then-check order in alp_handle_op_enter is what removes
 * the TOCTOU window: the increment always happens before the
 * lifecycle read, so an op that observes OPEN is guaranteed counted
 * before alp_handle_begin_close's spin can pass, and an op that loses
 * the race to a CLOSING transition backs its own count back out
 * before touching backend state.
 *
 * The close-side spin is a deliberate, bounded busy-wait (no OS
 * primitive is portable across baremetal/yocto/Zephyr from this
 * shared header) -- correct because every class using this helper
 * only ever calls short, synchronous backend ops (no blocking
 * transfer is left outstanding across an open/op boundary the way
 * the SPI target's transceive is), so active_ops always drains in a
 * handful of instructions, never an unbounded wait.
 */
#define ALP_HANDLE_LC_UNOPENED 0u
#define ALP_HANDLE_LC_OPEN     1u
#define ALP_HANDLE_LC_CLOSING  2u

/**
 * @brief Enter a synchronous backend op, gated on the handle being open.
 *
 * @param[in,out] lifecycle   Handle lifecycle byte (ALP_HANDLE_LC_*).
 * @param[in,out] active_ops  Handle's in-flight-op counter.
 * @return true if the op may proceed (counted); false if the handle
 *         is closed/closing -- the counter has already been backed out.
 */
static inline bool alp_handle_op_enter(uint8_t *lifecycle, uint32_t *active_ops)
{
	__atomic_fetch_add(active_ops, 1u, __ATOMIC_ACQ_REL);
	if (alp_lifecycle_get(lifecycle) == ALP_HANDLE_LC_OPEN) {
		return true;
	}
	__atomic_fetch_sub(active_ops, 1u, __ATOMIC_ACQ_REL);
	return false;
}

/**
 * @brief Leave a synchronous backend op entered via alp_handle_op_enter().
 *
 * @param[in,out] active_ops  Handle's in-flight-op counter.
 */
static inline void alp_handle_op_leave(uint32_t *active_ops)
{
	__atomic_fetch_sub(active_ops, 1u, __ATOMIC_ACQ_REL);
}

/**
 * @brief Begin closing a handle: gate out new ops, then drain in-flight ones.
 *
 * @param[in,out] lifecycle   Handle lifecycle byte (ALP_HANDLE_LC_*).
 * @param[in]     active_ops  Handle's in-flight-op counter.
 * @return true if this caller won the OPEN -> CLOSING transition and
 *         may now tear the handle down; false if it was already
 *         closed/closing/never-opened (caller's close() is a no-op).
 */
static inline bool alp_handle_begin_close(uint8_t *lifecycle, uint32_t *active_ops)
{
	if (!alp_lifecycle_cas(lifecycle, ALP_HANDLE_LC_OPEN, ALP_HANDLE_LC_CLOSING)) {
		return false;
	}
	while (__atomic_load_n(active_ops, __ATOMIC_ACQUIRE) != 0u) {
		/* Spin: every op gated by alp_handle_op_enter() is a short,
		 * synchronous backend call, so this drains in a handful of
		 * instructions -- see the block comment above. */
	}
	return true;
}

/**
 * @brief Begin closing a handle that also hosts blocking ops (issue #629).
 *
 * Same CAS-then-drain contract as alp_handle_begin_close() above --
 * OPEN -> CLOSING, then wait for every op counted before the CAS to
 * leave -- but the drain SLEEPS between polls instead of busy-spinning,
 * so it is safe to use on a handle that counts an op taking a caller
 * `timeout_ms` (a real link-layer/broker/transfer round-trip that can
 * run far longer than "a handful of instructions").  This is the
 * generalisation of rpc_dispatch.c's _rpc_begin_close()/_rpc_drain()
 * (GHSA-xhm8-7f87-93q5) to the shared handle-pool helpers: same
 * sleep-poll-never-spin rationale, same portable sleep-tick primitive
 * (see src/common/alp_slot_claim.c). Defined out-of-line in
 * src/common/alp_slot_claim.c (not inline here) because the sleep
 * primitive needs an OS header (k_sleep / nanosleep) this header
 * deliberately does not pull in -- see the file comment at the top of
 * this header.
 *
 * A handle whose pool mixes short-sync ops with a blocking one (e.g.
 * BLE's advertise/gatt-notify alongside connect()) can use this
 * sleep-poll close uniformly: it drains the short ops just as
 * correctly, just via a sleep-poll loop instead of a spin.
 *
 * @param[in,out] lifecycle   Handle lifecycle byte (ALP_HANDLE_LC_*).
 * @param[in,out] active_ops  Handle's in-flight-op counter.
 * @return true if this caller won the OPEN -> CLOSING transition and
 *         may now tear the handle down; false if it was already
 *         closed/closing/never-opened (caller's close() is a no-op).
 */
bool alp_handle_begin_close_blocking(uint8_t *lifecycle, uint32_t *active_ops);

/**
 * @brief Sleep-poll wait for @p active_ops to reach 0 -- the drain half
 *        of alp_handle_begin_close_blocking(), without the CAS.
 *
 * Used by a close protocol that needs to elect single ownership by
 * some OTHER means before deciding whether/when to drain (e.g. a
 * two-step shutdown()/destroy() split for a backend with a worker
 * thread -- see can_ops.h's alp_can_close_finalize(), issue #756,
 * mirroring rpc_dispatch.c's _rpc_drain()).
 *
 * @param[in,out] active_ops  Handle's in-flight-op counter.
 */
void alp_handle_drain_blocking(uint32_t *active_ops);

/* --------------------------------------------------------------------
 * Reentrant self-close guard (issue #756).
 *
 * A handle op that invokes an application callback SYNCHRONOUSLY,
 * inline, before returning (alp_mqtt_loop()'s message callback,
 * alp_ble_scan_start()'s scan callback) can have that callback call
 * close() on its OWN handle, on the SAME thread and call stack that is
 * still inside the op's own alp_handle_op_enter() count.
 * alp_handle_begin_close_blocking() above would sleep-poll forever in
 * that case: the count it is waiting to see drop to zero cannot drop
 * until this very thread returns from the op, which it cannot do while
 * it is asleep inside close().
 *
 * The two helpers below split the close in two so the callback-
 * invoking op's OWN wrapper can finish the close AFTER it leaves,
 * instead of the reentrant close() call blocking on itself -- the same
 * "defer teardown to the context that can safely run it" shape
 * rpc_dispatch.c/src/backends/rpc/yocto_drv.c use for a worker-thread
 * self-close (GHSA-xhm8-7f87-93q5), specialised here for a callback
 * that runs on the calling thread's own stack rather than a separate
 * worker thread.
 *
 * Usage (see src/mqtt_dispatch.c's alp_mqtt_loop()/alp_mqtt_close() or
 * src/ble_dispatch.c's alp_ble_scan_start()/alp_ble_close()):
 *
 *   op wrapper (the one that may invoke a callback synchronously):
 *     alp_handle_op_enter(...);
 *     alp_handle_cb_enter(&h->cb_thread, &h->cb_active);
 *     rc = ops->the_op(...);            // may synchronously self-close
 *     alp_handle_cb_leave(&h->cb_active);
 *     // Consume close_pending BEFORE op_leave (dev-review follow-up):
 *     // this thread still holds its own counted op here, so `h` cannot
 *     // yet be freed by anyone -- a concurrent EXTERNAL closer's own
 *     // drain cannot complete until the op_leave below runs.  Reading
 *     // close_pending any LATER (after op_leave) would race that
 *     // external closer, which is free the instant active_ops hits 0.
 *     bool self_closed = alp_handle_take_deferred_close(&h->close_pending);
 *     alp_handle_op_leave(&h->active_ops);
 *     if (self_closed) {
 *         // begin_close_selfaware()'s CAS already elected this thread
 *         // the unique owner of this handle's close -- safe to drain
 *         // (any OTHER concurrent op) and tear down, exactly like the
 *         // ALP_HANDLE_CLOSE_NOW path alp_*_close() runs itself.
 *         alp_handle_drain_blocking(&h->active_ops);
 *         ops->close(...); lifecycle_set(UNOPENED); free the slot.
 *     }
 *
 *   close():
 *     alp_handle_close_mode_t mode;
 *     if (!alp_handle_begin_close_selfaware(&h->lifecycle, &h->active_ops,
 *                                           &h->cb_active, &h->cb_thread,
 *                                           &mode, &h->close_pending)) {
 *         return; // already closing/closed/never-opened -- no-op
 *     }
 *     if (mode == ALP_HANDLE_CLOSE_DEFERRED) {
 *         return; // the op wrapper above finishes the close once it leaves
 *     }
 *     // mode == ALP_HANDLE_CLOSE_NOW: active_ops already drained to 0.
 *     ops->close(...); lifecycle_set(UNOPENED); free the slot.
 *
 * Known limitation (documented, not defended): this assumes at most
 * ONE thread is ever inside the callback-invoking op for a given
 * handle at a time -- concurrent overlapping calls to
 * alp_mqtt_loop()/alp_ble_scan_start() on the SAME handle from two
 * different threads is already discouraged/undefined usage for these
 * single-broker/single-radio classes (matches alp_rpc_call()'s own
 * single-sync-slot design), so the second writer simply overwrites
 * `cb_thread`/`cb_active` rather than being tracked as a set.
 */

/** @brief Whether a self-close was detected by
 *         alp_handle_begin_close_selfaware(). */
typedef enum {
	/** No self-close: active_ops has ALREADY been drained to 0 on
	 *  return.  Safe to tear the handle down right here. */
	ALP_HANDLE_CLOSE_NOW = 0,
	/** The calling thread is the SAME thread currently running this
	 *  handle's callback-invoking op.  active_ops has NOT been
	 *  drained -- the caller must return immediately without touching
	 *  backend state; the op's own wrapper finishes the close (see
	 *  alp_handle_take_deferred_close()) once it leaves. */
	ALP_HANDLE_CLOSE_DEFERRED = 1,
} alp_handle_close_mode_t;

/**
 * @brief Mark the calling thread as "inside this handle's callback-
 *        invoking op" (issue #756).  Call before invoking the backend
 *        op that may synchronously run an application callback.
 *
 * @param[out] cb_thread  Set to this thread's token.
 * @param[out] cb_active  Set true (release store).
 */
static inline void alp_handle_cb_enter(uintptr_t *cb_thread, bool *cb_active)
{
	*cb_thread = alp_thread_token_self();
	__atomic_store_n(cb_active, true, __ATOMIC_RELEASE);
}

/**
 * @brief Clear the "inside this handle's callback-invoking op" marker.
 *        Call right after the backend op returns, BEFORE
 *        alp_handle_op_leave().
 *
 * @param[out] cb_active  Set false (release store).
 */
static inline void alp_handle_cb_leave(bool *cb_active)
{
	__atomic_store_n(cb_active, false, __ATOMIC_RELEASE);
}

/**
 * @brief Self-close-aware begin-close (issue #756).  See this
 *        section's block comment above for the full usage contract.
 *
 * @param[in,out] lifecycle     Handle lifecycle byte (ALP_HANDLE_LC_*).
 * @param[in,out] active_ops    Handle's in-flight-op counter.
 * @param[in]     cb_active     Set by alp_handle_cb_enter()/_leave().
 * @param[in]     cb_thread     Set by alp_handle_cb_enter().
 * @param[out]    mode          ALP_HANDLE_CLOSE_NOW or _DEFERRED; only
 *                               meaningful when this function returns true.
 * @param[out]    close_pending Set true on the DEFERRED path; consumed
 *                               by alp_handle_take_deferred_close().
 * @return true if this caller won the OPEN -> CLOSING transition
 *         (false = already closing/closed/never-opened, caller's
 *         close() is a no-op -- matches every other void-close
 *         idempotency contract in this header).
 */
bool alp_handle_begin_close_selfaware(uint8_t                 *lifecycle,
                                      uint32_t                *active_ops,
                                      const bool              *cb_active,
                                      const uintptr_t         *cb_thread,
                                      alp_handle_close_mode_t *mode,
                                      bool                    *close_pending);

/**
 * @brief Atomically consume a pending deferred self-close (issue #756,
 *        dev-review follow-up).  Call from the callback-invoking op's
 *        own wrapper AFTER the backend op returns but BEFORE
 *        alp_handle_op_leave() -- see this section's usage contract
 *        above.
 *
 * Reading (and clearing) close_pending here, while this thread still
 * holds its own counted op, is what makes this race-free: a concurrent
 * EXTERNAL close cannot free the handle until active_ops drains to 0,
 * which cannot happen before the caller's OWN alp_handle_op_leave()
 * runs.  Doing this check any later (after op_leave, the pre-fix
 * ordering) let a genuine external closer -- whose own drain unblocks
 * the instant this op leaves -- free the handle while this thread was
 * still about to dereference it to check close_pending, an exact
 * use-after-free / double-free window.
 *
 * @param[in,out] close_pending Set by alp_handle_begin_close_selfaware();
 *                               atomically cleared here.
 * @return true if a self-close was pending (now consumed) -- the
 *         caller uniquely owns this handle's teardown from here: call
 *         alp_handle_op_leave(), THEN alp_handle_drain_blocking()
 *         (safe now -- this thread has already left), THEN run
 *         ops->close(), set UNOPENED, release the slot.  false if
 *         nothing was pending (the common case) -- the caller must not
 *         touch the handle again after alp_handle_op_leave().
 */
static inline bool alp_handle_take_deferred_close(bool *close_pending)
{
	return __atomic_exchange_n(close_pending, false, __ATOMIC_ACQ_REL);
}

#endif /* ALP_COMMON_SLOT_CLAIM_H */
