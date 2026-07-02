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
 */

#include <stdbool.h>

#include <alp/peripheral.h>

static bool _initialised;

alp_status_t alp_init(void)
{
	if (_initialised) {
		return ALP_OK; /* idempotent */
	}
	/* Backend-registry walk needs no priming; nothing to do yet. */
	_initialised = true;
	return ALP_OK;
}

alp_status_t alp_deinit(void)
{
	_initialised = false;
	return ALP_OK;
}
