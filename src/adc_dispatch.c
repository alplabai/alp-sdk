/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ADC class dispatcher.  Owns the public alp_adc_* API surface
 * and routes through the backend registry mechanism shipped in
 * Slice 0 (PR #17).  All voltage-conversion math (raw -> uV)
 * lives here so every backend reports raw values in the same
 * convention.
 *
 * The handle struct layout (struct alp_adc) lives in
 * src/backends/adc/adc_ops.h so vendor-ext backend .c files can
 * reach the fields directly without duplicating the layout.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/adc.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "backends/adc/adc_ops.h"

ALP_BACKEND_DEFINE_CLASS(adc);

/* Reuse the existing TLS-backed last-error mechanism from
 * src/zephyr/last_error.c.  Declared in src/zephyr/handles.h but
 * we forward-declare here to avoid pulling in the broader handles
 * header (which carries unrelated peripheral declarations). */
extern void alp_z_set_last_error(alp_status_t s);

#ifndef CONFIG_ALP_SDK_ADC_HANDLE_POOL
#define CONFIG_ALP_SDK_ADC_HANDLE_POOL 8
#endif

static struct alp_adc _pool[CONFIG_ALP_SDK_ADC_HANDLE_POOL];

static struct alp_adc *_alloc_handle(void)
{
    for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_ADC_HANDLE_POOL; ++i) {
        if (!_pool[i].in_use) {
            memset(&_pool[i], 0, sizeof(_pool[i]));
            _pool[i].in_use = true;
            return &_pool[i];
        }
    }
    return NULL;
}

static void _free_handle(struct alp_adc *h)
{
    h->in_use = false;
}

alp_adc_t *alp_adc_open(const alp_adc_config_t *cfg)
{
    if (cfg == NULL) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    const alp_backend_t *be = alp_backend_select("adc", ALP_SOC_REF_STR);
    if (be == NULL) {
        alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
        return NULL;
    }
    const alp_adc_ops_t *ops = (const alp_adc_ops_t *)be->ops;
    if (ops == NULL || ops->open == NULL) {
        alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
        return NULL;
    }
    struct alp_adc *h = _alloc_handle();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    h->backend = be;
    h->state.ops = ops;
    alp_capabilities_t caps = { .flags = be->base_caps };
    if (be->probe != NULL) {
        uint32_t refined = caps.flags;
        (void)be->probe(cfg->channel_id, &refined);
        caps.flags = refined;
    }
    alp_status_t rc = ops->open(cfg, &h->state, &caps);
    if (rc != ALP_OK) {
        _free_handle(h);
        alp_z_set_last_error(rc);
        return NULL;
    }
    h->cached_caps = caps;
    return h;
}

alp_status_t alp_adc_read_raw(alp_adc_t *h, int32_t *raw_out)
{
    if (h == NULL || raw_out == NULL) {
        return ALP_ERR_INVAL;
    }
    return h->state.ops->read_raw(&h->state, raw_out);
}

alp_status_t alp_adc_read_uv(alp_adc_t *h, int32_t *uv_out)
{
    if (h == NULL || uv_out == NULL) {
        return ALP_ERR_INVAL;
    }
    int32_t raw = 0;
    alp_status_t rc = h->state.ops->read_raw(&h->state, &raw);
    if (rc != ALP_OK) {
        return rc;
    }
    const int64_t fs = (int64_t)((1u << h->state.resolution_bits) - 1u);
    if (fs == 0) {
        return ALP_ERR_NOT_READY;
    }
    *uv_out = (int32_t)((int64_t)raw * (int64_t)h->state.reference_uv / fs);
    return ALP_OK;
}

void alp_adc_close(alp_adc_t *h)
{
    if (h == NULL) {
        return;
    }
    if (h->state.ops != NULL && h->state.ops->close != NULL) {
        h->state.ops->close(&h->state);
    }
    _free_handle(h);
}

const alp_capabilities_t *alp_adc_capabilities(const alp_adc_t *h)
{
    return (h != NULL) ? &h->cached_caps : NULL;
}
