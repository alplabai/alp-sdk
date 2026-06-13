/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- chip lifecycle + meta operations.
 *
 * Built ONLY for CC3501E_HAL_BACKEND=ti (the bench build), against TI's
 * SimpleLink CC33xx SDK.  CI builds the stub backend instead, so this
 * file is never on the SDK-free path.
 *
 * STATUS: bench skeleton.  The seam contracts and call structure are in
 * place; the TI-SDK-specific calls flagged "[TI-SDK]" must be confirmed
 * against the SimpleLink CC33xx headers on the bench (the exact symbol
 * names differ across the CC32xx / CC33xx generations) and are NOT
 * fabricated here -- they return NOTIMPL until wired, so a half-built
 * bench image fails loud rather than reporting a fabricated MAC.
 */

#include <stdint.h>

#include "../cc3501e_hw.h"

/* [TI-SDK] SimpleLink headers, e.g.:
 *   #include <ti/drivers/net/wifi/simplelink.h>
 *   #include <ti/devices/cc33xx/.../hw_types.h>
 * Pulled in by the vendored SDK on the bench build. */

/* Deferred-reset latch: CMD_RESET sets this; the transport's post-reply
 * drain path (or the idle tick) performs the actual reboot once the ack
 * has been clocked back to the host. */
static volatile uint8_t reset_pending;

void                    cc3501e_hw_init(void)
{
    /* [TI-SDK] Clock/power bring-up + sl_Start() of the SimpleLink
     * network processor.  For v0.1 bring-up the radio need not be
     * started to answer PING / GET_VERSION; sl_Start() moves in with
     * the v0.2 Wi-Fi handlers. */
}

void cc3501e_hw_tick(void)
{
    if (reset_pending) {
        /* [TI-SDK] Perform the deferred reboot once the reply is drained
         * (e.g. sl_Stop() then a system reset).  Confirm the SDK's
         * reset primitive on the bench. */
    }
}

int cc3501e_hw_get_mac(uint8_t mac[6])
{
    if (mac == 0) return CC3501E_HW_ERR_INVAL;
    /* [TI-SDK] Read the factory MAC, e.g. sl_NetCfgGet() with the
     * MAC-address config id (confirm the exact id/signature for the
     * CC33xx SimpleLink build).  Until wired on the bench, report
     * NOTIMPL rather than a fabricated address. */
    return CC3501E_HW_ERR_NOTIMPL;
}

void cc3501e_hw_request_reset(void)
{
    reset_pending = 1u;
}
