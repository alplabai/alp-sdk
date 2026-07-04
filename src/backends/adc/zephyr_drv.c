/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr adc_* driver-class backend.  Used on any SoC
 * unless a vendor-specific backend (alif_e7 / alif_e8 / gd32_bridge)
 * registers a more specific silicon_ref match at the same priority.
 *
 * Channel resolution follows the alp-sdk ADC convention documented
 * in zephyr/dts/bindings/adc/alp,adc-input.yaml: each alp-adcN DT
 * alias points at an "alp,adc-input" consumer node whose io-channels
 * phandle selects the controller + input; ADC_DT_SPEC_GET on the
 * alias pulls the channel@N config (gain / reference / resolution /
 * acquisition time) that adc_channel_setup_dt programs.  Boards or
 * overlays that don't wire an alias leave the spec slot NULL and
 * open() degrades with ALP_ERR_NOT_READY.
 *
 * This is what makes native_sim's zephyr,adc-emul controller a REAL
 * positive path for the portable ADC surface (see the conformance
 * suite's overlays) -- and any future SoC whose upstream Zephyr ADC
 * driver works out of the box gets the portable API for free.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/sys/util.h>

#include <alp/adc.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "adc_ops.h"

/* DT alias table.  Each alp-adcN alias resolves to an adc_dt_spec;
 * unwired aliases collapse to a zeroed spec (dev == NULL). */
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc0))
#define ALP_Z_ADC_SPEC_0 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc0))
#else
#define ALP_Z_ADC_SPEC_0 { 0 }
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc1))
#define ALP_Z_ADC_SPEC_1 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc1))
#else
#define ALP_Z_ADC_SPEC_1 { 0 }
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc2))
#define ALP_Z_ADC_SPEC_2 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc2))
#else
#define ALP_Z_ADC_SPEC_2 { 0 }
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc3))
#define ALP_Z_ADC_SPEC_3 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc3))
#else
#define ALP_Z_ADC_SPEC_3 { 0 }
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc4))
#define ALP_Z_ADC_SPEC_4 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc4))
#else
#define ALP_Z_ADC_SPEC_4 { 0 }
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc5))
#define ALP_Z_ADC_SPEC_5 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc5))
#else
#define ALP_Z_ADC_SPEC_5 { 0 }
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc6))
#define ALP_Z_ADC_SPEC_6 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc6))
#else
#define ALP_Z_ADC_SPEC_6 { 0 }
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc7))
#define ALP_Z_ADC_SPEC_7 ADC_DT_SPEC_GET(DT_ALIAS(alp_adc7))
#else
#define ALP_Z_ADC_SPEC_7 { 0 }
#endif

static const struct adc_dt_spec _specs[8] = {
	ALP_Z_ADC_SPEC_0, ALP_Z_ADC_SPEC_1, ALP_Z_ADC_SPEC_2, ALP_Z_ADC_SPEC_3,
	ALP_Z_ADC_SPEC_4, ALP_Z_ADC_SPEC_5, ALP_Z_ADC_SPEC_6, ALP_Z_ADC_SPEC_7,
};

typedef struct alp_z_adc_state {
	const struct adc_dt_spec *spec;
	uint8_t                   resolution_bits;
	/* 32-bit sample word: wide enough for every Zephyr ADC driver's
	 * per-channel sample slot (some vendor drivers -- e.g. the Alif
	 * adc_alif -- require 4 bytes per channel and reject a 2-byte
	 * buffer with -ENOMEM; see src/backends/adc/alif_e8.c). */
	int32_t sample_buf;
	bool    in_use;
} alp_z_adc_state_t;

#ifndef CONFIG_ALP_SDK_ADC_HANDLE_POOL
#define CONFIG_ALP_SDK_ADC_HANDLE_POOL 8
#endif

static alp_z_adc_state_t _state_pool[CONFIG_ALP_SDK_ADC_HANDLE_POOL];

static alp_z_adc_state_t *_alloc_state(void)
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

static void _free_state(alp_z_adc_state_t *s)
{
	s->in_use = false;
}

static alp_status_t
z_open(const alp_adc_config_t *cfg, alp_adc_backend_state_t *st, alp_capabilities_t *caps_out)
{
	if (cfg->channel_id >= ARRAY_SIZE(_specs)) {
		return ALP_ERR_INVAL;
	}
	if (cfg->resolution_bits != 0 && cfg->resolution_bits > ALP_SOC_ADC_MAX_RESOLUTION_BITS) {
		return ALP_ERR_OUT_OF_RANGE;
	}
	const struct adc_dt_spec *spec = &_specs[cfg->channel_id];
	if (spec->dev == NULL || !device_is_ready(spec->dev)) {
		return ALP_ERR_NOT_READY;
	}
	if (cfg->resolution_bits != 0 && cfg->resolution_bits > spec->resolution) {
		return ALP_ERR_OUT_OF_RANGE;
	}

	alp_z_adc_state_t *s = _alloc_state();
	if (s == NULL) {
		return ALP_ERR_NOMEM;
	}
	s->spec = spec;
	s->resolution_bits =
	    (cfg->resolution_bits != 0) ? cfg->resolution_bits : (uint8_t)spec->resolution;

	int err = adc_channel_setup_dt(spec);
	if (err != 0) {
		_free_state(s);
		return ALP_ERR_IO;
	}

	st->be_data = s;
	/* vref_mv comes from the channel@N config when set; fall back to
	 * the controller's internal reference so read_uv() scales sanely. */
	st->reference_uv    = (spec->vref_mv != 0) ? (uint32_t)spec->vref_mv * 1000u
	                                           : (uint32_t)adc_ref_internal(spec->dev) * 1000u;
	st->resolution_bits = s->resolution_bits;

	caps_out->max_resolution_bits = ALP_SOC_ADC_MAX_RESOLUTION_BITS;
	caps_out->max_sample_rate     = 0u; /* not advertised by the generic backend */
	caps_out->channel_count       = ALP_SOC_ADC_COUNT;
	return ALP_OK;
}

static alp_status_t z_read_raw(alp_adc_backend_state_t *st, int32_t *raw_out)
{
	alp_z_adc_state_t *s = (alp_z_adc_state_t *)st->be_data;

	/* Non-NULL options with a valid user_data: harmless on upstream
	 * drivers, and REQUIRED on some vendor drivers (adc_alif derefs
	 * sequence->options->user_data with no NULL guard -- see the
	 * fault post-mortem in src/backends/adc/alif_e8.c). */
	uint8_t                           scratch = 0;
	const struct adc_sequence_options opts    = {
		.interval_us     = 0,
		.callback        = NULL,
		.user_data       = &scratch,
		.extra_samplings = 0,
	};
	struct adc_sequence seq = {
		.options      = &opts,
		.channels     = BIT(s->spec->channel_id),
		.buffer       = &s->sample_buf,
		.buffer_size  = sizeof s->sample_buf,
		.resolution   = s->resolution_bits,
		.oversampling = 0,
	};
	int err = adc_read(s->spec->dev, &seq);
	if (err != 0) {
		return ALP_ERR_IO;
	}
	/* Drivers fill the buffer at their native sample width; a
	 * conversion of 16 bits or less lands in the low half-word
	 * (little-endian on every supported target). */
	if (s->resolution_bits <= 16u) {
		int16_t half;
		memcpy(&half, &s->sample_buf, sizeof(half));
		*raw_out = (int32_t)half;
	} else {
		*raw_out = s->sample_buf;
	}
	return ALP_OK;
}

static void z_close(alp_adc_backend_state_t *st)
{
	if (st->be_data != NULL) {
		_free_state((alp_z_adc_state_t *)st->be_data);
		st->be_data = NULL;
	}
}

static const alp_adc_ops_t _ops = {
	.open     = z_open,
	.read_raw = z_read_raw,
	.close    = z_close,
};

ALP_BACKEND_REGISTER(adc,
                     zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
