/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Yocto-side last-error shim for the shared class dispatchers.
 *
 * The portable class dispatchers (src/rtc_dispatch.c, src/wdt_dispatch.c,
 * and the rest of src/*_dispatch.c) are written Zephyr-first: they call
 * the thread-local helpers alp_z_set_last_error() / alp_z_clear_last_error()
 * that src/zephyr/last_error.c defines.  That file is NOT compiled on the
 * Yocto/Linux plain-CMake build -- here the last-error slot lives in
 * src/common/stub_backend.c behind alp_internal_set_last_error() (read back
 * by alp_last_error()).
 *
 * When the RTC + WDT registry migration (#33) pulled the shared dispatchers
 * into the Yocto link, those two extern symbols became undefined references.
 * This shim defines them as thin forwarders onto the common slot, so the one
 * canonical last-error static (z_last_error in stub_backend.c) stays the
 * single source the public alp_last_error() reads -- no second error slot,
 * no duplicate definition of alp_last_error.
 *
 * Compiled only on the Yocto build (src/yocto/CMakeLists.txt); the Zephyr
 * build keeps its own definitions in src/zephyr/last_error.c.
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
