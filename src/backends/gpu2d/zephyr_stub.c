/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPU2D stub backend.  Wildcard ("*") registration at priority 0:
 * picks up every silicon_ref the build targets so apps that
 * #include <alp/gpu2d.h> link cleanly on every supported SoC.
 *
 * Stub open() returns ALP_OK so the dispatcher hands the caller a
 * real handle -- this keeps the dispatcher's surface-validation
 * pre-checks (NULL pointer, zero dimensions, format range) reachable
 * and preserves the legacy src/zephyr/gpu2d_zephyr.c contract where
 * customers got an ALP_ERR_INVAL on bad surfaces rather than a
 * silent NOSUPPORT.  fill_rect / blit / blend bodies return
 * ALP_ERR_NOT_IMPLEMENTED -- they're only reached after the
 * dispatcher's surface validation passes.
 *
 * Real backends (Alif D/AVE 2D HAL on the AEN family, NXP Vivante
 * GC328 on i.MX 93) land per the tracking issue below with their
 * own silicon-specific entries at higher priority than this
 * wildcard.  V2N has no on-die 2D block so it stays on this stub
 * indefinitely.
 *
 * @par Tracking: github.com/alplabai/alp-sdk/issues/24
 */

#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/gpu2d.h>
#include <alp/peripheral.h>

#include "gpu2d_ops.h"

static alp_status_t stub_open(alp_gpu2d_backend_state_t *state,
                              alp_capabilities_t *caps_out)
{
    (void)state;
    (void)caps_out;
    /* Successful open keeps the dispatcher's INVAL surface validation
     * reachable, matching legacy gpu2d_zephyr.c behaviour where the
     * NOSUPPORT surfaced at fill_rect / blit / blend time AFTER the
     * surface pre-checks ran. */
    return ALP_OK;
}

static alp_status_t stub_fill_rect(alp_gpu2d_backend_state_t *state,
                                   const alp_gpu2d_surface_t *dst,
                                   uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                   uint32_t argb_color)
{
    (void)state;
    (void)dst;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)argb_color;
    return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t stub_blit(alp_gpu2d_backend_state_t *state,
                              const alp_gpu2d_surface_t *src,
                              uint32_t sx, uint32_t sy,
                              const alp_gpu2d_surface_t *dst,
                              uint32_t dx, uint32_t dy,
                              uint32_t w, uint32_t h)
{
    (void)state;
    (void)src;
    (void)sx;
    (void)sy;
    (void)dst;
    (void)dx;
    (void)dy;
    (void)w;
    (void)h;
    return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t stub_blend(alp_gpu2d_backend_state_t *state,
                               const alp_gpu2d_surface_t *src,
                               uint32_t sx, uint32_t sy,
                               const alp_gpu2d_surface_t *dst,
                               uint32_t dx, uint32_t dy,
                               uint32_t w, uint32_t h,
                               alp_gpu2d_blend_mode_t mode)
{
    (void)state;
    (void)src;
    (void)sx;
    (void)sy;
    (void)dst;
    (void)dx;
    (void)dy;
    (void)w;
    (void)h;
    (void)mode;
    return ALP_ERR_NOT_IMPLEMENTED;
}

static const alp_gpu2d_ops_t _ops = {
    .open      = stub_open,
    .fill_rect = stub_fill_rect,
    .blit      = stub_blit,
    .blend     = stub_blend,
    .close     = NULL,
};

ALP_BACKEND_REGISTER(gpu2d, zephyr_stub, {
    .silicon_ref = "*",
    .vendor      = "stub",
    .base_caps   = 0u,
    .priority    = 0,
    .ops         = &_ops,
    .probe       = NULL,
});
