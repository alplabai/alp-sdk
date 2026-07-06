/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * SDK lifecycle: alp_init / alp_deinit.
 *
 * OS-agnostic -- compiled into every backend build (Zephyr module,
 * Yocto, baremetal).  Deliberately thin today: the backend registry
 * is linker-section based (see <alp/backend.h>) and needs no runtime
 * setup, and no current backend hand-rolls per-app bring-up that
 * belongs here.  The functions exist so application code has ONE
 * portable entry point that future backends (bridge links, vendor
 * HAL init, clock bring-up) can hook without breaking every app.
 *
 * The once-guard is a compiler-builtin atomic compare-exchange (no
 * OS dependency -- this TU builds for baremetal too), so concurrent
 * first calls race cleanly: exactly one caller flips the flag and
 * runs the bring-up work; the others return ALP_OK immediately.
 * See <alp/peripheral.h> for the full thread-safety contract.
 */

#include <stdint.h>

#include <alp/peripheral.h>

static uint8_t _initialised; /* 0 = uninitialised, 1 = initialised; atomic access only */

alp_status_t alp_init(void)
{
	uint8_t expected = 0u;
	if (!__atomic_compare_exchange_n(
	        &_initialised, &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		return ALP_OK; /* idempotent: someone already initialised (or is doing so) */
	}
	/* Backend-registry walk needs no priming; nothing to do yet.
	 * When bring-up work lands here, a failure must reset the flag
	 * to 0 before returning so a later retry can run it again. */
	return ALP_OK;
}

alp_status_t alp_deinit(void)
{
	__atomic_store_n(&_initialised, 0u, __ATOMIC_RELEASE);
	return ALP_OK;
}
