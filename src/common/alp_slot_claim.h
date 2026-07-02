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
	return __atomic_compare_exchange_n(
	    state, &from, to, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
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

#endif /* ALP_COMMON_SLOT_CLAIM_H */
