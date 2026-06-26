/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file pdm_mic.h
 * @brief Generic PDM microphone block helper.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Consumed by alp-studio block `blk_pdm_mic`.  Lives under
 * `<alp/blocks/>` (NOT `<alp/chips/>`) because — like
 * `<alp/blocks/button_led.h>` — this is an SDK-level *block*
 * utility, not a binding to a specific microphone IC.  Hence the
 * `alp_` prefix on every symbol.  Any compliant PDM mic (Knowles
 * SPH0645LM4H-B, STMicro MP34DT06J, Infineon IM73A135V01, …) drops
 * in.  See `blocks/README.md` for the full block-vs-chip rationale.
 *
 * v0.2 scope: declares the surface a PDM source needs to expose to
 * `<alp/audio.h>`; the underlying I²S/PDM peripheral abstraction
 * (`alp_i2s_t`) ships in v0.2's audio library work — until then
 * @ref alp_pdm_mic_open returns NULL and read() returns
 * `ALP_ERR_NOSUPPORT`.  Same shape as `<alp/iot.h>` in v0.1.
 */

#ifndef ALP_BLOCKS_PDM_MIC_H
#define ALP_BLOCKS_PDM_MIC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Number of channels the helper carries from the PDM front-end. */
typedef enum { ALP_PDM_MIC_MONO = 1, ALP_PDM_MIC_STEREO = 2 } alp_pdm_mic_channels_t;

/** @brief Opaque PDM-microphone helper handle (allocated by @ref alp_pdm_mic_open). */
typedef struct alp_pdm_mic alp_pdm_mic_t;

/** @brief Open-time configuration for a PDM microphone helper. */
typedef struct {
	uint32_t               peripheral_id;  /**< Studio-resolved I²S/PDM instance. */
	uint32_t               sample_rate_hz; /**< 8 kHz / 16 kHz / 48 kHz typical. */
	alp_pdm_mic_channels_t channels;       /**< Mono or stereo; selects the capture layout. */
	uint8_t                sample_bits;    /**< 16 or 24. */
	uint32_t               left_gain_db;   /**< +/- gain in dB applied by the decimator. */
	uint32_t right_gain_db; /**< Same as @ref left_gain_db, applied to the right channel. */
} alp_pdm_mic_config_t;

/**
 * @brief Acquire a PDM-microphone helper handle.
 *
 * v0.2: returns NULL — the underlying `alp_i2s_*` peripheral abstraction
 * lands in v0.2's audio library work (see `<alp/audio.h>`).
 *
 * @param cfg Configuration to apply; copied by the call, so the caller may
 *            free or reuse it afterwards. Must not be NULL.
 * @return Owning handle to pass to the other @c alp_pdm_mic_* calls and
 *         release with @ref alp_pdm_mic_close, or NULL on failure (including
 *         the v0.2 not-yet-implemented case).
 */
alp_pdm_mic_t *alp_pdm_mic_open(const alp_pdm_mic_config_t *cfg);

/**
 * @brief Pull @p frames PCM frames from the mic into @p out.
 *
 * Frames interleave channels (L,R,L,R,…) in stereo configurations. Blocks up to
 * @p timeout_ms for data; a short read (fewer than @p frames) is reported via
 * @p out_frames rather than treated as an error.
 *
 * @param mic        Handle from @ref alp_pdm_mic_open. Must not be NULL.
 * @param out        Caller-owned buffer receiving signed 16-bit PCM samples;
 *                   sized for @p frames frames (frames * channels samples).
 * @param frames     Maximum number of frames to read into @p out.
 * @param out_frames Out: number of frames actually written to @p out. May be 0
 *                   on timeout. Must not be NULL.
 * @param timeout_ms Maximum time to wait for data, in milliseconds; 0 polls.
 * @return ALP_OK on success (including a 0-frame timeout), or an ALP_ERR_* code
 *         on failure. Returns ALP_ERR_NOSUPPORT until the v0.2 audio backend lands.
 */
alp_status_t alp_pdm_mic_read(
    alp_pdm_mic_t *mic, int16_t *out, size_t frames, size_t *out_frames, uint32_t timeout_ms);

/**
 * @brief Apply a runtime gain change (dB, signed) to the channels.
 *
 * @param mic           Handle from @ref alp_pdm_mic_open. Must not be NULL.
 * @param left_gain_db  Signed gain for the left channel, in dB.
 * @param right_gain_db Signed gain for the right channel, in dB; ignored in mono.
 * @return ALP_OK on success, or an ALP_ERR_* code on failure.
 */
alp_status_t alp_pdm_mic_set_gain(alp_pdm_mic_t *mic, int32_t left_gain_db, int32_t right_gain_db);

/**
 * @brief Release the helper and stop the underlying peripheral.
 *
 * @param mic Handle from @ref alp_pdm_mic_open. NULL is a no-op. The handle is
 *            invalid after this call.
 */
void alp_pdm_mic_close(alp_pdm_mic_t *mic);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_BLOCKS_PDM_MIC_H */
