/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif Ensemble E8 ADC backend.  Routes through Zephyr's adc
 * driver class; the alp-sdk vendored "alif,adc" driver
 * (zephyr/drivers/adc/adc_alif.c, ADR 0017 Tier-2) registers each
 * E8 ADC (12-bit x3 + 24-bit x1) as a DT node and handles the
 * register-level details.
 *
 * Mirrors src/backends/adc/alif_e7.c: the portable surface is
 * identical (the E8 ADC IP is the same Ensemble analog block as the
 * E7, just with the E8's instance count / NPU-side neighbours), so
 * the open / read / close bodies are shared in spirit.  This sibling
 * exists so the SoC-keyed backend registry resolves the correct
 * silicon_ref ("alif:ensemble:e8") at link time.
 *
 * Channel resolution: uses the alp-adcN DT aliases (alp-adc0,
 * alp-adc1, ...).  An alp-adcN alias must point at an okay'd
 * "alif,adc" node carrying io-channels + the standard ADC
 * channel-config properties.
 *
 * Also hosts the alp_alif_adc_* vendor-extension bodies, since the
 * vendor knobs reach into per-instance state stored alongside the
 * backend's ops table in this translation unit.
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
#define ALIF_ADC_SPEC_0 { 0 }
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc1))
#define ALIF_ADC_SPEC_1 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc1))
#else
#define ALIF_ADC_SPEC_1 { 0 }
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc2))
#define ALIF_ADC_SPEC_2 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc2))
#else
#define ALIF_ADC_SPEC_2 { 0 }
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc3))
#define ALIF_ADC_SPEC_3 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc3))
#else
#define ALIF_ADC_SPEC_3 { 0 }
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc4))
#define ALIF_ADC_SPEC_4 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc4))
#else
#define ALIF_ADC_SPEC_4 { 0 }
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc5))
#define ALIF_ADC_SPEC_5 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc5))
#else
#define ALIF_ADC_SPEC_5 { 0 }
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc6))
#define ALIF_ADC_SPEC_6 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc6))
#else
#define ALIF_ADC_SPEC_6 { 0 }
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc7))
#define ALIF_ADC_SPEC_7 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc7))
#else
#define ALIF_ADC_SPEC_7 { 0 }
#endif

static const struct adc_dt_spec _alif_specs[8] = {
	ALIF_ADC_SPEC_0, ALIF_ADC_SPEC_1, ALIF_ADC_SPEC_2, ALIF_ADC_SPEC_3,
	ALIF_ADC_SPEC_4, ALIF_ADC_SPEC_5, ALIF_ADC_SPEC_6, ALIF_ADC_SPEC_7,
};

typedef struct alif_e8_adc_state {
	const struct adc_dt_spec *spec;
	uint8_t                   channel_id;
	uint8_t                   resolution_bits;
	uint16_t                  oversample_ratio; /* sourced from cfg->oversampling_ratio at open */
	alp_alif_adc_trigger_t    trigger_source;
	/* 32-bit sample word: the Alif adc_alif driver's check_buffer_size() requires
	 * 4 bytes per channel (it stores a full 24-bit-capable sample), and rejects a
	 * 2-byte buffer with -ENOMEM. A 16-bit buffer here makes adc_read() fail -> the
	 * read returned ALP_ERR_IO on E8 until this was widened. */
	int32_t sample_buf;
	bool    in_use;
} alif_e8_adc_state_t;

#ifndef CONFIG_ALP_SDK_ADC_HANDLE_POOL
#define CONFIG_ALP_SDK_ADC_HANDLE_POOL 8
#endif

static alif_e8_adc_state_t _state_pool[CONFIG_ALP_SDK_ADC_HANDLE_POOL];

static alif_e8_adc_state_t *_alloc_state(void)
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

static void _free_state(alif_e8_adc_state_t *s)
{
	s->in_use = false;
}

static alp_status_t
alif_e8_open(const alp_adc_config_t *cfg, alp_adc_backend_state_t *st, alp_capabilities_t *caps_out)
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

	alif_e8_adc_state_t *s = _alloc_state();
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

static alp_status_t alif_e8_read_raw(alp_adc_backend_state_t *st, int32_t *raw_out)
{
	alif_e8_adc_state_t *s = (alif_e8_adc_state_t *)st->be_data;

	/* The Alif adc_alif driver UNCONDITIONALLY dereferences
	 * sequence->options->user_data in adc_start_read() (adc_alif.c:728:
	 * `data->comparator = sequence->options->user_data;` -- no NULL guard,
	 * unlike check_buffer_size() at :683 which does test `sequence->options`).
	 * So adc_read() with .options == NULL faults on a NULL deref before the
	 * conversion ever runs -- which is exactly the early/empty-console fault the
	 * <alp/*> loopback hit while the raw-API aen-adc-regcheck (which passes a
	 * non-NULL options carrying a valid user_data, see its main.c) PASSed.
	 * Pass a non-NULL options with a stack-byte user_data to satisfy the driver;
	 * the comparator pointer is only read back by the driver's compare path,
	 * which this single-shot read does not arm. */
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
		/* MUST be 0: the Alif adc_alif driver programs resolution from the DT
		 * channel config and returns -ENOTSUP for a non-zero adc_sequence.resolution
		 * (read returned ALP_ERR_IO on E8 until this was zeroed).  The raw->uV scale
		 * still uses st->resolution_bits, set from the channel at open. */
		.resolution = 0,
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

static void alif_e8_close(alp_adc_backend_state_t *st)
{
	if (st->be_data != NULL) {
		_free_state((alif_e8_adc_state_t *)st->be_data);
		st->be_data = NULL;
	}
}

static const alp_adc_ops_t alif_e8_ops = {
	.open     = alif_e8_open,
	.read_raw = alif_e8_read_raw,
	.close    = alif_e8_close,
};

ALP_BACKEND_REGISTER(adc,
                     alif_e8,
                     {
                         .silicon_ref = "alif:ensemble:e8",
                         .vendor      = "alif",
                         .base_caps   = (uint32_t)(ALP_INSTANCE_CAP_HW_OVERSAMPLE |
                                                   ALP_INSTANCE_CAP_HW_TRIGGER),
                         .priority    = 100,
                         .ops         = &alif_e8_ops,
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
	alif_e8_adc_state_t *s = (alif_e8_adc_state_t *)h->state.be_data;
	/* NOTE: the source is RECORDED but the current read path (alif_e8_read_raw)
	 * is software-triggered single-shot -- it does not yet wire an external HW
	 * trigger, so a non-software src is stored for a future trigger-aware read
	 * path, not applied now. Tracked for the HW-trigger follow-up (#21). */
	s->trigger_source = src;
	return ALP_OK;
}
