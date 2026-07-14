/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * ADC (+ stream/filter/spectrum composition layer) NOSUPPORT stubs --
 * <alp/adc.h>.  Split out of the former src/common/stub_backend.c
 * monolith (issue #673); owns every `alp_adc_*` symbol not provided
 * by a vendor backend.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/adc.h"
#include "alp/peripheral.h"

#include "stub_internal.h"

#if !defined(ALP_VENDOR_OVERRIDES_ADC)
alp_adc_t *alp_adc_open(const alp_adc_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_adc_read_raw(alp_adc_t *a, int32_t *r)
{
	(void)a;
	(void)r;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_adc_read_uv(alp_adc_t *a, int32_t *u)
{
	(void)a;
	(void)u;
	return ALP_ERR_NOSUPPORT;
}
void alp_adc_close(alp_adc_t *a)
{
	(void)a;
}
const alp_capabilities_t *alp_adc_capabilities(const alp_adc_t *a)
{
	(void)a;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_ADC */

/* ADC filter/stream/spectrum (alp/adc.h v0.5.x composition layer) --
 * unguarded (no ALP_VENDOR_OVERRIDES_ADC mute): unlike one-shot ADC
 * open/read/close, this trio has no dispatcher-owned implementation
 * anywhere -- the only real body is Zephyr-only (V2N GD32 supervisor
 * DMA-backed stream slots, src/zephyr/peripheral_adc.c), so Yocto's
 * adc_dispatch.c (which mutes the block above) leaves these three
 * undefined without a stub of their own (#593). */
alp_adc_stream_t *alp_adc_stream_open(const alp_adc_stream_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_adc_stream_read_mv(alp_adc_stream_t *stream, uint16_t *mv, size_t cap, size_t *got)
{
	(void)stream;
	(void)mv;
	(void)cap;
	if (got != NULL) *got = 0;
	return ALP_ERR_NOSUPPORT;
}
void alp_adc_stream_close(alp_adc_stream_t *stream)
{
	(void)stream;
}
alp_adc_filter_t *alp_adc_filter_open(const alp_adc_filter_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t
alp_adc_filter_read_mv(alp_adc_filter_t *filter, int16_t *out_mv, size_t cap, size_t *got)
{
	(void)filter;
	(void)out_mv;
	(void)cap;
	if (got != NULL) *got = 0;
	return ALP_ERR_NOSUPPORT;
}
void alp_adc_filter_close(alp_adc_filter_t *filter)
{
	(void)filter;
}
alp_adc_spectrum_t *alp_adc_spectrum_open(const alp_adc_spectrum_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t
alp_adc_spectrum_read_bins(alp_adc_spectrum_t *spec, float *bins, size_t cap, size_t *got)
{
	(void)spec;
	(void)bins;
	(void)cap;
	if (got != NULL) *got = 0;
	return ALP_ERR_NOSUPPORT;
}
void alp_adc_spectrum_close(alp_adc_spectrum_t *spec)
{
	(void)spec;
}
