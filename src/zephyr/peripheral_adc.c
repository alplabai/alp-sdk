/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/adc.h>.
 *
 * Channel resolution.  Each studio-resolved channel_id (0..7) maps to
 * the `alp-adcN` DT alias.  The alias must point at a node with
 * `io-channels` plus the Zephyr ADC channel-config properties
 * (`zephyr,resolution`, `zephyr,reference`, `zephyr,gain`,
 * `zephyr,acquisition-time`):
 *
 *     adc_user0: adc_user_0 {
 *         io-channels = <&adc0 0>;
 *         zephyr,resolution  = <12>;
 *         zephyr,reference   = "ADC_REF_INTERNAL";
 *         zephyr,gain        = "ADC_GAIN_1";
 *         zephyr,acquisition-time = <ADC_ACQ_TIME_DEFAULT>;
 *         zephyr,vref-mv     = <3300>;
 *     };
 *     aliases { alp-adc0 = &adc_user0; };
 *
 * Capability validation.  The active SoC's documented ADC max
 * resolution is exposed by `<alp/soc_caps.h>` as
 * `ALP_SOC_ADC_MAX_RESOLUTION_BITS`.  Asking for a higher resolution
 * than the SoC supports returns NULL with last_error =
 * ALP_ERR_OUT_OF_RANGE — for example, a 16-bit request on an Alif E3
 * (12-bit hardware) is rejected before any I/O.
 *
 * Conditional spec construction.  ADC_DT_SPEC_GET fails to expand
 * when given DT_INVALID_NODE (it interrogates the node's properties
 * unconditionally), so we can't just COND_CODE_1 on alias existence.
 * Per-index #if blocks emit either ADC_DT_SPEC_GET or a NULL spec,
 * and the array is built from those.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/sys/util.h>

#include "alp/adc.h"
#include "alp/soc_caps.h"
#include "handles.h"

#if DT_NODE_EXISTS(DT_ALIAS(alp_adc0))
#define ALP_ADC_SPEC_0_INIT  ADC_DT_SPEC_GET(DT_ALIAS(alp_adc0))
#else
#define ALP_ADC_SPEC_0_INIT  {.dev = NULL}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc1))
#define ALP_ADC_SPEC_1_INIT  ADC_DT_SPEC_GET(DT_ALIAS(alp_adc1))
#else
#define ALP_ADC_SPEC_1_INIT  {.dev = NULL}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc2))
#define ALP_ADC_SPEC_2_INIT  ADC_DT_SPEC_GET(DT_ALIAS(alp_adc2))
#else
#define ALP_ADC_SPEC_2_INIT  {.dev = NULL}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc3))
#define ALP_ADC_SPEC_3_INIT  ADC_DT_SPEC_GET(DT_ALIAS(alp_adc3))
#else
#define ALP_ADC_SPEC_3_INIT  {.dev = NULL}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc4))
#define ALP_ADC_SPEC_4_INIT  ADC_DT_SPEC_GET(DT_ALIAS(alp_adc4))
#else
#define ALP_ADC_SPEC_4_INIT  {.dev = NULL}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc5))
#define ALP_ADC_SPEC_5_INIT  ADC_DT_SPEC_GET(DT_ALIAS(alp_adc5))
#else
#define ALP_ADC_SPEC_5_INIT  {.dev = NULL}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc6))
#define ALP_ADC_SPEC_6_INIT  ADC_DT_SPEC_GET(DT_ALIAS(alp_adc6))
#else
#define ALP_ADC_SPEC_6_INIT  {.dev = NULL}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc7))
#define ALP_ADC_SPEC_7_INIT  ADC_DT_SPEC_GET(DT_ALIAS(alp_adc7))
#else
#define ALP_ADC_SPEC_7_INIT  {.dev = NULL}
#endif

static const struct adc_dt_spec alp_adcs[] = {
    ALP_ADC_SPEC_0_INIT,
    ALP_ADC_SPEC_1_INIT,
    ALP_ADC_SPEC_2_INIT,
    ALP_ADC_SPEC_3_INIT,
    ALP_ADC_SPEC_4_INIT,
    ALP_ADC_SPEC_5_INIT,
    ALP_ADC_SPEC_6_INIT,
    ALP_ADC_SPEC_7_INIT,
};

static alp_status_t errno_to_alp(int err) {
    switch (err) {
    case 0:           return ALP_OK;
    case -EINVAL:     return ALP_ERR_INVAL;
    case -EBUSY:      return ALP_ERR_BUSY;
    case -ENOTSUP:
    case -ENOSYS:     return ALP_ERR_NOSUPPORT;
    default:          return ALP_ERR_IO;
    }
}

alp_adc_t *alp_adc_open(const alp_adc_config_t *cfg) {
    alp_z_clear_last_error();

    if (cfg == NULL) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (cfg->channel_id >= ARRAY_SIZE(alp_adcs)) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

    /* Capability check — reject configs the active SoC's documented
     * hardware can't honour.  This catches the canonical
     * "16-bit ADC requested on a 12-bit SoC" case before any
     * runtime I/O. */
    if (cfg->resolution_bits != 0 &&
        cfg->resolution_bits > ALP_SOC_ADC_MAX_RESOLUTION_BITS) {
        alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
        return NULL;
    }
    if (cfg->channel_id >= ALP_SOC_ADC_COUNT) {
        alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
        return NULL;
    }

    const struct adc_dt_spec *spec = &alp_adcs[cfg->channel_id];
    if (spec->dev == NULL || !device_is_ready(spec->dev)) {
        alp_z_set_last_error(ALP_ERR_NOT_READY);
        return NULL;
    }

    /* Cross-check against the runtime device's DT-declared maximum.
     * The SoC-cap macro is the documented limit; the DT spec is
     * what's actually wired on this board. */
    if (cfg->resolution_bits != 0 &&
        cfg->resolution_bits > spec->resolution) {
        alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
        return NULL;
    }

    struct alp_adc *h = alp_z_adc_pool_acquire();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }

    h->channel_id = cfg->channel_id;
    h->dev        = spec->dev;
    h->channel    = spec->channel_id;
    h->resolution = (cfg->resolution_bits != 0)
                      ? cfg->resolution_bits
                      : (uint8_t)spec->resolution;
    h->vref_mv    = spec->vref_mv;

    int err = adc_channel_setup_dt(spec);
    if (err != 0) {
        alp_z_set_last_error(errno_to_alp(err));
        alp_z_adc_pool_release(h);
        return NULL;
    }
    return h;
}

static alp_status_t one_shot(struct alp_adc *h, int32_t *raw_out) {
    struct adc_sequence seq = {
        .channels    = BIT(h->channel),
        .buffer      = &h->sample_buf,
        .buffer_size = sizeof h->sample_buf,
        .resolution  = h->resolution,
    };
    int err = adc_read(h->dev, &seq);
    if (err != 0) return errno_to_alp(err);
    *raw_out = (int32_t)h->sample_buf;
    return ALP_OK;
}

alp_status_t alp_adc_read_raw(alp_adc_t *adc, int32_t *raw_out) {
    if (adc == NULL || !adc->in_use) return ALP_ERR_NOT_READY;
    if (raw_out == NULL) return ALP_ERR_INVAL;
    return one_shot(adc, raw_out);
}

alp_status_t alp_adc_read_uv(alp_adc_t *adc, int32_t *uv_out) {
    if (adc == NULL || !adc->in_use) return ALP_ERR_NOT_READY;
    if (uv_out == NULL) return ALP_ERR_INVAL;

    int32_t raw = 0;
    alp_status_t s = one_shot(adc, &raw);
    if (s != ALP_OK) return s;

    int32_t mv = raw;
    int err = adc_raw_to_millivolts(adc->vref_mv,
                                    /* gain */ ADC_GAIN_1,
                                    adc->resolution,
                                    &mv);
    if (err != 0) {
        /* Fallback: raw passthrough as a μV proxy. */
        *uv_out = raw;
        return ALP_OK;
    }
    *uv_out = mv * 1000;
    return ALP_OK;
}

void alp_adc_close(alp_adc_t *adc) {
    alp_z_adc_pool_release(adc);
}
