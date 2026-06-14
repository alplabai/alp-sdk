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
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <alsa/asoundlib.h>

#include <alp/audio.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "audio_ops.h"

/* Per-handle backend data: the open ALSA PCM handle plus the derived
 * frame geometry.  Boxed onto the heap so the void* be_data slot in the
 * audio in/out backend state owns it. */
typedef struct {
    snd_pcm_t         *pcm;
    bool               started;
    uint16_t           frames_per_block;
    uint8_t            channels;
    uint8_t            sample_bytes; /* per-channel bytes per frame element */
    alp_audio_format_t format;
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
static alp_status_t y_in_open(const alp_audio_config_t *cfg, alp_audio_in_backend_state_t *state,
                              alp_capabilities_t *caps_out)
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

    state->be_data      = d;
    caps_out->flags     = 0u;
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
static alp_status_t y_in_read(alp_audio_in_backend_state_t *state, void *buf, size_t frames,
                              size_t *out_frames, uint32_t timeout_ms)
{
    if (out_frames != NULL) *out_frames = 0;
    y_audio_data_t *d = (y_audio_data_t *)state->be_data;
    if (d == NULL) return ALP_ERR_NOT_READY;
    if (buf == NULL && frames > 0) return ALP_ERR_INVAL;
    if (frames == 0) return ALP_OK;

    /* snd_pcm_wait returns 1 when frames are available, 0 on timeout,
     * negative on error.  timeout_ms == 0 means "don't wait" -- pass
     * 0 through; ALSA treats 0 as immediate return. */
    int rc = snd_pcm_wait(d->pcm, (int)timeout_ms);
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
static alp_status_t y_out_open(const alp_audio_config_t *cfg, alp_audio_out_backend_state_t *state,
                               alp_capabilities_t *caps_out)
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

    state->volume       = 255u; /* full volume by default */
    state->be_data      = d;
    caps_out->flags     = 0u;
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
static void apply_software_volume(const y_audio_data_t *d, uint8_t volume, void *buf, size_t frames)
{
    if (volume == 255u || frames == 0) return;
    if (d->format != ALP_AUDIO_FMT_S16_LE) return;
    int16_t *samples = (int16_t *)buf;
    size_t   n       = frames * d->channels;
    int32_t  scale   = volume; /* 0..255 */
    for (size_t i = 0; i < n; ++i) {
        int32_t s  = (int32_t)samples[i] * scale / 255;
        samples[i] = (int16_t)s;
    }
}

/**
 * @brief Wait for driver readiness, apply software volume, and write.
 *
 * Recovers transparently from xruns via snd_pcm_recover.  The software
 * volume scale runs on a mutable scratch copy only when volume is not
 * full and the format is S16_LE; the common full-volume path is
 * zero-copy.
 */
static alp_status_t y_out_write(alp_audio_out_backend_state_t *state, const void *buf,
                                size_t frames, size_t *out_frames, uint32_t timeout_ms)
{
    if (out_frames != NULL) *out_frames = 0;
    y_audio_data_t *d = (y_audio_data_t *)state->be_data;
    if (d == NULL) return ALP_ERR_NOT_READY;
    if (buf == NULL && frames > 0) return ALP_ERR_INVAL;
    if (frames == 0) return ALP_OK;

    int rc = snd_pcm_wait(d->pcm, (int)timeout_ms);
    if (rc == 0) return ALP_ERR_TIMEOUT;
    if (rc < 0) return alsa_to_alp(rc);

    /* Software volume needs to scale a mutable copy.  Most apps run
     * at full volume in tight loops; the const-cast + in-place
     * scaling is gated on the volume-not-max check so the common
     * path is zero-copy. */
    void    *write_buf = (void *)(uintptr_t)buf;
    uint8_t *scratch   = NULL;
    if (state->volume != 255u && d->format == ALP_AUDIO_FMT_S16_LE) {
        size_t bytes = frames * d->channels * d->sample_bytes;
        scratch      = (uint8_t *)malloc(bytes);
        if (scratch == NULL) return ALP_ERR_NOMEM;
        memcpy(scratch, buf, bytes);
        apply_software_volume(d, state->volume, scratch, frames);
        write_buf = scratch;
    }

    snd_pcm_sframes_t wrote = snd_pcm_writei(d->pcm, write_buf, frames);
    free(scratch);
    if (wrote < 0) {
        int rec = snd_pcm_recover(d->pcm, (int)wrote, 1 /* silent */);
        if (rec < 0) return alsa_to_alp(rec);
        return ALP_ERR_IO;
    }
    if (out_frames != NULL) *out_frames = (size_t)wrote;
    return ALP_OK;
}

/** @brief Record the 0..255 linear volume on the dispatcher out state. */
static alp_status_t y_out_set_volume(alp_audio_out_backend_state_t *state, uint8_t vol)
{
    if (state->be_data == NULL) return ALP_ERR_NOT_READY;
    state->volume = vol;
    return ALP_OK;
}

/** @brief Drain, close the playback PCM, and free the per-handle box. */
static void y_out_close(alp_audio_out_backend_state_t *state)
{
    y_audio_data_t *d = (y_audio_data_t *)state->be_data;
    if (d == NULL) return;
    if (d->pcm != NULL) {
        (void)snd_pcm_drain(d->pcm);
        (void)snd_pcm_close(d->pcm);
        d->pcm = NULL;
    }
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

ALP_BACKEND_REGISTER(audio, yocto_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "linux",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

#endif /* __linux__ */
