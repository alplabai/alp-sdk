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

#endif /* ALP_COMMON_SLOT_CLAIM_H */
