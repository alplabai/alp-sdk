/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test doubles for the seams src/zephyr/peripheral_adc.c reaches through:
 * the V2N supervisor singleton, the GD32G553 stream opcodes, the last-error
 * store and the DSP chain (the filter/spectrum half of that file, never
 * exercised here but still linked).
 *
 * The GD32 is modelled per stream slot: STREAM_BEGIN on an already-active
 * slot answers ALP_ERR_INVAL (docs/gd32-bridge-protocol.md: the firmware
 * refuses to rebind a live stream), STREAM_END clears it.  The supervisor
 * acquire result is scripted so a test can make it busy / timed out.
 */

#include "alp/chips/gd32g553.h"
#include "alp/dsp.h"
#include "alp_z_last_error.h"
#include "stubs.h"
#include "v2n_supervisor.h"

struct stub_state g_stub;

static gd32g553_t s_fake_ctx;

alp_status_t alp_z_v2n_supervisor_acquire(gd32g553_t **ctx_out)
{
	*ctx_out = NULL;
	g_stub.acquire_calls++;
	if (g_stub.script_pos < g_stub.script_len) {
		const alp_status_t s = g_stub.script[g_stub.script_pos++];
		if (s != ALP_OK) return s;
	} else if (g_stub.always_busy) {
		return ALP_ERR_BUSY;
	}
	*ctx_out = &s_fake_ctx;
	return ALP_OK;
}

void alp_z_v2n_supervisor_release(void)
{
	g_stub.release_calls++;
}

alp_status_t gd32g553_adc_stream_begin(gd32g553_t *ctx,
                                       uint8_t     stream_id,
                                       uint8_t     channel,
                                       uint32_t    sample_rate_hz)
{
	(void)ctx;
	(void)channel;
	(void)sample_rate_hz;
	if (g_stub.gd32_stream_active[stream_id]) return ALP_ERR_INVAL;
	g_stub.gd32_stream_active[stream_id] = true;
	return ALP_OK;
}

alp_status_t gd32g553_adc_stream_end(gd32g553_t *ctx, uint8_t stream_id)
{
	(void)ctx;
	g_stub.end_calls++;
	g_stub.gd32_stream_active[stream_id] = false;
	return ALP_OK;
}

alp_status_t gd32g553_adc_stream_read(gd32g553_t *ctx,
                                      uint8_t     stream_id,
                                      uint8_t     max_samples,
                                      uint8_t    *got_samples,
                                      uint16_t   *mv)
{
	(void)ctx;
	(void)stream_id;
	(void)max_samples;
	(void)mv;
	*got_samples = 0u;
	return ALP_OK;
}

void alp_z_set_last_error(alp_status_t s)
{
	g_stub.last_error = s;
}

void alp_z_clear_last_error(void)
{
	g_stub.last_error = ALP_OK;
}

alp_dsp_chain_t *alp_dsp_chain_open(const alp_dsp_stage_t *stages, size_t n_stages)
{
	(void)stages;
	(void)n_stages;
	return NULL;
}

alp_status_t alp_dsp_chain_apply_samples(alp_dsp_chain_t *chain,
                                         const int16_t   *in_mv,
                                         size_t           in_n,
                                         int16_t         *out_mv,
                                         size_t           out_cap,
                                         size_t          *got)
{
	(void)chain;
	(void)in_mv;
	(void)in_n;
	(void)out_mv;
	(void)out_cap;
	(void)got;
	return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_dsp_chain_apply_bins(alp_dsp_chain_t *chain,
                                      const int16_t   *in_mv,
                                      size_t           in_n,
                                      float           *out_bins,
                                      size_t           out_cap,
                                      size_t          *got)
{
	(void)chain;
	(void)in_mv;
	(void)in_n;
	(void)out_bins;
	(void)out_cap;
	(void)got;
	return ALP_ERR_NOSUPPORT;
}

void alp_dsp_chain_close(alp_dsp_chain_t *chain)
{
	(void)chain;
}
