/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file audio.h
 * @brief ALP SDK audio abstraction (PDM input + I²S output).
 *
 * v0.2 deliverable.  v0.1 ships only the public surface; every entry
 * point returns ALP_ERR_NOSUPPORT and `*_open()` returns NULL — see
 * `<alp/iot.h>` v0.1 for the same contract.  Real implementation
 * (PDM ingest via the SoC's PDM/SAI block, I²S egress, ALP-default
 * DSP chain: DC-block → AGC → resample) lands in v0.2 and sits on
 * top of the v0.2 `<alp/i2s.h>` wrapper.
 *
 * Backends:
 *   - Zephyr   : `audio_dmic` API for PDM in, `i2s` driver class for I²S out.
 *   - Yocto    : ALSA hwparams over the kernel's PCM capture/playback DAIs.
 *   - Baremetal: Vendor PDM driver + SAI peripheral direct.
 *
 * Typical usage:
 * @code
 *     alp_audio_in_t *mic = alp_audio_in_open(&(alp_audio_config_t){
 *         .peripheral_id   = 0,
 *         .sample_rate_hz  = 16000,
 *         .channels        = 1,
 *         .format          = ALP_AUDIO_FMT_S16_LE,
 *         .frames_per_block = 256,
 *     });
 *     alp_audio_in_start(mic);
 *     int16_t buf[256];
 *     size_t got = 0;
 *     alp_audio_in_read(mic, buf, 256, &got, 100);
 * @endcode
 */

#ifndef ALP_AUDIO_H
#define ALP_AUDIO_H

#include <stdint.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** PCM sample format. */
typedef enum {
    ALP_AUDIO_FMT_S16_LE = 0,    /**< Signed 16-bit little-endian. */
    ALP_AUDIO_FMT_S24_LE = 1,    /**< Signed 24-bit, packed in 32-bit slots. */
    ALP_AUDIO_FMT_S32_LE = 2     /**< Signed 32-bit. */
} alp_audio_format_t;

/** Configuration shared by both audio-in and audio-out streams. */
typedef struct {
    uint32_t           peripheral_id;   /**< Studio-resolved PDM/I²S instance. */
    uint32_t           sample_rate_hz;  /**< 8 k / 16 k / 44.1 k / 48 k typical. */
    uint8_t            channels;        /**< 1 = mono, 2 = stereo. */
    alp_audio_format_t format;
    uint16_t           frames_per_block; /**< Block size for DMA / ring queueing. */
} alp_audio_config_t;

/* ------------------------------------------------------------------ */
/* Audio input (PDM mic, line-in)                                      */
/* ------------------------------------------------------------------ */

/** Opaque audio-input handle.  Allocate via @ref alp_audio_in_open. */
typedef struct alp_audio_in alp_audio_in_t;

/**
 * @brief Acquire and configure an audio input stream.
 *
 * @param[in] cfg  Configuration.  Must be non-NULL.
 * @return Open handle on success, or NULL if the backend can't satisfy
 *         the requested configuration.
 */
alp_audio_in_t *alp_audio_in_open(const alp_audio_config_t *cfg);

/** @brief Begin capturing.  Frames flow into an internal ring buffer. */
alp_status_t alp_audio_in_start(alp_audio_in_t *in);

/** @brief Stop capturing.  In-flight frames are drained. */
alp_status_t alp_audio_in_stop(alp_audio_in_t *in);

/**
 * @brief Block until the next PCM block is available, then copy it
 *        into the caller's buffer.
 *
 * Frames are interleaved across channels.
 *
 * @param[in]  in           Handle from @ref alp_audio_in_open.
 * @param[out] buf          Destination buffer.  Size in bytes must be
 *                          ≥ @p frames × channels × sample-size.
 * @param[in]  frames       Maximum frames to deliver.
 * @param[out] out_frames   Receives the frame count actually delivered.
 *                          May be NULL.
 * @param[in]  timeout_ms   Max wait for available frames.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_TIMEOUT.
 */
alp_status_t alp_audio_in_read(alp_audio_in_t *in,
                               void *buf, size_t frames,
                               size_t *out_frames,
                               uint32_t timeout_ms);

/** @brief Stop, free buffers, release handle.  NULL is a no-op. */
void          alp_audio_in_close(alp_audio_in_t *in);

/* ------------------------------------------------------------------ */
/* Audio output (I²S DAC, line-out, headphone amp)                     */
/* ------------------------------------------------------------------ */

/** Opaque audio-output handle.  Allocate via @ref alp_audio_out_open. */
typedef struct alp_audio_out alp_audio_out_t;

/**
 * @brief Acquire and configure an audio output stream.
 *
 * @param[in] cfg  Configuration.  Must be non-NULL.
 * @return Open handle on success, or NULL on failure.
 */
alp_audio_out_t *alp_audio_out_open(const alp_audio_config_t *cfg);

/** @brief Begin playback.  Caller must keep feeding via @ref alp_audio_out_write. */
alp_status_t alp_audio_out_start(alp_audio_out_t *out);

/** @brief Stop playback.  Pending frames are drained. */
alp_status_t alp_audio_out_stop (alp_audio_out_t *out);

/**
 * @brief Block until the driver is ready for the next PCM block, then push.
 *
 * @param[in]  out          Handle from @ref alp_audio_out_open.
 * @param[in]  buf          Source PCM data.
 * @param[in]  frames       Frames to push.
 * @param[out] out_frames   Receives the frame count actually pushed.  May be NULL.
 * @param[in]  timeout_ms   Max wait for driver readiness.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_TIMEOUT.
 */
alp_status_t alp_audio_out_write(alp_audio_out_t *out,
                                 const void *buf, size_t frames,
                                 size_t *out_frames,
                                 uint32_t timeout_ms);

/**
 * @brief Adjust output volume.
 *
 * @param[in] out  Handle from @ref alp_audio_out_open.
 * @param[in] vol  Linear scale 0..255.  Not in dB — caller does the
 *                 dB → linear conversion if needed.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL.
 */
alp_status_t alp_audio_out_set_volume(alp_audio_out_t *out, uint8_t vol);

/** @brief Stop, free buffers, release handle.  NULL is a no-op. */
void          alp_audio_out_close(alp_audio_out_t *out);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_AUDIO_H */
