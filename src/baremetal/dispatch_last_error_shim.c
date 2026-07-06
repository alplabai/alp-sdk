/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Plain-CMake baremetal dispatcher shim.
 *
 * Registry dispatchers share the Zephyr-shaped alp_z_* last-error hooks.
 * Baremetal builds use the SDK-global last-error slot from peripheral.h,
 * so route the dispatcher hooks there without pulling Zephyr.
 */

#include "alp/peripheral.h"
#include "alp_internal.h"

void alp_z_set_last_error(alp_status_t s)
{
	alp_internal_set_last_error(s);
}

void alp_z_clear_last_error(void)
{
	alp_internal_set_last_error(ALP_OK);
}
