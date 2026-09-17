/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file i2s.h
 * @brief Alp SDK I²S / SAI bus abstraction.
 *
 * The audio-data bus underneath `<alp/audio.h>` and the PDM helper in
 * `<alp/blocks/pdm_mic.h>`.  Apps that want to talk directly to a DAC,
 * a digital-mic array, or a SAI-attached codec come here.
 *
 * Backends:
 *   - Zephyr   : `i2s_*` driver class.
 *   - Yocto    : ALSA hwparams over the kernel's I²S DAI.
 *   - Baremetal: vendor HAL SAI / I²S peripheral.
 *
 * Memory model.  The Zephyr backend allocates a 2-block ping-pong
 * memory slab inside @ref alp_i2s_open.  Writes copy the caller's
 * buffer into the slab; reads copy the slab block into the caller's
 * buffer + free the slab block immediately.  That trades two
 * memcpys for not surfacing Zephyr's `mem_slab` lifecycle in the
 * public API.  Apps that need true zero-copy can drop to the
 * underlying Zephyr `i2s_*` driver class directly.
 *
 * @par ABI status: [ABI-STABLE]
 *      v0.2.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_I2S_H
#define ALP_I2S_H

#include <stdint.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Direction of data flow over the bus. */
typedef enum {
	ALP_I2S_DIR_RX   = 0, /**< Microphone, codec input. */
	ALP_I2S_DIR_TX   = 1, /**< DAC, codec output. */
	ALP_I2S_DIR_BOTH = 2  /**< Full-duplex (rare on M-class targets). */
} alp_i2s_dir_t;

/** Wire-format choice.  Most modern codecs accept @ref ALP_I2S_FMT_I2S. */
typedef enum {
	ALP_I2S_FMT_I2S             = 0, /**< Standard I²S (Phillips). */
	ALP_I2S_FMT_LEFT_JUSTIFIED  = 1,
	ALP_I2S_FMT_RIGHT_JUSTIFIED = 2,
	ALP_I2S_FMT_PCM_SHORT       = 3, /**< PCM with short frame sync. */
	ALP_I2S_FMT_PCM_LONG        = 4
} alp_i2s_format_t;

/** Opaque I²S handle.  Allocate via @ref alp_i2s_open. */
typedef struct alp_i2s alp_i2s_t;

/** Configuration passed to @ref alp_i2s_open. */
typedef struct {
	uint32_t         bus_id;
	uint32_t         sample_rate_hz;
	uint8_t          word_bits; /**< 16 / 24 / 32. */
	uint8_t          channels;  /**< 1 = mono, 2 = stereo. */
	alp_i2s_format_t format;
	alp_i2s_dir_t    direction;
	uint16_t         block_frames; /**< Frames per DMA block. */
} alp_i2s_config_t;

/**
 * @brief Default-initialize an @ref alp_i2s_config_t for bus @p id.
 *
 * Identity from @p id; canonical defaults: @c sample_rate_hz = 48000
 * (the common professional/CD-audio I²S rate), @c word_bits = 16 and
 * @c channels = 2 (16-bit stereo, the usual codec-facing shape),
 * @c format = @ref ALP_I2S_FMT_I2S ("most modern codecs accept" this
 * per the file header), @c direction = @ref ALP_I2S_DIR_RX (the
 * passive, listen-only default -- matches the enum-zero convention
 * used by every other ALP_*_CONFIG_DEFAULT), @c block_frames = 256
 * (matching the DMA/ring block size used elsewhere in the SDK).
 *
 * @note Expands to a compound literal (a GCC/Clang extension in C++ -- the
 *       SDK's toolchains; standard through C23).  Usable as an initializer
 *       or an expression.  On a compiler that rejects compound literals in
 *       C++ (e.g. MSVC), initialize the config's fields individually.
 */
#define ALP_I2S_CONFIG_DEFAULT(id) \
	((alp_i2s_config_t){ .bus_id         = (id), \
	                     .sample_rate_hz = 48000u, \
	                     .word_bits      = 16u, \
	                     .channels       = 2u, \
	                     .format         = ALP_I2S_FMT_I2S, \
	                     .direction      = ALP_I2S_DIR_RX, \
	                     .block_frames   = 256u })

/**
 * @brief Configure an I²S bus and allocate its DMA buffer slab.
 *
 * Does not start the clock — call @ref alp_i2s_start when ready to
 * stream.  All config fields are validated against the backend; an
 * unsupported sample rate or word width returns NULL here, not later.
 *
 * @param[in] cfg  Configuration.  Must be non-NULL with valid
 *                 @c channels (1 or 2), @c word_bits (16/24/32), and
 *                 @c block_frames > 0.
 * @return Open handle on success, or NULL on:
 *         - invalid @p cfg
 *         - bus_id out of range
 *         - underlying device not ready
 *         - heap allocation failure for the slab
 */
alp_i2s_t *alp_i2s_open(const alp_i2s_config_t *cfg);

/**
 * @brief Begin streaming.  TX direction starts producing the bit clock.
 *
 * Calling this before the first @ref alp_i2s_write on a TX handle is
 * legal and does not fail merely because nothing is queued yet -- some
 * backends (e.g. the Zephyr DesignWare driver) cannot trigger the real
 * hardware start with an empty queue, so the trigger is deferred until
 * the first @ref alp_i2s_write actually queues a block. A trigger
 * failure discovered at that point surfaces from that
 * @ref alp_i2s_write call instead of from here, and every subsequent
 * write retries the trigger until it succeeds. Calling this after data
 * is already queued (write-then-start) triggers immediately, as does
 * the RX direction always (it has nothing to defer).
 *
 * A TX stream that underran (the queue ran dry while playing) or an RX
 * stream that overran (the slab or queue was exhausted while capturing)
 * is recovered transparently: this call resumes the I2S stream instead
 * of failing forever. Calling this right after an underrun/overrun
 * behaves like calling it on a fresh handle -- if nothing is queued yet
 * on TX, the real start is deferred exactly as above; the caller does
 * not need to detect or clear the condition itself. An external codec
 * or amplifier that shut itself down when the bit clock stopped is not
 * re-armed by this layer (see issue #2146).
 *
 * @param[in] i2s  Handle from @ref alp_i2s_open.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_NOSUPPORT /
 *         ALP_ERR_IO / ALP_ERR_BUSY (issue #2150 phase 1: an RX start
 *         refused because TX is live on the same shared bit clock at a
 *         different rate -- retry once TX stops, or reconfigures to the
 *         same rate).
 */
alp_status_t alp_i2s_start(alp_i2s_t *i2s);

/**
 * @brief Drain any in-flight frames and stop the clock.
 *
 * Recovers transparently from a TX underrun or RX overrun -- stopping a
 * stream in that state still returns @ref ALP_OK and releases whatever
 * was queued, instead of failing because the stream is not RUNNING.
 *
 * @param[in] i2s  Handle from @ref alp_i2s_open.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_NOSUPPORT /
 *         ALP_ERR_IO.
 */
alp_status_t alp_i2s_stop(alp_i2s_t *i2s);

/**
 * @brief Push a PCM block to the TX path.
 *
 * Blocks up to @p timeout_ms waiting for slab room.  The function
 * memcpy's @p block into the driver-owned slab so the caller's buffer
 * can be reused immediately on return.
 *
 * On a TX handle whose @ref alp_i2s_start was deferred (see its doc),
 * a successful queue here also retries the real hardware start; if
 * that retry fails, the block that was just queued is released and
 * this call returns the start failure -- a write that returns an error
 * has NEVER left a block queued, so the caller's slab room is never
 * silently consumed by a stream that isn't playing. The next write
 * retries the deferred start again.
 *
 * A gap between writes long enough for the stream to underrun is
 * recovered transparently: the write that follows the gap resumes the
 * I2S stream instead of failing forever. The caller does not need to
 * call @ref alp_i2s_stop / @ref alp_i2s_start to clear the condition
 * first -- writing again is enough (see @ref alp_i2s_start's doc for
 * what this recovery does not cover downstream of the bus).
 *
 * @param[in] i2s         Handle from @ref alp_i2s_open with TX direction.
 * @param[in] block       Source PCM data.
 * @param[in] bytes       Source length.  Must not exceed the block size
 *                        negotiated at open (`block_frames` × `channels` ×
 *                        `word_bits / 8`); a larger value is refused with
 *                        @ref ALP_ERR_OUT_OF_RANGE rather than truncated.
 * @param[in] timeout_ms  Max wait for an available slab block.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_OUT_OF_RANGE /
 *         ALP_ERR_TIMEOUT / a deferred-start trigger failure (see above).
 */
alp_status_t alp_i2s_write(alp_i2s_t *i2s, const void *block, size_t bytes, uint32_t timeout_ms);

/**
 * @brief Pull the next PCM block from the RX path.
 *
 * Returns the count actually copied via @p bytes_out.  May be less
 * than @p bytes if the driver's block size is smaller; never more.
 *
 * @param[in]  i2s         Handle from @ref alp_i2s_open with RX direction.
 * @param[out] block       Destination buffer.
 * @param[in]  bytes       Destination capacity.
 * @param[out] bytes_out   Receives the byte count copied.  May be NULL.
 * @param[in]  timeout_ms  Caller's preferred wait budget.  The Zephyr
 *                         backend currently ignores this and relies on
 *                         the underlying `i2s_read` blocking until a
 *                         frame is available (the upstream `i2s` API
 *                         doesn't take a per-call timeout); a future
 *                         revision may add config-time read-timeout
 *                         plumbing through `alp_i2s_config_t`.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_IO.
 */
alp_status_t
alp_i2s_read(alp_i2s_t *i2s, void *block, size_t bytes, size_t *bytes_out, uint32_t timeout_ms);

/** @brief Stop streaming, free the slab, release the handle.  NULL safe. */
void alp_i2s_close(alp_i2s_t *i2s);

/**
 * @brief Query the capabilities of an opened I²S handle.
 *
 * @param i2s  Handle from @ref alp_i2s_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p i2s is NULL.
 */
const alp_capabilities_t *alp_i2s_capabilities(const alp_i2s_t *i2s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_I2S_H */
