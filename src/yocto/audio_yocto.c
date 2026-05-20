/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux userspace audio backend for <alp/audio.h>'s
 * alp_audio_in_* / alp_audio_out_* surface.
 *
 * Binds against ALSA's libasound on a Yocto / Linux target.  Built
 * only when CMake's pkg_check_modules finds `alsa` (libasound2-dev
 * on Debian / Ubuntu; `alsa-lib` recipe on Yocto sysroots).
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
 * `alp_audio_out_set_volume` applies a *software* linear scale during
 * `alp_audio_out_write` rather than touching ALSA's mixer (which is
 * a separate API with system-specific control naming).  Apps that
 * need hardware-mixer control should drive ALSA's `snd_mixer_*`
 * directly.
 *
 * Sample formats
 * --------------
 * S16_LE / S24_LE / S32_LE map to the corresponding ALSA constants.
 * S24_LE is the 32-bit-container variant (`SND_PCM_FORMAT_S24_LE`)
 * which matches `<alp/audio.h>`'s documented "packed in 32-bit
 * slots" semantics.
 *
 * Compiled only on Linux hosts/targets, only when libasound is
 * present on the sysroot.
 */

#if !defined(__linux__)
#error "audio_yocto.c (yocto backend) requires a Linux target"
#endif

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <alsa/asoundlib.h>

#include "alp/audio.h"
#include "alp/peripheral.h"
#include "alp_internal.h"

#ifndef ALP_SDK_YOCTO_MAX_AUDIO_IN_HANDLES
#define ALP_SDK_YOCTO_MAX_AUDIO_IN_HANDLES 2
#endif

#ifndef ALP_SDK_YOCTO_MAX_AUDIO_OUT_HANDLES
#define ALP_SDK_YOCTO_MAX_AUDIO_OUT_HANDLES 2
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* ------------------------------------------------------------------ */
/* Internal handle structures                                          */
/* ------------------------------------------------------------------ */

struct alp_audio_in {
    bool         in_use;
    snd_pcm_t   *pcm;
    bool         started;
    uint16_t     frames_per_block;
    uint8_t      channels;
    uint8_t      sample_bytes;    /* per-channel bytes per frame element */
    alp_audio_format_t format;
};

struct alp_audio_out {
    bool         in_use;
    snd_pcm_t   *pcm;
    bool         started;
    uint16_t     frames_per_block;
    uint8_t      channels;
    uint8_t      sample_bytes;
    uint8_t      volume;          /* 0..255 software linear scale */
    alp_audio_format_t format;
};

static struct alp_audio_in  g_in_pool[ALP_SDK_YOCTO_MAX_AUDIO_IN_HANDLES];
static struct alp_audio_out g_out_pool[ALP_SDK_YOCTO_MAX_AUDIO_OUT_HANDLES];

/* ------------------------------------------------------------------ */
/* Pool helpers                                                        */
/* ------------------------------------------------------------------ */

static struct alp_audio_in *in_pool_acquire(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_in_pool); ++i) {
        if (!g_in_pool[i].in_use) {
            memset(&g_in_pool[i], 0, sizeof(g_in_pool[i]));
            g_in_pool[i].in_use = true;
            return &g_in_pool[i];
        }
    }
    return NULL;
}

static void in_pool_release(struct alp_audio_in *h)
{
    if (h == NULL) {
        return;
    }
    if (h->pcm != NULL) {
        (void)snd_pcm_drop(h->pcm);
        (void)snd_pcm_close(h->pcm);
        h->pcm = NULL;
    }
    h->in_use = false;
}

static struct alp_audio_out *out_pool_acquire(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_out_pool); ++i) {
        if (!g_out_pool[i].in_use) {
            memset(&g_out_pool[i], 0, sizeof(g_out_pool[i]));
            g_out_pool[i].in_use = true;
            g_out_pool[i].volume = 255;  /* full volume by default */
            return &g_out_pool[i];
        }
    }
    return NULL;
}

static void out_pool_release(struct alp_audio_out *h)
{
    if (h == NULL) {
        return;
    }
    if (h->pcm != NULL) {
        (void)snd_pcm_drain(h->pcm);
        (void)snd_pcm_close(h->pcm);
        h->pcm = NULL;
    }
    h->in_use = false;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static alp_status_t alsa_to_alp(int rc)
{
    /* snd_pcm_* APIs return negative errno on failure. */
    if (rc >= 0) return ALP_OK;
    switch (-rc) {
    case EINVAL:    return ALP_ERR_INVAL;
    case ENOENT:
    case ENODEV:    return ALP_ERR_NOT_READY;
    case EBUSY:     return ALP_ERR_BUSY;
    case EAGAIN:
    case ETIMEDOUT: return ALP_ERR_TIMEOUT;
    case ENOMEM:    return ALP_ERR_NOMEM;
    case ENOSYS:
    case ENOTSUP:   return ALP_ERR_NOSUPPORT;
    default:        return ALP_ERR_IO;
    }
}

static int format_bytes(alp_audio_format_t f)
{
    switch (f) {
    case ALP_AUDIO_FMT_S16_LE: return 2;
    case ALP_AUDIO_FMT_S24_LE: return 4;  /* 32-bit container per alp/audio.h */
    case ALP_AUDIO_FMT_S32_LE: return 4;
    default:                    return 0;
    }
}

static snd_pcm_format_t to_alsa_format(alp_audio_format_t f)
{
    switch (f) {
    case ALP_AUDIO_FMT_S16_LE: return SND_PCM_FORMAT_S16_LE;
    case ALP_AUDIO_FMT_S24_LE: return SND_PCM_FORMAT_S24_LE;
    case ALP_AUDIO_FMT_S32_LE: return SND_PCM_FORMAT_S32_LE;
    default:                   return SND_PCM_FORMAT_UNKNOWN;
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
    rc = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL);
    if (rc < 0) return alsa_to_alp(rc);

    snd_pcm_uframes_t period = cfg->frames_per_block;
    rc = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL);
    if (rc < 0) return alsa_to_alp(rc);

    /* Buffer = 4 periods.  Generous enough to absorb scheduling
     * jitter without bloating RAM; apps that need different sizing
     * tune frames_per_block, which is the period unit. */
    snd_pcm_uframes_t buf = period * 4u;
    rc = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buf);
    if (rc < 0) return alsa_to_alp(rc);

    rc = snd_pcm_hw_params(pcm, hw);
    if (rc < 0) return alsa_to_alp(rc);

    return ALP_OK;
}

/* ================================================================== */
/* Audio input                                                          */
/* ================================================================== */

alp_audio_in_t *alp_audio_in_open(const alp_audio_config_t *cfg)
{
    if (cfg == NULL) {
        alp_internal_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (format_bytes(cfg->format) == 0) {
        alp_internal_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

    char dev[32];
    int  n = resolve_device_name(cfg->peripheral_id, dev, sizeof(dev));
    if (n < 0 || (size_t)n >= sizeof(dev)) {
        alp_internal_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

    struct alp_audio_in *h = in_pool_acquire();
    if (h == NULL) {
        alp_internal_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }

    int rc = snd_pcm_open(&h->pcm, dev, SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) {
        alp_internal_set_last_error(alsa_to_alp(rc));
        in_pool_release(h);
        return NULL;
    }

    alp_status_t s = configure_pcm(h->pcm, cfg);
    if (s != ALP_OK) {
        alp_internal_set_last_error(s);
        in_pool_release(h);
        return NULL;
    }

    h->channels         = cfg->channels;
    h->sample_bytes     = (uint8_t)format_bytes(cfg->format);
    h->format           = cfg->format;
    h->frames_per_block = cfg->frames_per_block;
    return h;
}

alp_status_t alp_audio_in_start(alp_audio_in_t *in)
{
    if (in == NULL || !in->in_use) return ALP_ERR_NOT_READY;
    if (in->started) return ALP_OK;
    int rc = snd_pcm_prepare(in->pcm);
    if (rc < 0) return alsa_to_alp(rc);
    rc = snd_pcm_start(in->pcm);
    if (rc < 0) return alsa_to_alp(rc);
    in->started = true;
    return ALP_OK;
}

alp_status_t alp_audio_in_stop(alp_audio_in_t *in)
{
    if (in == NULL || !in->in_use) return ALP_ERR_NOT_READY;
    if (!in->started) return ALP_OK;
    int rc = snd_pcm_drop(in->pcm);
    in->started = false;
    return alsa_to_alp(rc);
}

alp_status_t alp_audio_in_read(alp_audio_in_t *in,
                               void *buf, size_t frames,
                               size_t *out_frames,
                               uint32_t timeout_ms)
{
    if (out_frames != NULL) *out_frames = 0;
    if (in == NULL || !in->in_use) return ALP_ERR_NOT_READY;
    if (buf == NULL && frames > 0) return ALP_ERR_INVAL;
    if (frames == 0) return ALP_OK;

    /* snd_pcm_wait returns 1 when frames are available, 0 on timeout,
     * negative on error.  timeout_ms == 0 means "don't wait" -- pass
     * 0 through; ALSA treats 0 as immediate return. */
    int rc = snd_pcm_wait(in->pcm, (int)timeout_ms);
    if (rc == 0) return ALP_ERR_TIMEOUT;
    if (rc < 0)  return alsa_to_alp(rc);

    snd_pcm_sframes_t got = snd_pcm_readi(in->pcm, buf, frames);
    if (got < 0) {
        /* Recover from xruns transparently -- the next read will
         * succeed if the underlying driver is healthy. */
        int rec = snd_pcm_recover(in->pcm, (int)got, 1 /* silent */);
        if (rec < 0) return alsa_to_alp(rec);
        return ALP_ERR_IO;
    }
    if (out_frames != NULL) *out_frames = (size_t)got;
    return ALP_OK;
}

void alp_audio_in_close(alp_audio_in_t *in)
{
    in_pool_release(in);
}

/* ================================================================== */
/* Audio output                                                         */
/* ================================================================== */

alp_audio_out_t *alp_audio_out_open(const alp_audio_config_t *cfg)
{
    if (cfg == NULL) {
        alp_internal_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (format_bytes(cfg->format) == 0) {
        alp_internal_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

    char dev[32];
    int  n = resolve_device_name(cfg->peripheral_id, dev, sizeof(dev));
    if (n < 0 || (size_t)n >= sizeof(dev)) {
        alp_internal_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

    struct alp_audio_out *h = out_pool_acquire();
    if (h == NULL) {
        alp_internal_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }

    int rc = snd_pcm_open(&h->pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        alp_internal_set_last_error(alsa_to_alp(rc));
        out_pool_release(h);
        return NULL;
    }

    alp_status_t s = configure_pcm(h->pcm, cfg);
    if (s != ALP_OK) {
        alp_internal_set_last_error(s);
        out_pool_release(h);
        return NULL;
    }

    h->channels         = cfg->channels;
    h->sample_bytes     = (uint8_t)format_bytes(cfg->format);
    h->format           = cfg->format;
    h->frames_per_block = cfg->frames_per_block;
    return h;
}

alp_status_t alp_audio_out_start(alp_audio_out_t *out)
{
    if (out == NULL || !out->in_use) return ALP_ERR_NOT_READY;
    if (out->started) return ALP_OK;
    int rc = snd_pcm_prepare(out->pcm);
    if (rc < 0) return alsa_to_alp(rc);
    out->started = true;
    return ALP_OK;
}

alp_status_t alp_audio_out_stop(alp_audio_out_t *out)
{
    if (out == NULL || !out->in_use) return ALP_ERR_NOT_READY;
    if (!out->started) return ALP_OK;
    int rc = snd_pcm_drain(out->pcm);
    out->started = false;
    return alsa_to_alp(rc);
}

/* Apply the per-handle linear volume scale in place.  Operates on the
 * caller's pcm bytes.  When volume == 255 the scale is identity and
 * the loop short-circuits.  Performed only on S16_LE for v0.4 prep;
 * other formats pass through unmodified (a software scaler for S24
 * + S32 lands when the inference / codec paths exercise them). */
static void apply_software_volume(const struct alp_audio_out *h,
                                  void *buf, size_t frames)
{
    if (h->volume == 255u || frames == 0) return;
    if (h->format != ALP_AUDIO_FMT_S16_LE) return;
    int16_t *samples = (int16_t *)buf;
    size_t   n       = frames * h->channels;
    int32_t  scale   = h->volume;  /* 0..255 */
    for (size_t i = 0; i < n; ++i) {
        int32_t s = (int32_t)samples[i] * scale / 255;
        samples[i] = (int16_t)s;
    }
}

alp_status_t alp_audio_out_write(alp_audio_out_t *out,
                                 const void *buf, size_t frames,
                                 size_t *out_frames,
                                 uint32_t timeout_ms)
{
    if (out_frames != NULL) *out_frames = 0;
    if (out == NULL || !out->in_use) return ALP_ERR_NOT_READY;
    if (buf == NULL && frames > 0) return ALP_ERR_INVAL;
    if (frames == 0) return ALP_OK;

    int rc = snd_pcm_wait(out->pcm, (int)timeout_ms);
    if (rc == 0) return ALP_ERR_TIMEOUT;
    if (rc < 0)  return alsa_to_alp(rc);

    /* Software volume needs to scale a mutable copy.  Most apps run
     * at full volume in tight loops; the const-cast + in-place
     * scaling is gated on the volume-not-max check so the common
     * path is zero-copy. */
    void *write_buf = (void *)(uintptr_t)buf;
    uint8_t *scratch = NULL;
    if (out->volume != 255u && out->format == ALP_AUDIO_FMT_S16_LE) {
        size_t bytes = frames * out->channels * out->sample_bytes;
        scratch = (uint8_t *)malloc(bytes);
        if (scratch == NULL) return ALP_ERR_NOMEM;
        memcpy(scratch, buf, bytes);
        apply_software_volume(out, scratch, frames);
        write_buf = scratch;
    }

    snd_pcm_sframes_t wrote = snd_pcm_writei(out->pcm, write_buf, frames);
    free(scratch);
    if (wrote < 0) {
        int rec = snd_pcm_recover(out->pcm, (int)wrote, 1 /* silent */);
        if (rec < 0) return alsa_to_alp(rec);
        return ALP_ERR_IO;
    }
    if (out_frames != NULL) *out_frames = (size_t)wrote;
    return ALP_OK;
}

alp_status_t alp_audio_out_set_volume(alp_audio_out_t *out, uint8_t vol)
{
    if (out == NULL || !out->in_use) return ALP_ERR_NOT_READY;
    out->volume = vol;
    return ALP_OK;
}

void alp_audio_out_close(alp_audio_out_t *out)
{
    out_pool_release(out);
}
