/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software audio fallback.  Wildcard backend at priority 0 --
 * picked only when no higher-priority hardware-aware audio backend
 * is linked into the build (the typical native_sim trimmed-image
 * case, or any build with CONFIG_ALP_SDK_AUDIO_IN / _OUT=n).
 *
 * Contract:
 *   - in_open / out_open  -> ALP_OK (the dispatcher hands out a
 *     handle the caller can subsequently address; no underlying
 *     hardware stands behind it)
 *   - in_start / in_stop / out_start / out_stop -> ALP_OK (no-op
 *     state toggle).
 *   - in_read   -> ALP_OK; zero-fills the caller's buffer up to
 *     @p frames, reports @p frames via *out_frames.  Silence
 *     source -- the test/native_sim use case is "the audio API is
 *     reachable and round-trips", not "the audio path produces
 *     anything meaningful".
 *   - out_write -> ALP_OK; null sink (discards), reports @p frames
 *     via *out_frames so caller-side accounting stays consistent.
 *   - out_set_volume -> ALP_OK (accepts any value; no underlying
 *     scale to apply).
 *   - in_close / out_close -> no-op.
 *
 * Matches the design spec Section 5 sw_fallback contract.
 *
 * @par Cost: ROM ~250 B, zero RAM (no per-handle backend state --
 *      every state->be_data is left NULL).  No Zephyr audio_dmic /
 *      alp_i2s_* linkage required, so this backend compiles
 *      cleanly on native_sim trimmed-image builds where neither
 *      driver class is present.
 * @par Performance: O(1) per call for start / stop / open / close /
 *      set_volume.  in_read is O(frames * channels * width) for the
 *      memset-style zero-fill; out_write is O(1) (the buffer is
 *      discarded, not copied).  All ops are reentrant and
 *      lock-free.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/audio.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "audio_ops.h"

/* ---------- Helpers ---------- */

static uint8_t sw_pcm_width_for(alp_audio_format_t f)
{
	switch (f) {
	case ALP_AUDIO_FMT_S16_LE:
		return 16;
	case ALP_AUDIO_FMT_S24_LE:
		return 24;
	case ALP_AUDIO_FMT_S32_LE:
		return 32;
	default:
		return 16;
	}
}

static size_t sw_bytes_per_frame(const alp_audio_config_t *cfg)
{
	return (size_t)cfg->channels * (size_t)((sw_pcm_width_for(cfg->format) + 7u) / 8u);
}

/* ---------- Input ---------- */

static alp_status_t sw_in_open(const alp_audio_config_t     *cfg,
                               alp_audio_in_backend_state_t *state,
                               alp_capabilities_t           *caps_out)
{
	(void)cfg;
	state->be_data  = NULL;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t sw_in_start(alp_audio_in_backend_state_t *state)
{
	(void)state;
	return ALP_OK;
}

static alp_status_t sw_in_stop(alp_audio_in_backend_state_t *state)
{
	(void)state;
	return ALP_OK;
}

static alp_status_t sw_in_read(alp_audio_in_backend_state_t *state,
                               void                         *buf,
                               size_t                        frames,
                               size_t                       *out_frames,
                               uint32_t                      timeout_ms)
{
	(void)timeout_ms;
	/* Zero-fill -- silence source.  Tests that need byte-counts get
     * a predictable pattern; production builds that need real audio
     * MUST link the priority-100 zephyr_drv backend. */
	size_t bytes = frames * sw_bytes_per_frame(&state->cfg);
	memset(buf, 0, bytes);
	if (out_frames != NULL) *out_frames = frames;
	return ALP_OK;
}

static void sw_in_close(alp_audio_in_backend_state_t *state)
{
	(void)state;
}

/* ---------- Output ---------- */

static alp_status_t sw_out_open(const alp_audio_config_t      *cfg,
                                alp_audio_out_backend_state_t *state,
                                alp_capabilities_t            *caps_out)
{
	(void)cfg;
	state->be_data  = NULL;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t sw_out_start(alp_audio_out_backend_state_t *state)
{
	(void)state;
	return ALP_OK;
}

static alp_status_t sw_out_stop(alp_audio_out_backend_state_t *state)
{
	(void)state;
	return ALP_OK;
}

static alp_status_t sw_out_write(alp_audio_out_backend_state_t *state,
                                 const void                    *buf,
                                 size_t                         frames,
                                 size_t                        *out_frames,
                                 uint32_t                       timeout_ms)
{
	(void)state;
	(void)buf;
	(void)timeout_ms;
	/* Null sink -- caller-side accounting needs the frame count back
     * so partial-write loops do not spin. */
	if (out_frames != NULL) *out_frames = frames;
	return ALP_OK;
}

static alp_status_t sw_out_set_volume(alp_audio_out_backend_state_t *state, uint8_t vol)
{
	(void)state;
	(void)vol;
	return ALP_OK;
}

static void sw_out_close(alp_audio_out_backend_state_t *state)
{
	(void)state;
}

/* ---------- Registration ---------- */

static const alp_audio_ops_t _ops = {
	.in_open        = sw_in_open,
	.in_start       = sw_in_start,
	.in_stop        = sw_in_stop,
	.in_read        = sw_in_read,
	.in_close       = sw_in_close,
	.out_open       = sw_out_open,
	.out_start      = sw_out_start,
	.out_stop       = sw_out_stop,
	.out_write      = sw_out_write,
	.out_set_volume = sw_out_set_volume,
	.out_close      = sw_out_close,
};

/* Export the audio static-archive anchor the dispatcher references (#368). */
ALP_BACKEND_ANCHOR_DEFINE(audio);

ALP_BACKEND_REGISTER(audio,
                     sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_fallback",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
