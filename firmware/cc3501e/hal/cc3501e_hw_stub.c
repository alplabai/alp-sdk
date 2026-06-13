/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Default HAL implementation: hardware-free.  Lifecycle hooks are
 * no-ops and HW-touching ops return CC3501E_HW_ERR_NOTIMPL, so the
 * protocol round-trip (PING / GET_VERSION) is exercisable on the host
 * with no TI SimpleLink SDK on the workspace.
 *
 * The real implementation against TI's SimpleLink CC33xx SDK lives
 * under hal/ti/; the build picks one or the other via
 * CC3501E_HAL_BACKEND in CMakeLists.txt.
 */

#include <stdint.h>

#include "cc3501e_hw.h"

void cc3501e_hw_init(void)
{
    /* no-op on the stub backend */
}

void cc3501e_hw_tick(void)
{
    /* no-op on the stub backend */
}

int cc3501e_hw_get_mac(uint8_t mac[6])
{
    /* No radio on the host stub -- zero the buffer and report NOTIMPL so
     * the protocol layer answers RESP_ERR_NOT_READY rather than handing
     * back a fabricated MAC. */
    if (mac != 0) {
        for (unsigned i = 0u; i < 6u; ++i)
            mac[i] = 0u;
    }
    return CC3501E_HW_ERR_NOTIMPL;
}

void cc3501e_hw_request_reset(void)
{
    /* no-op on the stub backend */
}
