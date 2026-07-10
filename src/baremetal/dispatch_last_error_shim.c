/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Plain-CMake baremetal dispatcher shim.
 *
 * Registry dispatchers share the Zephyr-shaped alp_z_* last-error hooks.
 * Baremetal builds route them onto the one common last-error slot
 * (ALP_LAST_ERROR_TLS-qualified in src/common/stub_backend.c) without
 * pulling in Zephyr.
 */

#include "alp/peripheral.h"
#include "alp_internal.h"
#include "alp_z_last_error.h"

void alp_z_set_last_error(alp_status_t s)
{
	alp_internal_set_last_error(s);
}

void alp_z_clear_last_error(void)
{
	alp_internal_set_last_error(ALP_OK);
}
