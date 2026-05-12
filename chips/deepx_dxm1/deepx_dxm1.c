/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * DEEPX DX-M1 host-side bring-up sequencer for V2N-M1.  See
 * <alp/chips/deepx_dxm1.h> for the surrounding rationale +
 * upstream-repo cross-links.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/deepx_dxm1.h"
#include "alp/peripheral.h"

/* Translate the polarity enum into the GPIO logic level that asserts
 * reset.  asserted = the silicon is HELD in reset.  V2N-M1 default
 * is ACTIVE_LOW per the schematic (host drives M1_RESET low to hold
 * reset, high to release). */
static bool asserted_level(deepx_dxm1_reset_polarity_t p)
{
    return p == DEEPX_DXM1_RESET_ACTIVE_HIGH;
}

static bool released_level(deepx_dxm1_reset_polarity_t p)
{
    return !asserted_level(p);
}

alp_status_t deepx_dxm1_init(deepx_dxm1_t *ctx,
                             alp_gpio_t *m1_reset,
                             pi3dbs12212_t *pcie_mux,
                             pi3dbs12212_state_t deepx_path)
{
    if (ctx == NULL || m1_reset == NULL || pcie_mux == NULL) return ALP_ERR_INVAL;
    if (deepx_path != PI3DBS_STATE_PATH_0 && deepx_path != PI3DBS_STATE_PATH_1) {
        return ALP_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->m1_reset_pin   = m1_reset;
    ctx->pcie_mux       = pcie_mux;
    ctx->deepx_path     = deepx_path;
    ctx->reset_polarity = DEEPX_DXM1_RESET_ACTIVE_LOW; /* V2N-M1 default */

    /* Park M1_RESET asserted + mux off so we start from a known
     * quiescent state regardless of POR-time residual levels. */
    alp_status_t s = alp_gpio_write(m1_reset, asserted_level(ctx->reset_polarity));
    if (s != ALP_OK) return s;
    s = pi3dbs12212_set_state(pcie_mux, PI3DBS_STATE_OFF);
    if (s != ALP_OK) return s;

    ctx->initialised = true;
    return ALP_OK;
}

alp_status_t deepx_dxm1_set_reset_polarity(deepx_dxm1_t *ctx,
                                           deepx_dxm1_reset_polarity_t p)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (p != DEEPX_DXM1_RESET_ACTIVE_HIGH && p != DEEPX_DXM1_RESET_ACTIVE_LOW) {
        return ALP_ERR_INVAL;
    }
    ctx->reset_polarity = p;
    /* Re-assert reset under the new polarity. */
    return alp_gpio_write(ctx->m1_reset_pin, asserted_level(p));
}

alp_status_t deepx_dxm1_bring_up(deepx_dxm1_t *ctx, uint32_t boot_us)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;

    /* 1. Route the muxes to DEEPX while they are still disabled --
     *    pi3dbs12212_set_state() handles glitch-free transitions, so
     *    we go OFF -> DEEPX_PATH in a single call. */
    alp_status_t s = pi3dbs12212_set_state(ctx->pcie_mux, ctx->deepx_path);
    if (s != ALP_OK) return s;

    /* 2. Release M1_RESET.  The DEEPX silicon's internal boot ROM
     *    starts executing on the rising edge (or falling, depending
     *    on polarity); link training on the PCIe side starts a few
     *    hundred microseconds later. */
    s = alp_gpio_write(ctx->m1_reset_pin, released_level(ctx->reset_polarity));
    if (s != ALP_OK) return s;

    /* 3. Wait for the firmware to come online if the caller asked
     *    for it.  Uses the portable alp_delay_us primitive which
     *    routes to k_busy_wait on Zephyr (cycle-accurate) and a
     *    calibrated portable-C busy-loop on yocto / baremetal. */
    if (boot_us > 0u) {
        alp_delay_us(boot_us);
    }

    return ALP_OK;
}

alp_status_t deepx_dxm1_shut_down(deepx_dxm1_t *ctx)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;

    /* Re-assert reset BEFORE killing the muxes so the PCIe controller
     * sees a clean link-down rather than a fabric-level glitch. */
    alp_status_t s = alp_gpio_write(ctx->m1_reset_pin,
                                    asserted_level(ctx->reset_polarity));
    if (s != ALP_OK) return s;

    s = pi3dbs12212_set_state(ctx->pcie_mux, PI3DBS_STATE_OFF);
    return s;
}

void deepx_dxm1_deinit(deepx_dxm1_t *ctx)
{
    if (ctx == NULL) return;
    if (ctx->initialised) {
        /* Best-effort safe-quiescent. */
        if (ctx->m1_reset_pin != NULL) {
            (void)alp_gpio_write(ctx->m1_reset_pin,
                                 asserted_level(ctx->reset_polarity));
        }
        if (ctx->pcie_mux != NULL) {
            (void)pi3dbs12212_set_state(ctx->pcie_mux, PI3DBS_STATE_OFF);
        }
    }
    ctx->initialised  = false;
    ctx->m1_reset_pin = NULL;
    ctx->pcie_mux     = NULL;
}
