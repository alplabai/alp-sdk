/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPU2D class dispatcher.  Routes the public alp_gpu2d_* API
 * through the backend registry mechanism shipped in Slice 0
 * (PR #17).
 *
 * Surface validation (NULL pointer, zero dimensions, format range)
 * lives here at the dispatcher so every backend gets consistent
 * INVAL pre-checks before the op fires.  Mirrors the legacy
 * src/zephyr/gpu2d_zephyr.c behaviour exactly: invalid surfaces
 * surface as ALP_ERR_INVAL regardless of whether the active backend
 * has a real implementation yet.
 *
 * Handle pool defaults to 1 (CONFIG_ALP_SDK_MAX_GPU2D_HANDLES) --
 * the legacy header documents the GPU2D handle as a "system-wide"
 * singleton (one DAVE2D block per SoC).  Customers needing per-core
 * handles can bump the Kconfig.
 *
 * The handle struct layout (struct alp_gpu2d) lives in
 * src/backends/gpu2d/gpu2d_ops.h so per-backend .c files can reach
 * the fields without duplicating the layout.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/gpu2d.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "backends/gpu2d/gpu2d_ops.h"

ALP_BACKEND_DEFINE_CLASS(gpu2d);

/* Reuse the existing TLS-backed last-error mechanism from
 * src/zephyr/last_error.c.  Forward-declared here to avoid pulling
 * in the broader handles.h header (which carries unrelated
 * peripheral pool declarations the dispatcher does not touch). */
extern void alp_z_set_last_error(alp_status_t s);
extern void alp_z_clear_last_error(void);

#ifndef CONFIG_ALP_SDK_MAX_GPU2D_HANDLES
#define CONFIG_ALP_SDK_MAX_GPU2D_HANDLES 1
#endif

static struct alp_gpu2d _pool[CONFIG_ALP_SDK_MAX_GPU2D_HANDLES];

static struct alp_gpu2d *_alloc(void)
{
    for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_GPU2D_HANDLES; ++i) {
        if (!_pool[i].in_use) {
            memset(&_pool[i], 0, sizeof(_pool[i]));
            _pool[i].in_use = true;
            return &_pool[i];
        }
    }
    return NULL;
}

static void _free(struct alp_gpu2d *h)
{
    h->in_use = false;
}

static alp_status_t _validate_surface(const alp_gpu2d_surface_t *s)
{
    if (s == NULL) {
        return ALP_ERR_INVAL;
    }
    if (s->base == NULL || s->width == 0u || s->height == 0u ||
        s->stride_bytes == 0u) {
        return ALP_ERR_INVAL;
    }
    if ((unsigned)s->format > (unsigned)ALP_GPU2D_FMT_RGBA8888) {
        return ALP_ERR_INVAL;
    }
    return ALP_OK;
}

alp_gpu2d_t *alp_gpu2d_open(void)
{
    alp_z_clear_last_error();
    const alp_backend_t *be = alp_backend_select("gpu2d", ALP_SOC_REF_STR);
    if (be == NULL) {
        alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
        return NULL;
    }
    const alp_gpu2d_ops_t *ops = (const alp_gpu2d_ops_t *)be->ops;
    if (ops == NULL || ops->open == NULL) {
        alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
        return NULL;
    }
    struct alp_gpu2d *h = _alloc();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    h->backend = be;
    h->state.ops = ops;
    alp_capabilities_t caps = { .flags = be->base_caps };
    alp_status_t rc = ops->open(&h->state, &caps);
    if (rc != ALP_OK) {
        _free(h);
        alp_z_set_last_error(rc);
        return NULL;
    }
    h->cached_caps = caps;
    return h;
}

alp_status_t alp_gpu2d_fill_rect(alp_gpu2d_t *h, const alp_gpu2d_surface_t *dst,
                                 uint32_t x, uint32_t y, uint32_t w, uint32_t height,
                                 uint32_t argb_color)
{
    if (h == NULL || !h->in_use) {
        return ALP_ERR_NOT_READY;
    }
    alp_status_t s = _validate_surface(dst);
    if (s != ALP_OK) {
        return s;
    }
    return h->state.ops->fill_rect(&h->state, dst, x, y, w, height, argb_color);
}

alp_status_t alp_gpu2d_blit(alp_gpu2d_t *h, const alp_gpu2d_surface_t *src,
                            uint32_t sx, uint32_t sy, const alp_gpu2d_surface_t *dst,
                            uint32_t dx, uint32_t dy, uint32_t w, uint32_t height)
{
    if (h == NULL || !h->in_use) {
        return ALP_ERR_NOT_READY;
    }
    alp_status_t s = _validate_surface(src);
    if (s != ALP_OK) {
        return s;
    }
    s = _validate_surface(dst);
    if (s != ALP_OK) {
        return s;
    }
    return h->state.ops->blit(&h->state, src, sx, sy, dst, dx, dy, w, height);
}

alp_status_t alp_gpu2d_blend(alp_gpu2d_t *h, const alp_gpu2d_surface_t *src,
                             uint32_t sx, uint32_t sy, const alp_gpu2d_surface_t *dst,
                             uint32_t dx, uint32_t dy, uint32_t w, uint32_t height,
                             alp_gpu2d_blend_mode_t mode)
{
    if (h == NULL || !h->in_use) {
        return ALP_ERR_NOT_READY;
    }
    if ((unsigned)mode > (unsigned)ALP_GPU2D_BLEND_MULTIPLY) {
        return ALP_ERR_INVAL;
    }
    alp_status_t s = _validate_surface(src);
    if (s != ALP_OK) {
        return s;
    }
    s = _validate_surface(dst);
    if (s != ALP_OK) {
        return s;
    }
    return h->state.ops->blend(&h->state, src, sx, sy, dst, dx, dy, w, height, mode);
}

void alp_gpu2d_close(alp_gpu2d_t *h)
{
    if (h == NULL || !h->in_use) {
        return;
    }
    if (h->state.ops != NULL && h->state.ops->close != NULL) {
        h->state.ops->close(&h->state);
    }
    _free(h);
}

const alp_capabilities_t *alp_gpu2d_capabilities(const alp_gpu2d_t *h)
{
    return (h != NULL) ? &h->cached_caps : NULL;
}
