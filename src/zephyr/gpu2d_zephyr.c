/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/gpu2d.h>.  v0.5 ships NOSUPPORT stubs
 * with INVAL pre-checks so the public surface is link-resolvable
 * on every Zephyr build, including SoMs without a 2D accelerator.
 *
 * The real implementations land per-SoC once the vendor HAL packs
 * stabilise:
 *
 *   - AEN-family : Alif `alif_dave2d-driver` (D/AVE 2D HAL).  The
 *     vendor pack is the gating dep -- when it lands as a Zephyr
 *     module, the dispatch here grows a per-SoC switch on
 *     CONFIG_ALP_SOC_ALIF_ENSEMBLE_E[3-8].
 *   - i.MX 93    : Vivante GC328 (NXP BSP, planned).
 *   - V2N        : no 2D block; stays NOSUPPORT.
 *
 * Per the AEN audit (docs/aen-feature-audit-2026-05.md), GPU2D
 * is the highest-demand peripheral-class gap.  Customers
 * migrating from V2N to AEN silently lose 2D acceleration if the
 * SDK never exposed a portable surface -- this header + stub
 * pair fixes that, giving the eventual HAL impl a stable place
 * to hook in.
 */

#include "alp/gpu2d.h"
#include "alp/peripheral.h"
#include "handles.h"

struct alp_gpu2d {
    bool in_use;
};

static struct alp_gpu2d g_alp_gpu2d;

alp_gpu2d_t *alp_gpu2d_open(void)
{
    alp_z_clear_last_error();
    /* No vendor HAL wired yet on any active SoM. */
    alp_z_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
}

static alp_status_t validate_surface(const alp_gpu2d_surface_t *s)
{
    if (s == NULL) return ALP_ERR_INVAL;
    if (s->base == NULL || s->width == 0u || s->height == 0u || s->stride_bytes == 0u) {
        return ALP_ERR_INVAL;
    }
    if ((unsigned)s->format > (unsigned)ALP_GPU2D_FMT_RGBA8888) return ALP_ERR_INVAL;
    return ALP_OK;
}

alp_status_t alp_gpu2d_fill_rect(alp_gpu2d_t *handle, const alp_gpu2d_surface_t *dst, uint32_t x,
                                 uint32_t y, uint32_t w, uint32_t h, uint32_t argb_color)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)argb_color;
    if (handle == NULL) return ALP_ERR_NOT_READY;
    const alp_status_t s = validate_surface(dst);
    if (s != ALP_OK) return s;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_gpu2d_blit(alp_gpu2d_t *handle, const alp_gpu2d_surface_t *src, uint32_t sx,
                            uint32_t sy, const alp_gpu2d_surface_t *dst, uint32_t dx, uint32_t dy,
                            uint32_t w, uint32_t h)
{
    (void)sx;
    (void)sy;
    (void)dx;
    (void)dy;
    (void)w;
    (void)h;
    if (handle == NULL) return ALP_ERR_NOT_READY;
    alp_status_t s = validate_surface(src);
    if (s != ALP_OK) return s;
    s = validate_surface(dst);
    if (s != ALP_OK) return s;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_gpu2d_blend(alp_gpu2d_t *handle, const alp_gpu2d_surface_t *src, uint32_t sx,
                             uint32_t sy, const alp_gpu2d_surface_t *dst, uint32_t dx, uint32_t dy,
                             uint32_t w, uint32_t h, alp_gpu2d_blend_mode_t mode)
{
    (void)sx;
    (void)sy;
    (void)dx;
    (void)dy;
    (void)w;
    (void)h;
    if (handle == NULL) return ALP_ERR_NOT_READY;
    if ((unsigned)mode > (unsigned)ALP_GPU2D_BLEND_MULTIPLY) return ALP_ERR_INVAL;
    alp_status_t s = validate_surface(src);
    if (s != ALP_OK) return s;
    s = validate_surface(dst);
    if (s != ALP_OK) return s;
    return ALP_ERR_NOSUPPORT;
}

void alp_gpu2d_close(alp_gpu2d_t *handle)
{
    if (handle == NULL) return;
    handle->in_use = false;
}
