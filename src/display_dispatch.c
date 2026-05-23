/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Display class dispatcher.  Owns the public alp_display_* API
 * surface and routes through the backend registry.
 *
 * The handle struct layout (struct alp_display) lives in
 * src/backends/display/display_ops.h so per-backend .c files can
 * reach the fields without duplicating the layout.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/display.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "backends/display/display_ops.h"

ALP_BACKEND_DEFINE_CLASS(display);

/* Reuse the existing TLS-backed last-error mechanism from
 * src/zephyr/last_error.c.  Forward-declared here to avoid pulling
 * in the broader handles.h header (which carries unrelated
 * peripheral pool declarations the dispatcher does not touch). */
extern void alp_z_set_last_error(alp_status_t s);
extern void alp_z_clear_last_error(void);

#ifndef CONFIG_ALP_SDK_MAX_DISPLAY_HANDLES
#define CONFIG_ALP_SDK_MAX_DISPLAY_HANDLES 2
#endif

static struct alp_display _pool[CONFIG_ALP_SDK_MAX_DISPLAY_HANDLES];

static struct alp_display *_alloc(void)
{
    for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_DISPLAY_HANDLES; ++i) {
        if (!_pool[i].in_use) {
            memset(&_pool[i], 0, sizeof(_pool[i]));
            _pool[i].in_use = true;
            return &_pool[i];
        }
    }
    return NULL;
}

static void _free(struct alp_display *h)
{
    h->in_use = false;
}

alp_display_t *alp_display_open(const alp_display_config_t *cfg)
{
    alp_z_clear_last_error();
    if (cfg == NULL) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    const alp_backend_t *be = alp_backend_select("display", ALP_SOC_REF_STR);
    if (be == NULL) {
        alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
        return NULL;
    }
    const alp_display_ops_t *ops = (const alp_display_ops_t *)be->ops;
    if (ops == NULL || ops->open == NULL) {
        alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
        return NULL;
    }
    struct alp_display *h = _alloc();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    h->backend = be;
    h->state.ops = ops;
    h->state.display_id = cfg->display_id;
    alp_capabilities_t caps = { .flags = be->base_caps };
    alp_status_t rc = ops->open(cfg, &h->state, &caps);
    if (rc != ALP_OK) {
        _free(h);
        alp_z_set_last_error(rc);
        return NULL;
    }
    h->cached_caps = caps;
    return h;
}

alp_status_t alp_display_get_caps(alp_display_t *h, alp_display_caps_t *out)
{
    if (out == NULL) {
        return ALP_ERR_INVAL;
    }
    if (h == NULL || !h->in_use) {
        return ALP_ERR_NOT_READY;
    }
    return h->state.ops->get_caps(&h->state, out);
}

alp_status_t alp_display_blit(alp_display_t *h,
                              uint16_t x, uint16_t y,
                              uint16_t w, uint16_t h_rect,
                              const void *pixels)
{
    if (h == NULL || !h->in_use) {
        return ALP_ERR_NOT_READY;
    }
    if (pixels == NULL) {
        return ALP_ERR_INVAL;
    }
    return h->state.ops->blit(&h->state, x, y, w, h_rect, pixels);
}

alp_status_t alp_display_clear(alp_display_t *h)
{
    if (h == NULL || !h->in_use) {
        return ALP_ERR_NOT_READY;
    }
    return h->state.ops->clear(&h->state);
}

void alp_display_close(alp_display_t *h)
{
    if (h == NULL || !h->in_use) {
        return;
    }
    if (h->state.ops != NULL && h->state.ops->close != NULL) {
        h->state.ops->close(&h->state);
    }
    _free(h);
}

const alp_capabilities_t *alp_display_capabilities(const alp_display_t *h)
{
    return (h != NULL) ? &h->cached_caps : NULL;
}
