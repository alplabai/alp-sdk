/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Real Linux/Yocto i2s_* driver-class backend.  Binds the alp_i2s
 * dispatcher's ops vtable to ALSA (libasound) over the kernel's I²S /
 * SAI DAI, which Linux surfaces as an ALSA PCM device (hw:<card>,<dev>).
 * Registered at priority 100 with vendor "linux"; the sw_fallback
 * backend (priority 0) still wins on non-Linux native_sim builds where
 * this TU compiles to an empty object.
 *
 * Selected on any silicon (silicon_ref "*") because the ALSA PCM ABI is
 * SoC-agnostic; the device-tree / machine driver decides which physical
 * I²S DAI backs a given card/device number.
 *
 * Device naming: alp_i2s_config_t.bus_id maps to an ALSA PCM device:
 *     bus_id == 0 -> "default"        (the per-user ALSA default;
 *                                      honours /etc/asound.conf)
 *     bus_id == N -> "hw:<N-1>,0"     (card N-1, device 0)
 * This mirrors src/yocto/audio_yocto.c's resolve_device_name() so the
 * audio and raw-I²S surfaces share one numbering convention.
 *
 * STATUS: real implementation; Yocto-link + on-target run BENCH-UNVERIFIED
 * (no sysroot / no real I²S DAI device node available in this tree).
 *
 * CMake-gated behind pkg_check_modules(ALSA libasound) exactly like
 * audio_yocto.c -- when libasound is absent the class stays on the stub
 * and this TU is not compiled.
 */

#if defined(__linux__)

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <alsa/asoundlib.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/i2s.h>
#include <alp/peripheral.h>

#include "i2s_ops.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* Per-handle backend sidecar.  Holds the open ALSA PCM handle plus the
 * frame geometry needed to translate the dispatcher's byte-oriented
 * write/read into ALSA's frame-oriented snd_pcm_writei/readi.  Boxed
 * onto the heap so the void* be_data slot in alp_i2s_backend_state_t
 * owns it and the portable handle stays free of ALSA types. */
typedef struct {
	snd_pcm_t *pcm;
	size_t     frame_bytes; /* channels * (word_bits / 8) */
	bool       capture;     /* true: RX stream; false: TX stream */
} y_i2s_data_t;

/** @brief Clamp a uint32_t millisecond timeout to ALSA's int-typed wait.
 *
 * snd_pcm_wait() takes an int; a timeout_ms above INT_MAX would wrap
 * negative through a plain cast, and ALSA treats a negative timeout as
 * "wait forever" -- turning a finite timeout into an infinite block.
 * INT_MAX milliseconds is ~24.8 days, so the clamp is semantically a
 * no-op for any realistic timeout. */
static int _clamp_wait_ms(uint32_t timeout_ms)
{
	return (timeout_ms > (uint32_t)INT_MAX) ? INT_MAX : (int)timeout_ms;
}

/** @brief Map a snd_pcm_* return code (negative errno) to alp_status_t. */
static alp_status_t _alsa_to_alp(int rc)
{
	if (rc >= 0) return ALP_OK;
	switch (-rc) {
	case EINVAL:
		return ALP_ERR_INVAL;
	case ENOENT:
	case ENODEV:
		return ALP_ERR_NOT_READY;
	case EBUSY:
		return ALP_ERR_BUSY;
	case EAGAIN:
	case ETIMEDOUT:
		return ALP_ERR_TIMEOUT;
	case ENOMEM:
		return ALP_ERR_NOMEM;
	case ENOSYS:
	case ENOTSUP:
		return ALP_ERR_NOSUPPORT;
	default:
		return ALP_ERR_IO;
	}
}

/** @brief Map an alp_i2s word width to its packed ALSA sample format.
 *
 * The alp_i2s contract documents 16 / 24 / 32 bit words.  ALSA's
 * SND_PCM_FORMAT_S24_LE is the 32-bit-container variant, which is the
 * standard way I²S DAIs carry 24-bit data on Linux.  Returns
 * SND_PCM_FORMAT_UNKNOWN for any other width so the caller rejects it. */
static snd_pcm_format_t _to_alsa_format(uint8_t word_bits)
{
	switch (word_bits) {
	case 16:
		return SND_PCM_FORMAT_S16_LE;
	case 24:
		return SND_PCM_FORMAT_S24_LE; /* 32-bit container */
	case 32:
		return SND_PCM_FORMAT_S32_LE;
	default:
		return SND_PCM_FORMAT_UNKNOWN;
	}
}

/* Resolve a bus_id into the canonical ALSA PCM device name.  Writes
 * into the caller-supplied buffer (32 bytes is plenty for "hw:<N>,0"). */
static int _resolve_device_name(uint32_t bus_id, char *out, size_t cap)
{
	if (bus_id == 0u) {
		return snprintf(out, cap, "default");
	}
	return snprintf(out, cap, "hw:%u,0", (unsigned)(bus_id - 1u));
}

/* Apply hw params (access / format / channels / rate / period) to a
 * freshly-opened PCM handle from the alp_i2s config.  Mirrors the
 * configure_pcm() flow in audio_yocto.c.  Returns ALP_OK or an
 * alp_status_t on failure. */
static alp_status_t
_configure_pcm(snd_pcm_t *pcm, const alp_i2s_config_t *cfg, snd_pcm_format_t fmt)
{
	snd_pcm_hw_params_t *hw = NULL;
	snd_pcm_hw_params_alloca(&hw);

	int rc = snd_pcm_hw_params_any(pcm, hw);
	if (rc < 0) return _alsa_to_alp(rc);

	rc = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (rc < 0) return _alsa_to_alp(rc);

	rc = snd_pcm_hw_params_set_format(pcm, hw, fmt);
	if (rc < 0) return _alsa_to_alp(rc);

	rc = snd_pcm_hw_params_set_channels(pcm, hw, cfg->channels);
	if (rc < 0) return _alsa_to_alp(rc);

	unsigned int rate = cfg->sample_rate_hz;
	rc                = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL);
	if (rc < 0) return _alsa_to_alp(rc);

	/* block_frames is the alp_i2s DMA-block unit; map it to the ALSA
     * period size so each writei/readi corresponds to one block. */
	snd_pcm_uframes_t period = cfg->block_frames;
	rc                       = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL);
	if (rc < 0) return _alsa_to_alp(rc);

	/* Buffer = 4 periods: enough to absorb scheduling jitter without
     * bloating RAM.  Apps that need different sizing tune block_frames. */
	snd_pcm_uframes_t buf = period * 4u;
	rc                    = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buf);
	if (rc < 0) return _alsa_to_alp(rc);

	rc = snd_pcm_hw_params(pcm, hw);
	if (rc < 0) return _alsa_to_alp(rc);

	return ALP_OK;
}

/**
 * @brief Open the ALSA PCM device backing bus_id and configure it.
 *
 * The direction picks the PCM stream: ALP_I2S_DIR_RX -> CAPTURE,
 * ALP_I2S_DIR_TX -> PLAYBACK.  ALP_I2S_DIR_BOTH (full-duplex) cannot be
 * expressed as a single ALSA PCM handle and is rejected with
 * ALP_ERR_NOSUPPORT -- a true full-duplex backend would open two PCMs
 * and is out of scope for this slice.
 *
 * Wire format (Phillips I²S vs left/right-justified vs PCM) is a
 * property of the DAI link configured by the ALSA machine driver /
 * device-tree, not a userspace snd_pcm_* setting.  ALSA exposes no
 * standard PCM-handle API to override it, so cfg->format is honoured
 * only for the standard I²S case (which the DAI is presumed wired for);
 * any other requested format returns ALP_ERR_NOSUPPORT rather than
 * silently ignoring it.
 *
 * The ALSA PCM handle has no queryable instance-capability surface
 * beyond presence, so caps stay 0.
 */
static alp_status_t
y_open(const alp_i2s_config_t *cfg, alp_i2s_backend_state_t *st, alp_capabilities_t *caps_out)
{
	if (cfg == NULL || st == NULL) return ALP_ERR_INVAL;
	if (cfg->channels == 0u || cfg->channels > 2u) return ALP_ERR_INVAL;
	if (cfg->sample_rate_hz == 0u || cfg->block_frames == 0u) return ALP_ERR_INVAL;

	snd_pcm_format_t fmt = _to_alsa_format(cfg->word_bits);
	if (fmt == SND_PCM_FORMAT_UNKNOWN) return ALP_ERR_INVAL;

	/* Only standard I²S framing maps cleanly to a PCM handle (see the
     * function-level note); the other wire formats are DAI-link
     * properties with no userspace override. */
	if (cfg->format != ALP_I2S_FMT_I2S) return ALP_ERR_NOSUPPORT;

	snd_pcm_stream_t stream;
	bool             capture;
	switch (cfg->direction) {
	case ALP_I2S_DIR_RX:
		stream  = SND_PCM_STREAM_CAPTURE;
		capture = true;
		break;
	case ALP_I2S_DIR_TX:
		stream  = SND_PCM_STREAM_PLAYBACK;
		capture = false;
		break;
	case ALP_I2S_DIR_BOTH:
	default:
		/* Full-duplex needs two PCM handles; not modelled here. */
		return ALP_ERR_NOSUPPORT;
	}

	char dev[32];
	int  n = _resolve_device_name(cfg->bus_id, dev, sizeof(dev));
	if (n < 0 || (size_t)n >= sizeof(dev)) return ALP_ERR_INVAL;

	snd_pcm_t *pcm = NULL;
	int        rc  = snd_pcm_open(&pcm, dev, stream, 0);
	if (rc < 0) return _alsa_to_alp(rc);

	alp_status_t s = _configure_pcm(pcm, cfg, fmt);
	if (s != ALP_OK) {
		(void)snd_pcm_close(pcm);
		return s;
	}

	y_i2s_data_t *d = (y_i2s_data_t *)malloc(sizeof(*d));
	if (d == NULL) {
		(void)snd_pcm_close(pcm);
		return ALP_ERR_NOMEM;
	}
	d->pcm         = pcm;
	d->capture     = capture;
	d->frame_bytes = (size_t)cfg->channels * (size_t)((cfg->word_bits + 7u) / 8u);

	st->dev         = NULL;
	st->bus_id      = cfg->bus_id;
	st->be_data     = d;
	caps_out->flags = 0u;
	return ALP_OK;
}

/**
 * @brief Begin streaming.
 *
 * snd_pcm_prepare() moves the PCM out of SETUP into PREPARED so the
 * first writei/readi can run.  For a CAPTURE stream snd_pcm_start()
 * additionally kicks the DAI so frames start accumulating; a PLAYBACK
 * stream auto-starts on the first writei once the buffer threshold is
 * reached, so an explicit start there is unnecessary (and mirrors
 * audio_yocto.c's out path).
 */
static alp_status_t y_start(alp_i2s_backend_state_t *st)
{
	y_i2s_data_t *d = (y_i2s_data_t *)st->be_data;
	if (d == NULL || d->pcm == NULL) return ALP_ERR_NOT_READY;

	int rc = snd_pcm_prepare(d->pcm);
	if (rc < 0) return _alsa_to_alp(rc);

	if (d->capture) {
		rc = snd_pcm_start(d->pcm);
		if (rc < 0) return _alsa_to_alp(rc);
	}
	return ALP_OK;
}

/**
 * @brief Drain in-flight frames and stop the clock.
 *
 * A PLAYBACK stream uses snd_pcm_drain() so already-queued frames play
 * out before the clock stops (matches alp_i2s_stop's "drain" contract);
 * a CAPTURE stream uses snd_pcm_drop() to discard pending RX frames
 * immediately, since draining a capture stream has no meaning.
 */
static alp_status_t y_stop(alp_i2s_backend_state_t *st)
{
	y_i2s_data_t *d = (y_i2s_data_t *)st->be_data;
	if (d == NULL || d->pcm == NULL) return ALP_ERR_NOT_READY;

	int rc = d->capture ? snd_pcm_drop(d->pcm) : snd_pcm_drain(d->pcm);
	return _alsa_to_alp(rc);
}

/**
 * @brief Push a PCM block to the TX path via snd_pcm_writei().
 *
 * ALSA is frame-oriented; the dispatcher hands a byte count, so divide
 * by frame_bytes to get the interleaved frame count.  snd_pcm_writei()
 * may legally queue FEWER frames than requested (a signal, or partial
 * buffer room), so the write loops until every frame is queued or an
 * unrecoverable error surfaces -- the dispatcher contract is
 * all-or-nothing, never a silent partial write (issue #245).  An xrun
 * (-EPIPE) is recovered transparently via snd_pcm_recover() so the
 * next write succeeds on a healthy DAI.  timeout_ms gates each wait
 * for buffer room via snd_pcm_wait() (clamped to INT_MAX -- see
 * _clamp_wait_ms); 0 means "don't block".
 */
static alp_status_t
y_write(alp_i2s_backend_state_t *st, const void *block, size_t bytes, uint32_t timeout_ms)
{
	y_i2s_data_t *d = (y_i2s_data_t *)st->be_data;
	if (d == NULL || d->pcm == NULL) return ALP_ERR_NOT_READY;
	if (d->capture) return ALP_ERR_INVAL; /* wrong direction */
	if (block == NULL && bytes > 0u) return ALP_ERR_INVAL;
	if (d->frame_bytes == 0u) return ALP_ERR_NOT_READY;
	if (bytes == 0u) return ALP_OK;

	snd_pcm_uframes_t remaining = (snd_pcm_uframes_t)(bytes / d->frame_bytes);
	if (remaining == 0u) return ALP_ERR_INVAL; /* fewer than one frame */

	const uint8_t *pos     = (const uint8_t *)block;
	int            wait_ms = _clamp_wait_ms(timeout_ms);

	while (remaining > 0u) {
		int rc = snd_pcm_wait(d->pcm, wait_ms);
		if (rc == 0) return ALP_ERR_TIMEOUT;
		if (rc < 0) return _alsa_to_alp(rc);

		snd_pcm_sframes_t wrote = snd_pcm_writei(d->pcm, pos, remaining);
		if (wrote < 0) {
			int rec = snd_pcm_recover(d->pcm, (int)wrote, 1 /* silent */);
			if (rec < 0) return _alsa_to_alp(rec);
			return ALP_ERR_IO;
		}
		/* Partial write: advance past the queued frames and loop for
		 * the rest.  wrote is bounded by remaining, so no overshoot. */
		pos += (size_t)wrote * d->frame_bytes;
		remaining -= (snd_pcm_uframes_t)wrote;
	}
	return ALP_OK;
}

/**
 * @brief Pull the next PCM block from the RX path via snd_pcm_readi().
 *
 * Reports the byte count actually copied via @p bytes_out (frames read
 * × frame_bytes), never more than @p bytes.  Xruns (-EPIPE /
 * -ESTRPIPE) are recovered via snd_pcm_recover().  timeout_ms gates the
 * wait for available frames via snd_pcm_wait() (clamped to INT_MAX --
 * see _clamp_wait_ms); 0 means "don't block".
 */
static alp_status_t y_read(
    alp_i2s_backend_state_t *st, void *block, size_t bytes, size_t *bytes_out, uint32_t timeout_ms)
{
	if (bytes_out != NULL) *bytes_out = 0u;

	y_i2s_data_t *d = (y_i2s_data_t *)st->be_data;
	if (d == NULL || d->pcm == NULL) return ALP_ERR_NOT_READY;
	if (!d->capture) return ALP_ERR_INVAL; /* wrong direction */
	if (block == NULL && bytes > 0u) return ALP_ERR_INVAL;
	if (d->frame_bytes == 0u) return ALP_ERR_NOT_READY;
	if (bytes == 0u) return ALP_OK;

	snd_pcm_uframes_t frames = (snd_pcm_uframes_t)(bytes / d->frame_bytes);
	if (frames == 0u) return ALP_ERR_INVAL; /* dest < one frame */

	int rc = snd_pcm_wait(d->pcm, _clamp_wait_ms(timeout_ms));
	if (rc == 0) return ALP_ERR_TIMEOUT;
	if (rc < 0) return _alsa_to_alp(rc);

	snd_pcm_sframes_t got = snd_pcm_readi(d->pcm, block, frames);
	if (got < 0) {
		int rec = snd_pcm_recover(d->pcm, (int)got, 1 /* silent */);
		if (rec < 0) return _alsa_to_alp(rec);
		return ALP_ERR_IO;
	}
	if (bytes_out != NULL) *bytes_out = (size_t)got * d->frame_bytes;
	return ALP_OK;
}

/** @brief Drain (TX) / drop (RX), close the PCM, free the sidecar. */
static void y_close(alp_i2s_backend_state_t *st)
{
	y_i2s_data_t *d = (y_i2s_data_t *)st->be_data;
	if (d != NULL) {
		if (d->pcm != NULL) {
			if (d->capture) {
				(void)snd_pcm_drop(d->pcm);
			} else {
				(void)snd_pcm_drain(d->pcm);
			}
			(void)snd_pcm_close(d->pcm);
			d->pcm = NULL;
		}
		free(d);
		st->be_data = NULL;
	}
}

static const alp_i2s_ops_t _ops = {
	.open  = y_open,
	.start = y_start,
	.stop  = y_stop,
	.write = y_write,
	.read  = y_read,
	.close = y_close,
};

ALP_BACKEND_REGISTER(i2s,
                     yocto_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "linux",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

#endif /* __linux__ */
