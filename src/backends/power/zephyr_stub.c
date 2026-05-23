/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Power stub backend.  Wildcard ("*") registration at priority 0:
 * picks up every silicon_ref the build targets so apps that
 * #include <alp/power.h> link cleanly on every supported SoC.
 *
 * Behaviour differs from the Camera / Display / GPU2D stubs:
 * stub_open returns ALP_OK so the dispatcher hands the caller a
 * real handle, and stub_configure_wake_source accepts the bitmap
 * silently.  request_sleep is the actual "not implemented" gate.
 * Customer wake-bitmap setup code links + runs unchanged; only the
 * actual sleep call sees the NOT_IMPLEMENTED status.
 *
 * Real backends (Zephyr pm_policy_* + per-SoC pm_state tables on
 * AEN; GD32G553 supervisor CMD_POWER_MODE_SET opcode 0x28 on V2N)
 * land per the tracking issue below with their own silicon-specific
 * entries at higher priority than this wildcard.
 *
 * @par Yocto / Linux path: deferred to slice #33.
 *      The Yocto `/sys/power/state`-write + `/sys/class/rtc/rtcN/
 *      wakealarm` path documented in <alp/power.h> is *not* served
 *      by this stub.  Linux backends land in a dedicated slice per
 *      the standing "NEVER touch src/yocto/*.c" guardrail; until
 *      then customers building against ALP_OS=yocto see
 *      ALP_ERR_NOT_IMPLEMENTED from request_sleep.  Open + wake-
 *      source configuration still succeed so application setup
 *      code keeps linking unchanged across both consumer paths.
 *
 * @par Tracking: github.com/alplabai/alp-sdk/issues/22
 */

#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/power.h>

#include "power_ops.h"

static alp_status_t stub_open(alp_power_backend_state_t *state,
                              alp_capabilities_t *caps_out)
{
    (void)state;
    (void)caps_out;
    /* Successful open is required so the dispatcher hands the caller
     * a handle; alp_power_open always returns a valid pointer.  The
     * real "this feature isn't implemented" surface is at
     * request_sleep below. */
    return ALP_OK;
}

static alp_status_t stub_configure_wake_source(alp_power_backend_state_t *state,
                                               uint32_t wake_bitmap)
{
    (void)state;
    (void)wake_bitmap;
    /* Accept any bitmap silently; the stub's request_sleep will fail
     * anyway, and the dispatcher's mirror keeps the bitmap visible
     * for the INVAL guard in alp_power_request_sleep. */
    return ALP_OK;
}

static alp_status_t stub_request_sleep(alp_power_backend_state_t *state,
                                       alp_power_mode_t mode,
                                       uint32_t wake_after_ms,
                                       alp_power_wake_info_t *info)
{
    (void)state;
    (void)wake_after_ms;
    if (info != NULL) {
        info->realised_mode = mode;
        info->wake_source   = 0u;
        info->slept_ms      = 0u;
    }
    return ALP_ERR_NOT_IMPLEMENTED;
}

static const alp_power_ops_t _ops = {
    .open                  = stub_open,
    .configure_wake_source = stub_configure_wake_source,
    .request_sleep         = stub_request_sleep,
    .close                 = NULL,
};

ALP_BACKEND_REGISTER(power, zephyr_stub, {
    .silicon_ref = "*",
    .vendor      = "stub",
    .base_caps   = 0u,
    .priority    = 0,
    .ops         = &_ops,
    .probe       = NULL,
});
