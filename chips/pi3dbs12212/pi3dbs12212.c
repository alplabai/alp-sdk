/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Diodes Inc PI3DBS12212A 2:1 differential mux driver.  See
 * <alp/chips/pi3dbs12212.h> for the public API + the V2N-M1 wiring
 * convention.  GPIO-only; no I2C surface.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/pi3dbs12212.h"

alp_status_t pi3dbs12212_init(pi3dbs12212_t *ctx, alp_gpio_t *pd, alp_gpio_t *sel)
{
    if (ctx == NULL || pd == NULL || sel == NULL) return ALP_ERR_INVAL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->pd  = pd;
    ctx->sel = sel;

    /* Start in OFF: drive PD low, leave SEL at 0 for determinism. */
    alp_status_t s = alp_gpio_write(pd, 0);
    if (s != ALP_OK) return s;
    s = alp_gpio_write(sel, 0);
    if (s != ALP_OK) return s;

    ctx->state       = PI3DBS_STATE_OFF;
    ctx->initialised = true;
    return ALP_OK;
}

alp_status_t pi3dbs12212_set_state(pi3dbs12212_t *ctx, pi3dbs12212_state_t state)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;

    /* Glitch-free transition: if moving between PATH_0 <-> PATH_1
     * (both enabled), pulse OFF first so SEL changes don't traverse
     * a metastable mid-point on the live link.  Cost: a few hundred
     * nanoseconds of PCIe link interruption -- the host's PCIe IP
     * will re-train. */
    alp_status_t s;
    if ((ctx->state == PI3DBS_STATE_PATH_0 && state == PI3DBS_STATE_PATH_1) ||
        (ctx->state == PI3DBS_STATE_PATH_1 && state == PI3DBS_STATE_PATH_0)) {
        s = alp_gpio_write(ctx->pd, 0);
        if (s != ALP_OK) return s;
    }

    switch (state) {
    case PI3DBS_STATE_OFF:
        s = alp_gpio_write(ctx->pd, 0);
        if (s != ALP_OK) return s;
        break;
    case PI3DBS_STATE_PATH_0:
        s = alp_gpio_write(ctx->sel, 0);
        if (s != ALP_OK) return s;
        s = alp_gpio_write(ctx->pd, 1);
        if (s != ALP_OK) return s;
        break;
    case PI3DBS_STATE_PATH_1:
        s = alp_gpio_write(ctx->sel, 1);
        if (s != ALP_OK) return s;
        s = alp_gpio_write(ctx->pd, 1);
        if (s != ALP_OK) return s;
        break;
    default:
        return ALP_ERR_INVAL;
    }
    ctx->state = state;
    return ALP_OK;
}

alp_status_t pi3dbs12212_get_state(pi3dbs12212_t *ctx, pi3dbs12212_state_t *state)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (state == NULL) return ALP_ERR_INVAL;
    *state = ctx->state;
    return ALP_OK;
}

void pi3dbs12212_deinit(pi3dbs12212_t *ctx)
{
    if (ctx == NULL) return;
    if (ctx->initialised) {
        /* Safe quiescent state: mux disabled. */
        (void)alp_gpio_write(ctx->pd, 0);
    }
    ctx->initialised = false;
    ctx->pd          = NULL;
    ctx->sel         = NULL;
}
