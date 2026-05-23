/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software QEnc fallback.  Deterministic step counter for
 * native_sim builds; advances by +7 per get_position call so
 * tests observe a stable, non-zero accumulating value.
 *
 * @par Cost: ROM ~300 B, RAM 4 B per handle (the accumulator).
 * @par Performance: O(1) per call; no host clock access.  Tests
 *      assert deterministic step values.
 */

#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/counter.h>
#include <alp/peripheral.h>

#include "qenc_ops.h"

static alp_status_t sw_open(const alp_qenc_config_t *cfg,
                            alp_qenc_backend_state_t *st,
                            alp_capabilities_t *caps_out) {
    (void)cfg;
    st->dev = NULL;
    st->encoder_id = cfg->encoder_id;
    st->last_position = 0;
    st->be_data = NULL;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t sw_get_position(alp_qenc_backend_state_t *st, int32_t *pos_out) {
    st->last_position += 7;
    *pos_out = st->last_position;
    return ALP_OK;
}

static alp_status_t sw_reset_position(alp_qenc_backend_state_t *st) {
    st->last_position = 0;
    return ALP_OK;
}

static const alp_qenc_ops_t _ops = {
    .open           = sw_open,
    .get_position   = sw_get_position,
    .reset_position = sw_reset_position,
    .close          = NULL,
};

ALP_BACKEND_REGISTER(qenc, sw_fallback, {
    .silicon_ref = "*",
    .vendor      = "sw_fallback",
    .base_caps   = 0u,
    .priority    = 0,
    .ops         = &_ops,
    .probe       = NULL,
});
