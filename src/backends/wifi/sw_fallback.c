/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software Wi-Fi fallback.  Wildcard backend at priority 0 -- picked
 * only when no hardware backend is linked into the build
 * (native_sim trimmed-image case).  No real Wi-Fi radio exists
 * under native_sim, so the stub lets examples that include
 * <alp/iot.h> compile and exercise the dispatcher without pulling
 * in CONFIG_WIFI + CONFIG_NET_MGMT.
 *
 * Contract:
 *   - open()        -> ALP_OK (no radio bring-up)
 *   - connect()     -> ALP_ERR_NOT_IMPLEMENTED (no real AP)
 *   - disconnect()  -> ALP_OK (idempotent on a never-connected radio)
 *   - close()       -> no-op
 *
 * Matches the design spec Section 5 sw_fallback contract.
 *
 * @par Cost: ROM ~120 B, zero RAM (no per-handle state).
 * @par Performance: O(1) per call; every op short-circuits to
 *      ALP_OK / ALP_ERR_NOT_IMPLEMENTED with no Zephyr-subsystem
 *      touch.
 */

#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/iot.h>
#include <alp/peripheral.h>

#include "wifi_ops.h"

static alp_status_t sw_open(alp_wifi_backend_state_t *st,
                            alp_capabilities_t *caps_out)
{
    st->be_data     = NULL;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t sw_connect(alp_wifi_backend_state_t *st,
                               const alp_wifi_credentials_t *creds,
                               uint32_t timeout_ms)
{
    (void)st;
    (void)creds;
    (void)timeout_ms;
    /* No real radio under native_sim, so no real AP can be reached.
     * Returning NOT_IMPLEMENTED rather than OK keeps callers from
     * thinking they've associated when they haven't. */
    return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t sw_disconnect(alp_wifi_backend_state_t *st)
{
    (void)st;
    /* Idempotent on a never-connected radio. */
    return ALP_OK;
}

static void sw_close(alp_wifi_backend_state_t *st)
{
    (void)st;
}

static const alp_wifi_ops_t _ops = {
    .open       = sw_open,
    .connect    = sw_connect,
    .disconnect = sw_disconnect,
    .close      = sw_close,
};

ALP_BACKEND_REGISTER(wifi, sw_fallback, {
    .silicon_ref = "*",
    .vendor      = "sw_fallback",
    .base_caps   = 0u,
    .priority    = 0,
    .ops         = &_ops,
    .probe       = NULL,
});
