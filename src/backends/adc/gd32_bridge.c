/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * V2N ADC backend.  V2N's M33 has no direct ADC; the SDK routes
 * ADC commands through the GD32G553 supervisor MCU using the
 * existing alp_z_v2n_supervisor_* mutex and the gd32g553_adc_*
 * host driver functions.
 *
 * The bridge's CMD_ADC_READ replies are already mV-corrected
 * (the bridge runs its own raw -> mV conversion using its known
 * 3.3 V reference + 12-bit ADC).  The dispatcher's portable
 * read_uv multiplies raw * reference_uv / fs to get microvolts.
 * To make that math collapse to "mV * 1000", this backend reports
 * raw == mV at 16-bit resolution with a 65535 mV reference, so:
 *
 *     uv = mV * 65535000 / 65535 = mV * 1000
 *
 * Customers calling alp_adc_read_raw on V2N get a value in mV
 * (16-bit-encoded), which is the same contract the legacy
 * peripheral_adc.c presented.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/adc.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "adc_ops.h"

/* Internal SDK headers — NOT customer-facing.  Provide:
 *   alp_z_v2n_supervisor_acquire / _release  (via v2n_supervisor.h)
 *   gd32g553_adc_configure / gd32g553_adc_read  (via chips/gd32g553.h)
 */
#include "v2n_supervisor.h"
#include <alp/chips/gd32g553.h>

typedef struct gd32_bridge_state {
    uint8_t channel_id;
    bool    in_use;
} gd32_bridge_state_t;

#ifndef CONFIG_ALP_SDK_ADC_HANDLE_POOL
#define CONFIG_ALP_SDK_ADC_HANDLE_POOL 8
#endif

static gd32_bridge_state_t _state_pool[CONFIG_ALP_SDK_ADC_HANDLE_POOL];

static gd32_bridge_state_t *_alloc_state(void)
{
    for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_ADC_HANDLE_POOL; ++i) {
        if (!_state_pool[i].in_use) {
            memset(&_state_pool[i], 0, sizeof(_state_pool[i]));
            _state_pool[i].in_use = true;
            return &_state_pool[i];
        }
    }
    return NULL;
}

static void _free_state(gd32_bridge_state_t *s) { s->in_use = false; }

static alp_status_t gd32_open(const alp_adc_config_t *cfg,
                              alp_adc_backend_state_t *st,
                              alp_capabilities_t *caps_out)
{
    /* E1M spec reserves 8 ADC channels; the bridge advertises
     * exactly that many in gd32-io-mcu-map.tsv. */
    if (cfg->channel_id >= 8u) {
        return ALP_ERR_INVAL;
    }

    /* Probe the supervisor up-front + push any provided tuning knobs
     * before returning the handle. */
    gd32g553_t *ctx = NULL;
    alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);
    if (s != ALP_OK) {
        return s;
    }
    if (cfg->oversampling_ratio != 0u ||
        cfg->sample_cycles != 0u ||
        cfg->resolution_bits != 0u) {
        s = gd32g553_adc_configure(ctx, (uint8_t)cfg->channel_id,
                                   cfg->oversampling_ratio,
                                   cfg->sample_cycles,
                                   cfg->resolution_bits);
        if (s != ALP_OK) {
            alp_z_v2n_supervisor_release();
            return s;
        }
    }
    alp_z_v2n_supervisor_release();

    gd32_bridge_state_t *bs = _alloc_state();
    if (bs == NULL) {
        return ALP_ERR_NOMEM;
    }
    bs->channel_id = (uint8_t)cfg->channel_id;

    st->be_data         = bs;
    /* raw is mV; fs = 65535; reference_uv = 65535000 -> uv = mV*1000. */
    st->reference_uv    = 65535000u;
    st->resolution_bits = 16u;

    caps_out->max_resolution_bits = 12u;     /* the SoC actually delivers 12-bit */
    caps_out->max_sample_rate     = 0u;      /* not advertised at v0.7 */
    caps_out->channel_count       = 8u;
    return ALP_OK;
}

static alp_status_t gd32_read_raw(alp_adc_backend_state_t *st,
                                  int32_t *raw_out)
{
    gd32_bridge_state_t *bs = (gd32_bridge_state_t *)st->be_data;

    gd32g553_t *ctx = NULL;
    alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);
    if (s != ALP_OK) {
        return s;
    }
    uint16_t mv = 0;
    s = gd32g553_adc_read(ctx, bs->channel_id, 1u, &mv);
    alp_z_v2n_supervisor_release();
    if (s != ALP_OK) {
        return s;
    }
    *raw_out = (int32_t)mv;
    return ALP_OK;
}

static void gd32_close(alp_adc_backend_state_t *st)
{
    if (st->be_data != NULL) {
        _free_state((gd32_bridge_state_t *)st->be_data);
        st->be_data = NULL;
    }
}

static const alp_adc_ops_t gd32_ops = {
    .open     = gd32_open,
    .read_raw = gd32_read_raw,
    .close    = gd32_close,
};

ALP_BACKEND_REGISTER(adc, gd32_bridge, {
    .silicon_ref = "renesas:rzv2n:n44",
    .vendor      = "renesas",       /* SoC vendor, not bridge chip */
    .base_caps   = 0u,              /* no HW oversample/trigger via bridge in v0.7 */
    .priority    = 100,
    .ops         = &gd32_ops,
    .probe       = NULL,
});
