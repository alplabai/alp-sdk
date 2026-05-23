/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software WDT fallback.  No-op stub for native_sim builds; never
 * actually resets anything.  Apps that depend on the watchdog
 * action MUST NOT use this backend in production.
 *
 * @par Cost: ROM ~200 B, RAM 0 B (stateless).
 * @par Performance: O(1) per call; all ops just return ALP_OK.
 *      For native_sim build / test only.
 */

#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/wdt.h>

#include "wdt_ops.h"

static alp_status_t sw_open(uint32_t wdt_id,
                            const alp_wdt_config_t *cfg,
                            alp_wdt_backend_state_t *st,
                            alp_capabilities_t *caps_out) {
    (void)cfg;
    st->dev = NULL;
    st->wdt_id = wdt_id;
    st->channel_id = 0;
    st->be_data = NULL;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t sw_feed(alp_wdt_backend_state_t *st)    { (void)st; return ALP_OK; }
static alp_status_t sw_disable(alp_wdt_backend_state_t *st) { (void)st; return ALP_OK; }

static const alp_wdt_ops_t _ops = {
    .open    = sw_open,
    .feed    = sw_feed,
    .disable = sw_disable,
    .close   = NULL,
};

ALP_BACKEND_REGISTER(wdt, sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_fallback",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
