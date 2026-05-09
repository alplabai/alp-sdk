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
 * DSP chain: DC-block → AGC → resample) lands in v0.2.
 *
 * Backends:
 *   - Zephyr   : `audio_dmic` API for PDM in, `i2s` driver class for I²S out.
 *   - Yocto    : ALSA-backed (PCM capture/playback).
 *   - Baremetal: Vendor PDM driver + SAI peripheral direct.
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

typedef struct alp_audio_in alp_audio_in_t;

alp_audio_in_t *alp_audio_in_open(const alp_audio_config_t *cfg);

alp_status_t alp_audio_in_start(alp_audio_in_t *in);
alp_status_t alp_audio_in_stop(alp_audio_in_t *in);

/**
 * @brief Block until the next PCM block is available, then copy it
 *        into the caller's buffer.
 *
 * Frames are interleaved across channels.  Returns the number of
 * frames actually delivered via @p out_frames.
 */
alp_status_t alp_audio_in_read(alp_audio_in_t *in,
                               void *buf, size_t frames,
                               size_t *out_frames,
                               uint32_t timeout_ms);

void          alp_audio_in_close(alp_audio_in_t *in);

/* ------------------------------------------------------------------ */
/* Audio output (I²S DAC, line-out, headphone amp)                     */
/* ------------------------------------------------------------------ */

typedef struct alp_audio_out alp_audio_out_t;

alp_audio_out_t *alp_audio_out_open(const alp_audio_config_t *cfg);

alp_status_t alp_audio_out_start(alp_audio_out_t *out);
alp_status_t alp_audio_out_stop (alp_audio_out_t *out);

/** Block until the driver is ready for the next PCM block, then push. */
alp_status_t alp_audio_out_write(alp_audio_out_t *out,
                                 const void *buf, size_t frames,
                                 size_t *out_frames,
                                 uint32_t timeout_ms);

/** Adjust output volume.  Range 0..255 (linear, not dB). */
alp_status_t alp_audio_out_set_volume(alp_audio_out_t *out, uint8_t vol);

void          alp_audio_out_close(alp_audio_out_t *out);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_AUDIO_H */
