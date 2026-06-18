/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif Ensemble E7 ADC backend.  Routes through Zephyr's adc
 * driver class; the Alif HAL pack registers the SoC's ADC as a
 * DT node and the driver class handles register-level details.
 *
 * Channel resolution: uses the alp-adcN DT aliases (alp-adc0,
 * alp-adc1, ...).  An alp-adcN alias must point at a DT node
 * carrying io-channels + the standard ADC channel-config
 * properties.
 *
 * Also hosts the alp_alif_adc_* vendor-extension bodies, since
 * the vendor knobs reach into per-instance state stored alongside
 * the backend's ops table in this translation unit.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>

#include <alp/adc.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/ext/alif/adc.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "adc_ops.h"

/* DT alias table.  Each alp-adcN alias resolves to an adc_dt_spec;
 * we look it up by channel_id at open time. */
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc0))
#define ALIF_ADC_SPEC_0 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc0))
#else
#define ALIF_ADC_SPEC_0                                                                            \
	{                                                                                              \
		0                                                                                          \
	}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc1))
#define ALIF_ADC_SPEC_1 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc1))
#else
#define ALIF_ADC_SPEC_1                                                                            \
	{                                                                                              \
		0                                                                                          \
	}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc2))
#define ALIF_ADC_SPEC_2 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc2))
#else
#define ALIF_ADC_SPEC_2                                                                            \
	{                                                                                              \
		0                                                                                          \
	}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc3))
#define ALIF_ADC_SPEC_3 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc3))
#else
#define ALIF_ADC_SPEC_3                                                                            \
	{                                                                                              \
		0                                                                                          \
	}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc4))
#define ALIF_ADC_SPEC_4 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc4))
#else
#define ALIF_ADC_SPEC_4                                                                            \
	{                                                                                              \
		0                                                                                          \
	}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc5))
#define ALIF_ADC_SPEC_5 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc5))
#else
#define ALIF_ADC_SPEC_5                                                                            \
	{                                                                                              \
		0                                                                                          \
	}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc6))
#define ALIF_ADC_SPEC_6 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc6))
#else
#define ALIF_ADC_SPEC_6                                                                            \
	{                                                                                              \
		0                                                                                          \
	}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc7))
#define ALIF_ADC_SPEC_7 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc7))
#else
#define ALIF_ADC_SPEC_7                                                                            \
	{                                                                                              \
		0                                                                                          \
	}
#endif

static const struct adc_dt_spec _alif_specs[8] = {
	ALIF_ADC_SPEC_0, ALIF_ADC_SPEC_1, ALIF_ADC_SPEC_2, ALIF_ADC_SPEC_3,
	ALIF_ADC_SPEC_4, ALIF_ADC_SPEC_5, ALIF_ADC_SPEC_6, ALIF_ADC_SPEC_7,
};

typedef struct alif_e7_adc_state {
	const struct adc_dt_spec *spec;
	uint8_t                   channel_id;
	uint8_t                   resolution_bits;
	uint16_t                  oversample_ratio; /* sourced from cfg->oversampling_ratio at open */
	alp_alif_adc_trigger_t    trigger_source;
	int16_t                   sample_buf;
	bool                      in_use;
} alif_e7_adc_state_t;

#ifndef CONFIG_ALP_SDK_ADC_HANDLE_POOL
#define CONFIG_ALP_SDK_ADC_HANDLE_POOL 8
#endif

static alif_e7_adc_state_t _state_pool[CONFIG_ALP_SDK_ADC_HANDLE_POOL];

static alif_e7_adc_state_t *_alloc_state(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_ADC_HANDLE_POOL; ++i) {
		if (!_state_pool[i].in_use) {
			memset(&_state_pool[i], 0, sizeof(_state_pool[i]));
			_state_pool[i].in_use           = true;
			_state_pool[i].oversample_ratio = 1u;
			return &_state_pool[i];
		}
	}
	return NULL;
}

static void _free_state(alif_e7_adc_state_t *s)
{
	s->in_use = false;
}

static alp_status_t
alif_e7_open(const alp_adc_config_t *cfg, alp_adc_backend_state_t *st, alp_capabilities_t *caps_out)
{
	if (cfg->channel_id >= 8u) {
		return ALP_ERR_INVAL;
	}
	if (cfg->resolution_bits != 0 && cfg->resolution_bits > ALP_SOC_ADC_MAX_RESOLUTION_BITS) {
		return ALP_ERR_OUT_OF_RANGE;
	}
	const struct adc_dt_spec *spec = &_alif_specs[cfg->channel_id];
	if (spec->dev == NULL || !device_is_ready(spec->dev)) {
		return ALP_ERR_NOT_READY;
	}
	if (cfg->resolution_bits != 0 && cfg->resolution_bits > spec->resolution) {
		return ALP_ERR_OUT_OF_RANGE;
	}

	alif_e7_adc_state_t *s = _alloc_state();
	if (s == NULL) {
		return ALP_ERR_NOMEM;
	}
	s->spec       = spec;
	s->channel_id = cfg->channel_id;
	s->resolution_bits =
	    (cfg->resolution_bits != 0) ? cfg->resolution_bits : (uint8_t)spec->resolution;
	/* HW oversampling reaches us via the portable config field (Zephyr's
     * adc_sequence.oversampling already abstracts this).  Vendor-ext
     * for oversampling would be redundant -- it's promoted to portable. */
	s->oversample_ratio = (cfg->oversampling_ratio > 1u) ? cfg->oversampling_ratio : 1u;

	int err = adc_channel_setup_dt(spec);
	if (err != 0) {
		_free_state(s);
		return ALP_ERR_IO;
	}

	st->be_data         = s;
	st->reference_uv    = (uint32_t)spec->vref_mv * 1000u;
	st->resolution_bits = s->resolution_bits;

	caps_out->max_resolution_bits = ALP_SOC_ADC_MAX_RESOLUTION_BITS;
	caps_out->max_sample_rate     = 0u; /* not advertised at v0.7 */
	caps_out->channel_count       = ALP_SOC_ADC_COUNT;
	return ALP_OK;
}

static alp_status_t alif_e7_read_raw(alp_adc_backend_state_t *st, int32_t *raw_out)
{
	alif_e7_adc_state_t *s = (alif_e7_adc_state_t *)st->be_data;

	/* Same shared adc_alif driver as the E8 sibling: it UNCONDITIONALLY
	 * dereferences sequence->options->user_data (adc_alif.c:728) and rejects a
	 * non-zero adc_sequence.resolution with -ENOTSUP (:755).  So .options MUST be
	 * non-NULL and .resolution MUST be 0 -- otherwise adc_read() NULL-faults
	 * (empty console) or returns -ENOTSUP -> ALP_ERR_IO.  Mirror the E8 fix; the
	 * raw->uV scale uses st->resolution_bits set from the DT channel at open. */
	uint8_t                           cmp_status = 0;
	const struct adc_sequence_options opts       = {
		      .interval_us     = 0,
		      .callback        = NULL,
		      .user_data       = &cmp_status,
		      .extra_samplings = 0,
	};
	struct adc_sequence seq = {
		.options     = &opts,
		.channels    = BIT(s->spec->channel_id),
		.buffer      = &s->sample_buf,
		.buffer_size = sizeof s->sample_buf,
		.resolution  = 0,
		.oversampling =
		    (s->oversample_ratio > 1u) ? (uint8_t)__builtin_ctz(s->oversample_ratio) : 0u,
	};
	int err = adc_read(s->spec->dev, &seq);
	if (err != 0) {
		return ALP_ERR_IO;
	}
	*raw_out = (int32_t)s->sample_buf;
	return ALP_OK;
}

static void alif_e7_close(alp_adc_backend_state_t *st)
{
	if (st->be_data != NULL) {
		_free_state((alif_e7_adc_state_t *)st->be_data);
		st->be_data = NULL;
	}
}

static const alp_adc_ops_t alif_e7_ops = {
	.open     = alif_e7_open,
	.read_raw = alif_e7_read_raw,
	.close    = alif_e7_close,
};

ALP_BACKEND_REGISTER(adc,
                     alif_e7,
                     {
                         .silicon_ref = "alif:ensemble:e7",
                         .vendor      = "alif",
                         .base_caps   = (uint32_t)(ALP_INSTANCE_CAP_HW_OVERSAMPLE |
                                                 ALP_INSTANCE_CAP_HW_TRIGGER),
                         .priority    = 100,
                         .ops         = &alif_e7_ops,
                         .probe       = NULL,
                     });

/* === Vendor-extension bodies === */

alp_status_t alp_alif_adc_set_trigger_source(alp_adc_t *h, alp_alif_adc_trigger_t src)
{
	if (h == NULL) {
		return ALP_ERR_INVAL;
	}
	if (strcmp(h->backend->vendor, "alif") != 0) {
		return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
	}
	if ((unsigned)src > (unsigned)ALP_ALIF_ADC_TRIGGER_EXT_PIN) {
		return ALP_ERR_INVAL;
	}
	alif_e7_adc_state_t *s = (alif_e7_adc_state_t *)h->state.be_data;
	s->trigger_source      = src;
	return ALP_OK;
}
