/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Real Linux/Yocto audio_* driver-class backend.  Binds the alp_audio
 * dispatcher's combined in/out ops vtable to ALSA's libasound on a
 * Yocto / Linux target.  Registered at priority 100 with vendor
 * "linux"; the sw_fallback backend (priority 0) still wins on
 * non-Linux native_sim builds where this TU compiles to an empty
 * object.
 *
 * Built only when CMake's pkg_check_modules finds `alsa`
 * (libasound2-dev on Debian / Ubuntu; `alsa-lib` recipe on Yocto
 * sysroots).
 *
 * Selected on any silicon (silicon_ref "*") because the kernel PCM /
 * ALSA ABI is SoC-agnostic; the device-tree / kernel decides which
 * physical DAI backs each card.
 *
 * STATUS: real impl, Yocto-link + on-target run BENCH-UNVERIFIED (no
 *         ALSA sysroot / no real PCM device nodes in this environment).
 *
 * Device naming convention
 * ------------------------
 * `alp_audio_config_t.peripheral_id` maps to an ALSA PCM device name:
 *
 *     peripheral_id == 0 -> "default"            (per-user ALSA default;
 *                                                  honours /etc/asound.conf
 *                                                  + ~/.asoundrc)
 *     peripheral_id == N -> "hw:<N - 1>,0"       (card N-1, device 0)
 *
 * So `peripheral_id = 1` => `hw:0,0` (the first hardware card).  Apps
 * that need a non-default device subname can extend this mapping by
 * adding entries to the board's metadata; the SDK doesn't shoehorn
 * a free-form string into `peripheral_id` because every other
 * peripheral surface uses a numeric instance ID.
 *
 * Volume control
 * --------------
 * `out_set_volume` applies a *software* linear scale during
 * `out_write` rather than touching ALSA's mixer (which is a separate
 * API with system-specific control naming).  Apps that need
 * hardware-mixer control should drive ALSA's `snd_mixer_*` directly.
 * The scale runs on a per-handle scratch buffer allocated once at
 * open() (sized to one period) and reused by every write; a write
 * larger than one period is processed in that many bounded chunks
 * instead of growing the buffer or allocating per call (#632).  The
 * unity-volume (255) path skips the scratch buffer entirely and writes
 * the caller's buffer directly.
 *
 * Sample formats
 * --------------
 * S16_LE / S24_LE / S32_LE map to the corresponding ALSA constants.
 * S24_LE is the 32-bit-container variant (`SND_PCM_FORMAT_S24_LE`)
 * which matches `<alp/audio.h>`'s documented "packed in 32-bit
 * slots" semantics.
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
#include <time.h>

#include <alsa/asoundlib.h>

#include <alp/audio.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "audio_ops.h"

/* Per-handle backend data: the open ALSA PCM handle plus the derived
 * frame geometry.  Boxed onto the heap so the void* be_data slot in the
 * audio in/out backend state owns it.
 *
 * `scratch` is the bounded volume-scaling buffer for out_write (#632):
 * allocated once in y_out_open, sized to one period (frames_per_block),
 * reused by every write, and released exactly once in y_out_close. */
typedef struct {
	snd_pcm_t         *pcm;
	bool               started;
	uint16_t           frames_per_block;
	uint8_t            channels;
	uint8_t            sample_bytes; /* per-channel bytes per frame element */
	alp_audio_format_t format;
	uint8_t           *scratch;        /* out-only; NULL for in (capture) handles */
	uint16_t           scratch_frames; /* capacity of `scratch`, in frames */
} y_audio_data_t;

/* ------------------------------------------------------------------ */
/* Helpers (vendor-API bodies preserved verbatim from audio_yocto.c)   */
/* ------------------------------------------------------------------ */

/** @brief Map a negative ALSA errno (snd_pcm_* convention) to alp_status_t. */
static alp_status_t alsa_to_alp(int rc)
{
	/* snd_pcm_* APIs return negative errno on failure. */
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

/* Private test seam (issue #634): forward-declared so the definition below
 * has a prototype in this TU (-Wmissing-prototypes) without exporting it
 * through any public include/alp header -- see the doc comment on the
 * definition for why it's non-static. */
int alp_yocto_alsa_wait_arg(uint32_t timeout_ms);

/**
 * @brief Portable `timeout_ms` -> ALSA `snd_pcm_wait()` argument.
 *
 * Shared by capture and playback so the two paths can't drift apart.
 * Per the SDK-wide timeout contract (see `include/alp/peripheral.h`),
 * `0` means "poll once, don't wait" and `UINT32_MAX` means "block
 * forever" -- both map straight onto ALSA's own `0` / `-1` special
 * values.  Anything else is a bounded wait: clamp (rather than
 * silently truncate via a plain `(int)timeout_ms` cast) a finite value
 * above `INT_MAX` down to `INT_MAX` so a huge-but-finite caller budget
 * can never wrap into ALSA's negative "forever" sentinel.
 *
 * Not declared in any public header -- internal seam so hermetic tests
 * (tests/yocto/audio_alsa.c) can exercise every timeout class without
 * a real ALSA device, the same pattern as `alp_uart_read_fd_bounded`
 * in tests/yocto/peripheral_uart.c.
 */
int alp_yocto_alsa_wait_arg(uint32_t timeout_ms)
{
	if (timeout_ms == UINT32_MAX) return -1;
	if (timeout_ms > (uint32_t)INT_MAX) return INT_MAX;
	return (int)timeout_ms;
}

/* Monotonic milliseconds -- used to track one caller deadline across
 * the chunked out_write loop below. */
static uint64_t y_monotonic_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Remaining ALSA wait budget for the NEXT chunk, given the whole
 * call's converted budget (from alp_yocto_alsa_wait_arg) and how much
 * wall-clock time has elapsed since the call started.  `0` (don't
 * wait) and `-1` (forever) are timeless and pass through unchanged; a
 * finite budget shrinks toward (never below) zero as chunks consume
 * it, so a multi-chunk write honours ONE deadline for the whole call
 * instead of restarting the clock on every chunk. */
static int y_remaining_wait_ms(int call_budget_ms, uint64_t call_start_ms)
{
	if (call_budget_ms <= 0) return call_budget_ms;
	uint64_t elapsed = y_monotonic_ms() - call_start_ms;
	if (elapsed >= (uint64_t)call_budget_ms) return 0;
	return (int)((uint64_t)call_budget_ms - elapsed);
}

/** @brief Per-channel byte width for an alp_audio_format_t (0 = invalid). */
static int format_bytes(alp_audio_format_t f)
{
	switch (f) {
	case ALP_AUDIO_FMT_S16_LE:
		return 2;
	case ALP_AUDIO_FMT_S24_LE:
		return 4; /* 32-bit container per alp/audio.h */
	case ALP_AUDIO_FMT_S32_LE:
		return 4;
	default:
		return 0;
	}
}

/** @brief Translate an alp_audio_format_t to its ALSA PCM format constant. */
static snd_pcm_format_t to_alsa_format(alp_audio_format_t f)
{
	switch (f) {
	case ALP_AUDIO_FMT_S16_LE:
		return SND_PCM_FORMAT_S16_LE;
	case ALP_AUDIO_FMT_S24_LE:
		return SND_PCM_FORMAT_S24_LE;
	case ALP_AUDIO_FMT_S32_LE:
		return SND_PCM_FORMAT_S32_LE;
	default:
		return SND_PCM_FORMAT_UNKNOWN;
	}
}

/**
 * @brief Overflow-checked (frames, channels, sample_bytes) -> total PCM
 *        byte count.
 *
 * GHSA-p9jj-6w2g-pc4g: `y_out_open`'s scratch `malloc()` and the
 * volume-scaling write path previously derived their sizing from two
 * INDEPENDENT `frames * channels * sample_bytes` multiplications; a
 * wrapped `size_t` product let one compute a small allocation while the
 * other assumed a large one, an integer-overflow-to-heap-OOB-write
 * (CWE-190 -> CWE-122). Every caller that sizes an allocation, a
 * copy/scale bound, or an ALSA frame-count argument off a caller-supplied
 * `frames` MUST go through this one helper and use its single validated
 * result -- there is no second, independently-recomputed byte count
 * anywhere downstream of it.
 *
 * Also rejects a `frames` that would not survive the handoff to ALSA's
 * `snd_pcm_uframes_t` (narrower than `size_t` on some 32-bit targets), so
 * the same check protects the `snd_pcm_readi`/`snd_pcm_writei` frame-count
 * argument, not just the byte count.
 *
 * @return true and set *out_bytes on success; false (leaving *out_bytes
 *         unspecified) when the geometry can't be represented safely --
 *         callers must reject the request without allocating, copying,
 *         scaling, or touching ALSA.
 */
static bool
y_checked_total_bytes(size_t frames, uint8_t channels, uint8_t sample_bytes, size_t *out_bytes)
{
	if (channels == 0 || sample_bytes == 0) return false;
	if (frames > (size_t)(snd_pcm_uframes_t)-1) return false;

	size_t stride = (size_t)channels * (size_t)sample_bytes; /* <= 8 * 4: never overflows */
	if (frames != 0 && stride > SIZE_MAX / frames) return false;

	*out_bytes = frames * stride;
	return true;
}

/* Resolve a peripheral_id into the canonical ALSA device name.
 * Writes into the caller-supplied `out` buffer (capacity at least
 * 32 bytes is plenty for "hw:<N>,0"). */
static int resolve_device_name(uint32_t peripheral_id, char *out, size_t cap)
{
	if (peripheral_id == 0u) {
		return snprintf(out, cap, "default");
	}
	return snprintf(out, cap, "hw:%u,0", (unsigned)(peripheral_id - 1u));
}

/* Configure hw + sw params on a freshly-opened PCM handle.
 * Returns ALP_OK on success; an alp_status_t on failure. */
static alp_status_t configure_pcm(snd_pcm_t *pcm, const alp_audio_config_t *cfg)
{
	snd_pcm_format_t fmt = to_alsa_format(cfg->format);
	if (fmt == SND_PCM_FORMAT_UNKNOWN || cfg->channels == 0 || cfg->channels > 8 ||
	    cfg->sample_rate_hz == 0 || cfg->frames_per_block == 0) {
		return ALP_ERR_INVAL;
	}

	snd_pcm_hw_params_t *hw = NULL;
	snd_pcm_hw_params_alloca(&hw);
	int rc = snd_pcm_hw_params_any(pcm, hw);
	if (rc < 0) return alsa_to_alp(rc);

	rc = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (rc < 0) return alsa_to_alp(rc);

	rc = snd_pcm_hw_params_set_format(pcm, hw, fmt);
	if (rc < 0) return alsa_to_alp(rc);

	rc = snd_pcm_hw_params_set_channels(pcm, hw, cfg->channels);
	if (rc < 0) return alsa_to_alp(rc);

	unsigned int rate = cfg->sample_rate_hz;
	rc                = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL);
	if (rc < 0) return alsa_to_alp(rc);

	snd_pcm_uframes_t period = cfg->frames_per_block;
	rc                       = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL);
	if (rc < 0) return alsa_to_alp(rc);

	/* Buffer = 4 periods.  Generous enough to absorb scheduling
     * jitter without bloating RAM; apps that need different sizing
     * tune frames_per_block, which is the period unit. */
	snd_pcm_uframes_t buf = period * 4u;
	rc                    = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buf);
	if (rc < 0) return alsa_to_alp(rc);

	rc = snd_pcm_hw_params(pcm, hw);
	if (rc < 0) return alsa_to_alp(rc);

	return ALP_OK;
}

/* ================================================================== */
/* Audio input (microphone)                                            */
/* ================================================================== */

/**
 * @brief Open an ALSA capture PCM and stash it in the handle state.
 *
 * Resolves peripheral_id -> ALSA device name, opens
 * SND_PCM_STREAM_CAPTURE, and applies the shared hw-params via
 * @ref configure_pcm.  Frame geometry derived from the format is cached
 * on the boxed backend data.  caps stay 0 (ALSA exposes no queryable
 * cap bits here).
 */
static alp_status_t y_in_open(const alp_audio_config_t     *cfg,
                              alp_audio_in_backend_state_t *state,
                              alp_capabilities_t           *caps_out)
{
	if (cfg == NULL) return ALP_ERR_INVAL;
	if (format_bytes(cfg->format) == 0) return ALP_ERR_INVAL;

	char dev[32];
	int  n = resolve_device_name(cfg->peripheral_id, dev, sizeof(dev));
	if (n < 0 || (size_t)n >= sizeof(dev)) return ALP_ERR_INVAL;

	y_audio_data_t *d = (y_audio_data_t *)calloc(1, sizeof(*d));
	if (d == NULL) return ALP_ERR_NOMEM;

	int rc = snd_pcm_open(&d->pcm, dev, SND_PCM_STREAM_CAPTURE, 0);
	if (rc < 0) {
		free(d);
		return alsa_to_alp(rc);
	}

	alp_status_t s = configure_pcm(d->pcm, cfg);
	if (s != ALP_OK) {
		(void)snd_pcm_close(d->pcm);
		free(d);
		return s;
	}

	d->channels         = cfg->channels;
	d->sample_bytes     = (uint8_t)format_bytes(cfg->format);
	d->format           = cfg->format;
	d->frames_per_block = cfg->frames_per_block;

	state->be_data  = d;
	caps_out->flags = 0u;
	return ALP_OK;
}

/** @brief Prepare + start the capture stream (idempotent once started). */
static alp_status_t y_in_start(alp_audio_in_backend_state_t *state)
{
	y_audio_data_t *d = (y_audio_data_t *)state->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;
	if (d->started) return ALP_OK;
	int rc = snd_pcm_prepare(d->pcm);
	if (rc < 0) return alsa_to_alp(rc);
	rc = snd_pcm_start(d->pcm);
	if (rc < 0) return alsa_to_alp(rc);
	d->started = true;
	return ALP_OK;
}

/** @brief Drop in-flight capture frames and stop the stream. */
static alp_status_t y_in_stop(alp_audio_in_backend_state_t *state)
{
	y_audio_data_t *d = (y_audio_data_t *)state->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;
	if (!d->started) return ALP_OK;
	int rc     = snd_pcm_drop(d->pcm);
	d->started = false;
	return alsa_to_alp(rc);
}

/**
 * @brief Wait for and read interleaved capture frames into @p buf.
 *
 * Recovers transparently from xruns via snd_pcm_recover so the next
 * read succeeds when the underlying driver is healthy.
 */
static alp_status_t y_in_read(alp_audio_in_backend_state_t *state,
                              void                         *buf,
                              size_t                        frames,
                              size_t                       *out_frames,
                              uint32_t                      timeout_ms)
{
	if (out_frames != NULL) *out_frames = 0;
	y_audio_data_t *d = (y_audio_data_t *)state->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;
	if (buf == NULL && frames > 0) return ALP_ERR_INVAL;
	if (frames == 0) return ALP_OK;

	/* GHSA-p9jj-6w2g-pc4g: reject an unrepresentable frame count before
     * waiting on ALSA or touching the driver -- see y_checked_total_bytes. */
	size_t total_bytes;
	if (!y_checked_total_bytes(frames, d->channels, d->sample_bytes, &total_bytes)) {
		return ALP_ERR_INVAL;
	}

	/* snd_pcm_wait returns 1 when frames are available, 0 on timeout,
     * negative on error.  timeout_ms == 0 means "don't wait" -- pass
     * 0 through; ALSA treats 0 as immediate return.  UINT32_MAX means
     * "block forever" (-1); anything else is clamped, never truncated
     * into a negative value -- see alp_yocto_alsa_wait_arg. */
	int rc = snd_pcm_wait(d->pcm, alp_yocto_alsa_wait_arg(timeout_ms));
	if (rc == 0) return ALP_ERR_TIMEOUT;
	if (rc < 0) return alsa_to_alp(rc);

	snd_pcm_sframes_t got = snd_pcm_readi(d->pcm, buf, frames);
	if (got < 0) {
		/* Recover from xruns transparently -- the next read will
         * succeed if the underlying driver is healthy. */
		int rec = snd_pcm_recover(d->pcm, (int)got, 1 /* silent */);
		if (rec < 0) return alsa_to_alp(rec);
		return ALP_ERR_IO;
	}
	if (out_frames != NULL) *out_frames = (size_t)got;
	return ALP_OK;
}

/** @brief Drop, close the capture PCM, and free the per-handle box. */
static void y_in_close(alp_audio_in_backend_state_t *state)
{
	y_audio_data_t *d = (y_audio_data_t *)state->be_data;
	if (d == NULL) return;
	if (d->pcm != NULL) {
		(void)snd_pcm_drop(d->pcm);
		(void)snd_pcm_close(d->pcm);
		d->pcm = NULL;
	}
	free(d);
	state->be_data = NULL;
}

/* ================================================================== */
/* Audio output (speaker)                                              */
/* ================================================================== */

/**
 * @brief Open an ALSA playback PCM and stash it in the handle state.
 *
 * Mirror of @ref y_in_open for SND_PCM_STREAM_PLAYBACK.  The software
 * volume scale defaults to full (255) on the dispatcher-owned out
 * state; see @ref y_out_set_volume + @ref y_out_write.
 */
static alp_status_t y_out_open(const alp_audio_config_t      *cfg,
                               alp_audio_out_backend_state_t *state,
                               alp_capabilities_t            *caps_out)
{
	if (cfg == NULL) return ALP_ERR_INVAL;
	if (format_bytes(cfg->format) == 0) return ALP_ERR_INVAL;

	char dev[32];
	int  n = resolve_device_name(cfg->peripheral_id, dev, sizeof(dev));
	if (n < 0 || (size_t)n >= sizeof(dev)) return ALP_ERR_INVAL;

	y_audio_data_t *d = (y_audio_data_t *)calloc(1, sizeof(*d));
	if (d == NULL) return ALP_ERR_NOMEM;

	int rc = snd_pcm_open(&d->pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
	if (rc < 0) {
		free(d);
		return alsa_to_alp(rc);
	}

	alp_status_t s = configure_pcm(d->pcm, cfg);
	if (s != ALP_OK) {
		(void)snd_pcm_close(d->pcm);
		free(d);
		return s;
	}

	d->channels         = cfg->channels;
	d->sample_bytes     = (uint8_t)format_bytes(cfg->format);
	d->format           = cfg->format;
	d->frames_per_block = cfg->frames_per_block;

	/* Bounded per-handle volume-scaling scratch (#632): allocated once
     * here, sized to one period, reused by every out_write call.
     * Scaled writes larger than one period are processed in that many
     * chunks (see alp_yocto_alsa_out_write_core) instead of growing
     * this buffer or bouncing through a fresh malloc per write.
     *
     * GHSA-p9jj-6w2g-pc4g: sized through the SAME checked helper every
     * other path in this file uses, rather than an inline multiplication,
     * so the allocation can never disagree with the copy/scale bound
     * derived from it. */
	size_t scratch_bytes;
	if (!y_checked_total_bytes(
	        cfg->frames_per_block, d->channels, d->sample_bytes, &scratch_bytes)) {
		(void)snd_pcm_close(d->pcm);
		free(d);
		return ALP_ERR_INVAL;
	}
	d->scratch = (uint8_t *)malloc(scratch_bytes);
	if (d->scratch == NULL) {
		(void)snd_pcm_close(d->pcm);
		free(d);
		return ALP_ERR_NOMEM;
	}
	d->scratch_frames = cfg->frames_per_block;

	state->volume   = 255u; /* full volume by default */
	state->be_data  = d;
	caps_out->flags = 0u;
	return ALP_OK;
}

/** @brief Prepare the playback stream (idempotent once started). */
static alp_status_t y_out_start(alp_audio_out_backend_state_t *state)
{
	y_audio_data_t *d = (y_audio_data_t *)state->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;
	if (d->started) return ALP_OK;
	int rc = snd_pcm_prepare(d->pcm);
	if (rc < 0) return alsa_to_alp(rc);
	d->started = true;
	return ALP_OK;
}

/** @brief Drain pending playback frames and stop the stream. */
static alp_status_t y_out_stop(alp_audio_out_backend_state_t *state)
{
	y_audio_data_t *d = (y_audio_data_t *)state->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;
	if (!d->started) return ALP_OK;
	int rc     = snd_pcm_drain(d->pcm);
	d->started = false;
	return alsa_to_alp(rc);
}

/* Apply the per-handle linear volume scale in place.  Operates on the
 * caller's pcm bytes.  When volume == 255 the scale is identity and
 * the loop short-circuits.  Performed only on S16_LE for v0.4 prep;
 * other formats pass through unmodified (a software scaler for S24
 * + S32 lands when the inference / codec paths exercise them). */
static void apply_software_volume(alp_audio_format_t format,
                                  uint8_t            channels,
                                  uint8_t            volume,
                                  void              *buf,
                                  size_t             frames)
{
	if (volume == 255u || frames == 0) return;
	if (format != ALP_AUDIO_FMT_S16_LE) return;
	int16_t *samples = (int16_t *)buf;
	size_t   n       = frames * channels;
	int32_t  scale   = volume; /* 0..255 */
	for (size_t i = 0; i < n; ++i) {
		int32_t s  = (int32_t)samples[i] * scale / 255;
		samples[i] = (int16_t)s;
	}
}

/* Low-level ALSA entry points the write core drives, indirected so
 * hermetic tests can swap in an in-memory fake PCM (no real ALSA
 * device needed) while production always binds the real functions. */
typedef int (*y_alsa_wait_fn)(snd_pcm_t *pcm, int timeout_ms);
typedef snd_pcm_sframes_t (*y_alsa_writei_fn)(snd_pcm_t        *pcm,
                                              const void       *buf,
                                              snd_pcm_uframes_t frames);

/* Private test seam (issue #634): forward-declared so the definition below
 * has a prototype in this TU (-Wmissing-prototypes) without exporting it
 * through any public include/alp header -- see the doc comment on the
 * definition for why it's non-static. */
alp_status_t alp_yocto_alsa_out_write_core(snd_pcm_t         *pcm,
                                           alp_audio_format_t format,
                                           uint8_t            channels,
                                           uint8_t            sample_bytes,
                                           uint8_t            volume,
                                           uint8_t           *scratch,
                                           uint16_t           scratch_frames,
                                           const void        *buf,
                                           size_t             frames,
                                           size_t            *out_frames,
                                           uint32_t           timeout_ms,
                                           y_alsa_wait_fn     wait_fn,
                                           y_alsa_writei_fn   writei_fn);

/**
 * @brief Allocation-free chunked write core (#632).
 *
 * Walks @p frames in chunks bounded by @p scratch_frames -- the
 * unscaled (volume == 255) path never needs the scratch buffer at all
 * and writes the caller's buffer directly (zero-copy, one chunk);
 * the scaled path copies+scales each chunk into the CALLER-OWNED
 * @p scratch buffer before writing it.  Because this function never
 * owns or allocates a buffer of its own -- `scratch`/`scratch_frames`
 * are supplied by the caller (the per-handle scratch allocated once in
 * y_out_open) -- it has no code path that can allocate: the "no
 * allocation after open" contract holds by construction, not by
 * convention.
 *
 * One deadline covers the whole call: @p timeout_ms is converted ONCE
 * (via @ref alp_yocto_alsa_wait_arg) and the remaining budget shrinks
 * across chunks (@ref y_remaining_wait_ms) rather than resetting on
 * every chunk, so a slow chunk can't multiply the caller's requested
 * wait.  Partial ALSA writes (@p writei_fn returning fewer frames than
 * asked) accumulate into @p out_frames and the loop simply asks for
 * the remainder next iteration.
 *
 * Not declared in any public header -- internal seam so hermetic
 * tests (tests/yocto/audio_alsa.c) can drive the whole algorithm
 * against a fake ALSA layer without a real PCM device.
 *
 * GHSA-p9jj-6w2g-pc4g: @p frames is rejected via @ref y_checked_total_bytes
 * BEFORE this function waits on ALSA, allocates, copies, or scales -- on
 * BOTH the scaled and the unity-volume (@p scale == false) fast path, so a
 * `frames` that cannot be represented safely never reaches @p writei_fn.
 * `frames_done` and `chunk` are bounded by the already-validated @p frames,
 * so `frames_done * stride` / `chunk * stride` below can't overflow either.
 */
alp_status_t alp_yocto_alsa_out_write_core(snd_pcm_t         *pcm,
                                           alp_audio_format_t format,
                                           uint8_t            channels,
                                           uint8_t            sample_bytes,
                                           uint8_t            volume,
                                           uint8_t           *scratch,
                                           uint16_t           scratch_frames,
                                           const void        *buf,
                                           size_t             frames,
                                           size_t            *out_frames,
                                           uint32_t           timeout_ms,
                                           y_alsa_wait_fn     wait_fn,
                                           y_alsa_writei_fn   writei_fn)
{
	if (out_frames != NULL) *out_frames = 0;
	if (buf == NULL && frames > 0) return ALP_ERR_INVAL;
	if (frames == 0) return ALP_OK;

	/* GHSA-p9jj-6w2g-pc4g: reject an unrepresentable frame count up
     * front, on EVERY path (scaled and unity-volume alike) -- before any
     * ALSA wait, allocation, copy, or scaling happens below. */
	size_t total_bytes;
	if (!y_checked_total_bytes(frames, channels, sample_bytes, &total_bytes)) {
		return ALP_ERR_INVAL;
	}

	bool scale =
	    (volume != 255u && format == ALP_AUDIO_FMT_S16_LE && scratch != NULL && scratch_frames > 0);
	size_t stride = (size_t)channels * sample_bytes;

	int      budget_ms = alp_yocto_alsa_wait_arg(timeout_ms);
	uint64_t start_ms  = y_monotonic_ms();

	const uint8_t *src         = (const uint8_t *)buf;
	size_t         frames_done = 0;

	while (frames_done < frames) {
		size_t chunk = frames - frames_done;
		if (scale && chunk > scratch_frames) chunk = scratch_frames;

		int wait_ms = y_remaining_wait_ms(budget_ms, start_ms);
		int rc      = wait_fn(pcm, wait_ms);
		if (rc == 0) {
			if (out_frames != NULL) *out_frames = frames_done;
			return (frames_done > 0) ? ALP_OK : ALP_ERR_TIMEOUT;
		}
		if (rc < 0) {
			if (out_frames != NULL) *out_frames = frames_done;
			return alsa_to_alp(rc);
		}

		const void *chunk_buf = src + frames_done * stride;
		if (scale) {
			memcpy(scratch, chunk_buf, chunk * stride);
			apply_software_volume(format, channels, volume, scratch, chunk);
			chunk_buf = scratch;
		}

		snd_pcm_sframes_t wrote = writei_fn(pcm, chunk_buf, chunk);
		if (wrote < 0) {
			int rec = snd_pcm_recover(pcm, (int)wrote, 1 /* silent */);
			if (out_frames != NULL) *out_frames = frames_done;
			if (rec < 0) return alsa_to_alp(rec);
			return ALP_ERR_IO;
		}
		frames_done += (size_t)wrote;
	}

	if (out_frames != NULL) *out_frames = frames_done;
	return ALP_OK;
}

/**
 * @brief Wait for driver readiness, apply software volume, and write.
 *
 * Thin production binding of @ref alp_yocto_alsa_out_write_core to the
 * real ALSA calls and this handle's per-open scratch buffer.
 */
static alp_status_t y_out_write(alp_audio_out_backend_state_t *state,
                                const void                    *buf,
                                size_t                         frames,
                                size_t                        *out_frames,
                                uint32_t                       timeout_ms)
{
	if (out_frames != NULL) *out_frames = 0;
	y_audio_data_t *d = (y_audio_data_t *)state->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;

	return alp_yocto_alsa_out_write_core(d->pcm,
	                                     d->format,
	                                     d->channels,
	                                     d->sample_bytes,
	                                     state->volume,
	                                     d->scratch,
	                                     d->scratch_frames,
	                                     buf,
	                                     frames,
	                                     out_frames,
	                                     timeout_ms,
	                                     snd_pcm_wait,
	                                     snd_pcm_writei);
}

/** @brief Record the 0..255 linear volume on the dispatcher out state. */
static alp_status_t y_out_set_volume(alp_audio_out_backend_state_t *state, uint8_t vol)
{
	if (state->be_data == NULL) return ALP_ERR_NOT_READY;
	state->volume = vol;
	return ALP_OK;
}

/** @brief Drain, close the playback PCM, and free the per-handle box
 *         (including the volume-scaling scratch, exactly once). */
static void y_out_close(alp_audio_out_backend_state_t *state)
{
	y_audio_data_t *d = (y_audio_data_t *)state->be_data;
	if (d == NULL) return;
	if (d->pcm != NULL) {
		(void)snd_pcm_drain(d->pcm);
		(void)snd_pcm_close(d->pcm);
		d->pcm = NULL;
	}
	free(d->scratch);
	d->scratch = NULL;
	free(d);
	state->be_data = NULL;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const alp_audio_ops_t _ops = {
	.in_open        = y_in_open,
	.in_start       = y_in_start,
	.in_stop        = y_in_stop,
	.in_read        = y_in_read,
	.in_close       = y_in_close,
	.out_open       = y_out_open,
	.out_start      = y_out_start,
	.out_stop       = y_out_stop,
	.out_write      = y_out_write,
	.out_set_volume = y_out_set_volume,
	.out_close      = y_out_close,
};

ALP_BACKEND_REGISTER(audio,
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
