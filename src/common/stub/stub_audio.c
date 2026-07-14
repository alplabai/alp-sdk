/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Audio in/out NOSUPPORT stubs -- <alp/audio.h>.  Split out of the
 * former src/common/stub_backend.c monolith (issue #673); owns every
 * `alp_audio_in_*` / `alp_audio_out_*` symbol not provided by a
 * vendor backend.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/audio.h"
#include "alp/peripheral.h"

#include "stub_internal.h"

#if !defined(ALP_VENDOR_OVERRIDES_AUDIO_IN)
alp_audio_in_t *alp_audio_in_open(const alp_audio_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_audio_in_start(alp_audio_in_t *i)
{
	(void)i;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_audio_in_stop(alp_audio_in_t *i)
{
	(void)i;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_audio_in_read(alp_audio_in_t *i, void *b, size_t f, size_t *o, uint32_t t)
{
	(void)i;
	(void)b;
	(void)f;
	(void)t;
	if (o != NULL) *o = 0;
	return ALP_ERR_NOSUPPORT;
}
void alp_audio_in_close(alp_audio_in_t *i)
{
	(void)i;
}
const alp_capabilities_t *alp_audio_in_capabilities(const alp_audio_in_t *i)
{
	(void)i;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_AUDIO_IN */

#if !defined(ALP_VENDOR_OVERRIDES_AUDIO_OUT)
alp_audio_out_t *alp_audio_out_open(const alp_audio_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_audio_out_start(alp_audio_out_t *o)
{
	(void)o;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_audio_out_stop(alp_audio_out_t *o)
{
	(void)o;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t
alp_audio_out_write(alp_audio_out_t *o, const void *b, size_t f, size_t *of, uint32_t t)
{
	(void)o;
	(void)b;
	(void)f;
	(void)t;
	if (of != NULL) *of = 0;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_audio_out_set_volume(alp_audio_out_t *o, uint8_t v)
{
	(void)o;
	(void)v;
	return ALP_ERR_NOSUPPORT;
}
void alp_audio_out_close(alp_audio_out_t *o)
{
	(void)o;
}
const alp_capabilities_t *alp_audio_out_capabilities(const alp_audio_out_t *o)
{
	(void)o;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_AUDIO_OUT */
