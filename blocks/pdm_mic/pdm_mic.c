/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Generic PDM-microphone block helper.  See header.
 *
 * v0.2: the surface is declared so blocks can compile against it, but
 * the underlying alp_i2s_* peripheral abstraction lands alongside
 * <alp/audio.h> later in v0.2.  Until then alp_pdm_mic_open returns
 * NULL and alp_pdm_mic_read returns ALP_ERR_NOSUPPORT — same shape as
 * the v0.1 iot_stub.c contract for <alp/iot.h>.
 */

#include <stddef.h>

#include "alp/blocks/pdm_mic.h"

alp_pdm_mic_t *alp_pdm_mic_open(const alp_pdm_mic_config_t *cfg)
{
	(void)cfg;
	return NULL;
}

alp_status_t alp_pdm_mic_read(
    alp_pdm_mic_t *mic, int16_t *out, size_t frames, size_t *out_frames, uint32_t timeout_ms)
{
	(void)mic;
	(void)out;
	(void)frames;
	(void)timeout_ms;
	if (out_frames != NULL) *out_frames = 0;
	return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_pdm_mic_set_gain(alp_pdm_mic_t *mic, int32_t left_gain_db, int32_t right_gain_db)
{
	(void)mic;
	(void)left_gain_db;
	(void)right_gain_db;
	return ALP_ERR_NOSUPPORT;
}

void alp_pdm_mic_close(alp_pdm_mic_t *mic)
{
	(void)mic;
}
