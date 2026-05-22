/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * V2N QEnc backend routed through the GD32G553 supervisor MCU.
 * Lifted from the #if-ladder branches of the legacy
 * peripheral_qenc.c during the Slice 4a registry migration.  Every
 * E1M encoder rides the GD32 IO MCU bridge on V2N silicon.
 */

#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/counter.h>
#include <alp/peripheral.h>

#include "qenc_ops.h"
#include "v2n_supervisor.h"

static alp_status_t br_open(const alp_qenc_config_t *cfg,
                            alp_qenc_backend_state_t *st,
                            alp_capabilities_t *caps_out) {
    /* Probe the supervisor up front so open() surfaces bus failures
     * instead of deferring them to the first read. */
    gd32g553_t *ctx = NULL;
    alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);
    if (s != ALP_OK) return s;
    alp_z_v2n_supervisor_release();
    st->dev = NULL;                                    /* bridge sentinel */
    st->encoder_id = cfg->encoder_id;
    st->last_position = 0;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t br_get_position(alp_qenc_backend_state_t *st, int32_t *pos_out) {
    gd32g553_t *ctx = NULL;
    alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);
    if (s != ALP_OK) return s;
    s = gd32g553_qenc_read(ctx, (uint8_t)st->encoder_id, pos_out);
    alp_z_v2n_supervisor_release();
    if (s == ALP_OK) st->last_position = *pos_out;
    return s;
}

static alp_status_t br_reset_position(alp_qenc_backend_state_t *st) {
    gd32g553_t *ctx = NULL;
    alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);
    if (s != ALP_OK) return s;
    s = gd32g553_qenc_reset(ctx, (uint8_t)st->encoder_id);
    alp_z_v2n_supervisor_release();
    if (s == ALP_OK) st->last_position = 0;
    return s;
}

static const alp_qenc_ops_t _ops = {
    .open           = br_open,
    .get_position   = br_get_position,
    .reset_position = br_reset_position,
    .close          = NULL,
};

ALP_BACKEND_REGISTER(qenc, gd32_bridge, {
    .silicon_ref = "renesas:rzv2n:n44",
    .vendor      = "renesas",
    .base_caps   = 0u,
    .priority    = 100,
    .ops         = &_ops,
    .probe       = NULL,
});
