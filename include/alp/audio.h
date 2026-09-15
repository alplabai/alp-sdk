/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file audio.h
 * @brief Alp SDK audio abstraction (PDM input + I²S output).
 *
 * Real implementation lives in `src/zephyr/audio_zephyr.c` (input
 * path via Zephyr `audio_dmic` behind `CONFIG_ALP_SDK_AUDIO_IN`,
 * output path delegates to `<alp/i2s.h>` behind
 * `CONFIG_ALP_SDK_AUDIO_OUT`).  When either Kconfig is off the
 * matching `*_open` returns NULL with `alp_last_error() ==
 * ALP_ERR_NOSUPPORT` so apps link cleanly under `native_sim`.  The
 * default ALP DSP chain (DC-block in v0.2; AGC + resample in v0.3)
 * sits in-line on the input path.
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
 *
 * @par ABI status: [ABI-STABLE]
 *      v0.2 decl + v0.3 impl; PDM-in / I2S-out shape stable.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_AUDIO_H
#define ALP_AUDIO_H

#include <stdint.h>
#include <stddef.h>

#include "alp/cap_instance.h"
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** PCM sample format. */
typedef enum {
	ALP_AUDIO_FMT_S16_LE = 0, /**< Signed 16-bit little-endian. */
	ALP_AUDIO_FMT_S24_LE = 1, /**< Signed 24-bit, packed in 32-bit slots. */
	ALP_AUDIO_FMT_S32_LE = 2  /**< Signed 32-bit. */
} alp_audio_format_t;

/** Configuration shared by both audio-in and audio-out streams. */
typedef struct {
	uint32_t           peripheral_id;  /**< Studio-resolved PDM/I²S instance. */
	uint32_t           sample_rate_hz; /**< 8 k / 16 k / 44.1 k / 48 k typical. */
	uint8_t            channels;       /**< 1 = mono, 2 = stereo. */
	alp_audio_format_t format;
	uint16_t           frames_per_block; /**< Block size for DMA / ring queueing. */
} alp_audio_config_t;

/**
 * @brief Default-initialize an @ref alp_audio_config_t for peripheral @p id.
 *
 * Identity from @p id; canonical defaults (matching the mic capture
 * example above): @c sample_rate_hz = 16000 (16 kHz -- the common
 * voice-band rate), @c channels = 1 (mono), @c format = @ref
 * ALP_AUDIO_FMT_S16_LE (the most portable PCM format), @c
 * frames_per_block = 256 (a common DMA/ring block size). Shared by
 * both @ref alp_audio_in_open and @ref alp_audio_out_open.
 *
 * @note Expands to a compound literal (a GCC/Clang extension in C++ -- the
 *       SDK's toolchains; standard through C23).  Usable as an initializer
 *       or an expression.  On a compiler that rejects compound literals in
 *       C++ (e.g. MSVC), initialize the config's fields individually.
 */
#define ALP_AUDIO_CONFIG_DEFAULT(id) \
	((alp_audio_config_t){ .peripheral_id    = (id), \
	                       .sample_rate_hz   = 16000u, \
	                       .channels         = 1u, \
	                       .format           = ALP_AUDIO_FMT_S16_LE, \
	                       .frames_per_block = 256u })

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

/**
 * @brief Begin capturing.  Frames flow into an internal ring buffer.
 *
 * @param[in] in  Handle from @ref alp_audio_in_open.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY /
 *         ALP_ERR_NOSUPPORT / ALP_ERR_IO.
 */
alp_status_t alp_audio_in_start(alp_audio_in_t *in);

/**
 * @brief Stop capturing.  In-flight frames are drained.
 *
 * @param[in] in  Handle from @ref alp_audio_in_open.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY /
 *         ALP_ERR_NOSUPPORT.
 */
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
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_TIMEOUT /
 *         ALP_ERR_IO. ALP_ERR_IO (issue #2133 round 4c) means the backend
 *         itself reported dropped data mid-session (e.g. the Zephyr
 *         alif_pdm driver's slab-exhaustion/queue-overflow/hardware-FIFO-
 *         overflow detection, `-EIO` from `dmic_read()`) -- sticky until
 *         the caller stops and restarts the stream; every read keeps
 *         returning ALP_ERR_IO until then. This is distinct from
 *         ALP_ERR_TIMEOUT, which just means no data arrived within
 *         @p timeout_ms (including a non-blocking @p timeout_ms=0 call
 *         that simply found nothing queued yet -- issue #2133 round 4d)
 *         and does not by itself indicate loss.
 */
alp_status_t alp_audio_in_read(alp_audio_in_t *in,
                               void           *buf,
                               size_t          frames,
                               size_t         *out_frames,
                               uint32_t        timeout_ms);

/**
 * @brief Stop, free buffers, release handle.  NULL is a no-op.
 *
 * @param[in] in  Handle from @ref alp_audio_in_open, or NULL.
 */
void alp_audio_in_close(alp_audio_in_t *in);

/**
 * @brief Return the per-instance capability descriptor cached at open() time.
 *
 * Populated by the selected backend's ops->in_open() and snapshotted on the
 * handle so callers never see registry plumbing leak through.  Mirrors the
 * sibling getters on the other v0.7 backend-registry surfaces.
 *
 * @param[in] in  Handle from @ref alp_audio_in_open, or NULL.
 * @return Pointer to the cached descriptor, or NULL when @p in is NULL.
 *         The pointer stays valid until @ref alp_audio_in_close is called.
 */
const alp_capabilities_t *alp_audio_in_capabilities(const alp_audio_in_t *in);

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

/**
 * @brief Begin playback.  Caller must keep feeding via @ref alp_audio_out_write.
 *
 * Calling this before the first @ref alp_audio_out_write is legal and
 * does not fail merely because nothing is queued yet -- it does not
 * guarantee the hardware clock is running the instant this call returns.
 * A backend whose driver refuses to trigger with nothing queued (e.g.
 * the Zephyr I2S backend's DesignWare TX ring buffer, via
 * @ref alp_i2s_start) defers the real start until the first write
 * actually queues a block; a trigger failure discovered at that point
 * surfaces from that @ref alp_audio_out_write call instead of from here.
 * A second start() while already playing is backend-specific (e.g.
 * @ref ALP_ERR_IO on the Zephyr I2S backend, idempotent @ref ALP_OK on
 * Yocto/ALSA) -- do not assume either.
 *
 * On the Zephyr I2S backend, a gap between writes long enough for the
 * stream to underrun is recovered transparently -- calling this right
 * after (or the next @ref alp_audio_out_write, see its doc) resumes
 * playback instead of failing forever; the caller does not need to
 * detect or clear the condition itself.
 *
 * @param[in] out  Handle from @ref alp_audio_out_open.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY /
 *         ALP_ERR_NOSUPPORT / ALP_ERR_IO.
 */
alp_status_t alp_audio_out_start(alp_audio_out_t *out);

/**
 * @brief Stop playback.  Pending frames are drained.
 *
 * On the Zephyr I2S backend, recovers transparently from an underrun --
 * stopping a stream that underran still returns @ref ALP_OK and
 * releases whatever was queued, instead of failing because the stream
 * is not actively playing.
 *
 * @param[in] out  Handle from @ref alp_audio_out_open.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY /
 *         ALP_ERR_NOSUPPORT / ALP_ERR_IO (both the primary stop trigger and
 *         its own internal fallback were refused -- not expected in
 *         practice on a stream that was genuinely started).
 *
 * @note A stop/restart cycle here (or an underrun a backend recovers
 *   from transparently) only restores the SoC-side I2S link -- the bit
 *   clock and FSYNC resume, but nothing in this call reaches past the
 *   I2S pins.  An external codec or amplifier on the other end of that
 *   link may have put itself into a self-protective shutdown while the
 *   clock was gone and stay there once it comes back, silently: a
 *   subsequent @ref alp_audio_out_start returning @c ALP_OK says the
 *   SoC side restarted, not that the far end is listening again.  If
 *   your board has such a codec/amplifier, its driver may need to be
 *   re-armed after the first successful @ref alp_audio_out_write
 *   following that restart -- that write is what confirms the clock is
 *   actually running again, since a start can be deferred until it.
 *   For example, the TI TAS2563 smart amplifier enters software
 *   shutdown roughly 1 s after its TDM bit clock stops and needs
 *   `tas2563_resume()` (`<alp/chips/tas2563.h>`) called the same way,
 *   once the clock is confirmed back.
 */
alp_status_t alp_audio_out_stop(alp_audio_out_t *out);

/**
 * @brief Block until the driver is ready for the next PCM block, then push.
 *
 * On the Zephyr I2S backend, a successful queue here can also retry a
 * start() that @ref alp_audio_out_start deferred (see its doc); if that
 * retry still fails, the backend releases the block it just queued and
 * this call returns the start failure -- @p out_frames is NOT
 * incremented for a chunk whose queue attempt did not fully succeed, so
 * it always reflects frames genuinely accepted by the driver. A gap
 * between writes long enough for the stream to underrun is ALSO
 * recovered transparently here: this call resumes playback instead of
 * failing forever, with no separate stop()/start() needed first.
 *
 * @param[in]  out          Handle from @ref alp_audio_out_open.
 * @param[in]  buf          Source PCM data.
 * @param[in]  frames       Frames to push.  Must not exceed the
 *                          @c frames_per_block negotiated at open; a larger
 *                          value is refused with @ref ALP_ERR_OUT_OF_RANGE
 *                          rather than truncated.
 * @param[out] out_frames   Receives the frame count actually pushed.  May be NULL.
 * @param[in]  timeout_ms   Max wait for driver readiness.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_OUT_OF_RANGE /
 *         ALP_ERR_TIMEOUT / a deferred-start trigger failure (see above).
 */
alp_status_t alp_audio_out_write(alp_audio_out_t *out,
                                 const void      *buf,
                                 size_t           frames,
                                 size_t          *out_frames,
                                 uint32_t         timeout_ms);

/**
 * @brief Adjust output volume.
 *
 * @param[in] out  Handle from @ref alp_audio_out_open.
 * @param[in] vol  Linear scale 0..255.  Not in dB — caller does the
 *                 dB → linear conversion if needed.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT
 *         (@p vol is not full-scale (255) and the format the handle was
 *         opened with cannot be software-scaled by the backend -- e.g.
 *         S24_LE / S32_LE on the Yocto and Zephyr backends, which only
 *         scale S16_LE).
 */
alp_status_t alp_audio_out_set_volume(alp_audio_out_t *out, uint8_t vol);

/**
 * @brief Stop, free buffers, release handle.  NULL is a no-op.
 *
 * @param[in] out  Handle from @ref alp_audio_out_open, or NULL.
 */
void alp_audio_out_close(alp_audio_out_t *out);

/**
 * @brief Return the per-instance capability descriptor cached at open() time.
 *
 * Mirrors @ref alp_audio_in_capabilities -- snapshotted by the selected
 * backend's ops->out_open() on the handle.
 *
 * @param[in] out  Handle from @ref alp_audio_out_open, or NULL.
 * @return Pointer to the cached descriptor, or NULL when @p out is NULL.
 *         The pointer stays valid until @ref alp_audio_out_close is called.
 */
const alp_capabilities_t *alp_audio_out_capabilities(const alp_audio_out_t *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_AUDIO_H */
