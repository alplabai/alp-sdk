/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/audio.h> -- PDM input + I2S output.
 *
 * Replaces the v0.2 NOSUPPORT stub.  Two halves:
 *
 *   audio_in  : wraps Zephyr's `audio_dmic` API (CONFIG_AUDIO_DMIC).
 *               Optional ALP DSP chain runs between dmic_read and the
 *               caller's buffer: DC-block (1st-order high-pass) ->
 *               software gain (set via alp_audio_in_*).  AGC and
 *               resample land in v0.3 alongside <alp/security.h> --
 *               the chain hook stays here so v0.3 only adds passes.
 *
 *   audio_out : delegates to alp_i2s_* (already real).  The mapping
 *               is straight-through: peripheral_id -> bus_id,
 *               sample_rate_hz / channels / format / frames_per_block
 *               flow through verbatim.  Software volume applies a
 *               linear scale on every block before alp_i2s_write so
 *               apps without a separate codec gain pin still get a
 *               usable output volume control.
 *
 * Both halves are gated on per-feature CONFIG_ flags.  When OFF the
 * wrapper honours the v0.1 NULL-with-NOSUPPORT contract so apps that
 * link against <alp/audio.h> still compile under native_sim or any
 * other "no audio path" target.
 *
 * Optional buffering layer (v0.3 scaffolding, v0.4 first real use):
 * when CONFIG_ALP_SDK_USE_LWRB=y the wrapper may stage DMIC reads
 * through a MaJerle/lwrb ring buffer for byte-granular drain by the
 * caller, in addition to the k_mem_slab DMA scratch.  See
 * vendors/lwrb/README.md for the integration plan + Kconfig anchor.
 * No LwRB types leak through <alp/audio.h>; the ring stays an
 * implementation detail of this file.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "alp/audio.h"
#include "alp/i2s.h"
#include "handles.h"

#if defined(CONFIG_ALP_SDK_AUDIO_IN)
#include <zephyr/audio/dmic.h>
#endif

/* ------------------------------------------------------------------ */
/* Pool sizes                                                          */
/* ------------------------------------------------------------------ */

#ifndef CONFIG_ALP_SDK_MAX_AUDIO_IN_HANDLES
#define CONFIG_ALP_SDK_MAX_AUDIO_IN_HANDLES 1
#endif
#ifndef CONFIG_ALP_SDK_MAX_AUDIO_OUT_HANDLES
#define CONFIG_ALP_SDK_MAX_AUDIO_OUT_HANDLES 1
#endif

/* DMIC -> caller block size cap.  Apps that need larger blocks
 * override via CONFIG_ALP_SDK_AUDIO_BLOCK_BYTES. */
#ifndef CONFIG_ALP_SDK_AUDIO_BLOCK_BYTES
#define CONFIG_ALP_SDK_AUDIO_BLOCK_BYTES 4096
#endif

/* Memory-slab block count per audio_in.  4 blocks of double-buffer
 * latency is the conventional default; the DMIC driver consumes one
 * while the caller drains another, so 2 is the minimum. */
#ifndef CONFIG_ALP_SDK_AUDIO_IN_SLAB_BLOCKS
#define CONFIG_ALP_SDK_AUDIO_IN_SLAB_BLOCKS 4
#endif

/* ------------------------------------------------------------------ */
/* Internal handle structures                                          */
/* ------------------------------------------------------------------ */

struct alp_audio_in {
    bool               in_use;
    alp_audio_config_t cfg;
#if defined(CONFIG_ALP_SDK_AUDIO_IN)
    const struct device *dev;
    struct k_mem_slab    slab;
    uint8_t slab_buf[CONFIG_ALP_SDK_AUDIO_BLOCK_BYTES * CONFIG_ALP_SDK_AUDIO_IN_SLAB_BLOCKS];
    bool    started;
    /* 1st-order DC-block IIR state per channel.  alpha pinned at
     * 0.995 in 1.15 fixed-point (32 626/32 768) for a ~10 Hz cutoff
     * at 16 kHz -- inaudible but kills DC bias from PDM mics. */
    int32_t dc_x_prev[2];
    int32_t dc_y_prev[2];
#endif
};

struct alp_audio_out {
    bool               in_use;
    alp_audio_config_t cfg;
#if defined(CONFIG_ALP_SDK_AUDIO_OUT)
    alp_i2s_t *i2s;
    bool       started;
    uint16_t   volume_q8; /* Q8.8 software gain, 0x0100 = unity */
#endif
};

#if defined(CONFIG_ALP_SDK_AUDIO_IN)
static struct alp_audio_in  g_audio_in_pool[CONFIG_ALP_SDK_MAX_AUDIO_IN_HANDLES];

static struct alp_audio_in *audio_in_pool_acquire(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_audio_in_pool); ++i) {
        if (!g_audio_in_pool[i].in_use) {
            memset(&g_audio_in_pool[i], 0, sizeof(g_audio_in_pool[i]));
            g_audio_in_pool[i].in_use = true;
            return &g_audio_in_pool[i];
        }
    }
    return NULL;
}

static void audio_in_pool_release(struct alp_audio_in *h)
{
    if (h != NULL) h->in_use = false;
}
#endif /* CONFIG_ALP_SDK_AUDIO_IN */

#if defined(CONFIG_ALP_SDK_AUDIO_OUT)
static struct alp_audio_out  g_audio_out_pool[CONFIG_ALP_SDK_MAX_AUDIO_OUT_HANDLES];

static struct alp_audio_out *audio_out_pool_acquire(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_audio_out_pool); ++i) {
        if (!g_audio_out_pool[i].in_use) {
            memset(&g_audio_out_pool[i], 0, sizeof(g_audio_out_pool[i]));
            g_audio_out_pool[i].in_use = true;
            return &g_audio_out_pool[i];
        }
    }
    return NULL;
}

static void audio_out_pool_release(struct alp_audio_out *h)
{
    if (h != NULL) h->in_use = false;
}
#endif /* CONFIG_ALP_SDK_AUDIO_OUT */

#if defined(CONFIG_ALP_SDK_AUDIO_IN)
/* Only the DMIC path goes through Zephyr's errno surface; the
 * I2S delegation already returns alp_status_t.  Gate on _IN so
 * we don't trip -Werror=unused-function when only _OUT is on. */
static alp_status_t errno_to_alp(int err)
{
    switch (err) {
    case 0:
        return ALP_OK;
    case -EINVAL:
        return ALP_ERR_INVAL;
    case -EBUSY:
        return ALP_ERR_BUSY;
    case -EAGAIN:
    case -ETIMEDOUT:
        return ALP_ERR_TIMEOUT;
    case -EIO:
        return ALP_ERR_IO;
    case -ENOTSUP:
    case -ENOSYS:
        return ALP_ERR_NOSUPPORT;
    case -ENOMEM:
        return ALP_ERR_NOMEM;
    default:
        return ALP_ERR_IO;
    }
}
#endif

/* ------------------------------------------------------------------ */
/* Format helpers (only referenced from the gated-on paths)            */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_ALP_SDK_AUDIO_IN) || defined(CONFIG_ALP_SDK_AUDIO_OUT)
static uint8_t pcm_width_for(alp_audio_format_t f)
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

static size_t bytes_per_frame(const alp_audio_config_t *cfg)
{
    return (size_t)cfg->channels * (size_t)((pcm_width_for(cfg->format) + 7u) / 8u);
}
#endif

/* ================================================================== */
/* DMIC device resolution                                              */
/* ================================================================== */

#if defined(CONFIG_ALP_SDK_AUDIO_IN)

#define ALP_PDM_DEV_OR_NULL(idx)                                                                   \
    COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_pdm, idx))),                                   \
                (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_pdm, idx)))), (NULL))

static const struct device *const alp_pdm_devs[] = {
    ALP_PDM_DEV_OR_NULL(0),
    ALP_PDM_DEV_OR_NULL(1),
};

#endif /* CONFIG_ALP_SDK_AUDIO_IN */

/* ================================================================== */
/* DSP chain                                                           */
/* ================================================================== */

#if defined(CONFIG_ALP_SDK_AUDIO_IN)

/* DC-block: y[n] = x[n] - x[n-1] + alpha * y[n-1].  alpha = 0.995 in
 * Q15 (fixed point) so the multiply costs one >>15.  Operates in place
 * on signed 16-bit interleaved PCM. */
static void dc_block_s16(int16_t *frames, size_t n_frames, uint8_t channels, int32_t *x_prev,
                         int32_t *y_prev)
{
    const int32_t alpha_q15 = 32604; /* round(0.995 * 32768) */
    for (size_t i = 0; i < n_frames; ++i) {
        for (uint8_t c = 0; c < channels; ++c) {
            int32_t x = frames[i * channels + c];
            int32_t y = x - x_prev[c] + ((alpha_q15 * y_prev[c]) >> 15);
            if (y > INT16_MAX) y = INT16_MAX;
            if (y < INT16_MIN) y = INT16_MIN;
            frames[i * channels + c] = (int16_t)y;
            x_prev[c]                = x;
            y_prev[c]                = y;
        }
    }
}

#endif /* CONFIG_ALP_SDK_AUDIO_IN */

/* ================================================================== */
/* Audio input                                                         */
/* ================================================================== */

alp_audio_in_t *alp_audio_in_open(const alp_audio_config_t *cfg)
{
    alp_z_clear_last_error();

    if (cfg == NULL || cfg->channels == 0 || cfg->channels > 2 || cfg->frames_per_block == 0) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

#if defined(CONFIG_ALP_SDK_AUDIO_IN)
    if (cfg->peripheral_id >= ARRAY_SIZE(alp_pdm_devs)) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    const struct device *dev = alp_pdm_devs[cfg->peripheral_id];
    if (dev == NULL || !device_is_ready(dev)) {
        alp_z_set_last_error(ALP_ERR_NOT_READY);
        return NULL;
    }

    size_t bf          = bytes_per_frame(cfg);
    size_t block_bytes = (size_t)cfg->frames_per_block * bf;
    if (block_bytes == 0 || block_bytes > CONFIG_ALP_SDK_AUDIO_BLOCK_BYTES) {
        alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
        return NULL;
    }

    struct alp_audio_in *h = audio_in_pool_acquire();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }

    h->cfg = *cfg;
    h->dev = dev;

    int err =
        k_mem_slab_init(&h->slab, h->slab_buf, block_bytes, CONFIG_ALP_SDK_AUDIO_IN_SLAB_BLOCKS);
    if (err != 0) {
        alp_z_set_last_error(errno_to_alp(err));
        audio_in_pool_release(h);
        return NULL;
    }

    struct pcm_stream_cfg stream = {
        .pcm_rate   = cfg->sample_rate_hz,
        .pcm_width  = pcm_width_for(cfg->format),
        .block_size = block_bytes,
        .mem_slab   = &h->slab,
    };
    struct dmic_cfg dcfg = {
        .io =
            {
                .min_pdm_clk_freq = 1000000,
                .max_pdm_clk_freq = 3500000,
                .min_pdm_clk_dc   = 40,
                .max_pdm_clk_dc   = 60,
            },
        .streams                 = &stream,
        .channel.req_num_chan    = cfg->channels,
        .channel.req_num_streams = 1,
        .channel.req_chan_map_lo =
            (cfg->channels >= 1) ? dmic_build_channel_map(0, 0, PDM_CHAN_LEFT) : 0,
    };
    if (cfg->channels == 2) {
        dcfg.channel.req_chan_map_lo |= dmic_build_channel_map(1, 0, PDM_CHAN_RIGHT);
    }

    err = dmic_configure(dev, &dcfg);
    if (err != 0) {
        alp_z_set_last_error(errno_to_alp(err));
        audio_in_pool_release(h);
        return NULL;
    }

    h->started      = false;
    h->dc_x_prev[0] = h->dc_x_prev[1] = 0;
    h->dc_y_prev[0] = h->dc_y_prev[1] = 0;
    return h;
#else
    alp_z_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
#endif
}

alp_status_t alp_audio_in_start(alp_audio_in_t *in)
{
    if (in == NULL || !in->in_use) return ALP_ERR_NOT_READY;
#if defined(CONFIG_ALP_SDK_AUDIO_IN)
    int err = dmic_trigger(in->dev, DMIC_TRIGGER_START);
    if (err == 0) in->started = true;
    return errno_to_alp(err);
#else
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_audio_in_stop(alp_audio_in_t *in)
{
    if (in == NULL || !in->in_use) return ALP_ERR_NOT_READY;
#if defined(CONFIG_ALP_SDK_AUDIO_IN)
    int err = dmic_trigger(in->dev, DMIC_TRIGGER_STOP);
    if (err == 0) in->started = false;
    return errno_to_alp(err);
#else
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_audio_in_read(alp_audio_in_t *in, void *buf, size_t frames, size_t *out_frames,
                               uint32_t timeout_ms)
{
    if (out_frames != NULL) *out_frames = 0;
    if (in == NULL || !in->in_use) return ALP_ERR_NOT_READY;
    if (buf == NULL || frames == 0) return ALP_ERR_INVAL;

#if defined(CONFIG_ALP_SDK_AUDIO_IN)
    void    *block = NULL;
    uint32_t got   = 0;
    int      err   = dmic_read(in->dev, 0, &block, &got, (int32_t)timeout_ms);
    if (err != 0) return errno_to_alp(err);

    size_t bf       = bytes_per_frame(&in->cfg);
    size_t got_frms = got / bf;
    size_t want     = MIN(got_frms, frames);
    size_t cpy      = want * bf;
    memcpy(buf, block, cpy);
    k_mem_slab_free(&in->slab, block);

    /* DSP chain -- DC-block runs in place on the caller's buffer.
     * S16 only in v0.2; S24/S32 paths land alongside the v0.3
     * AGC/resample additions when the chain matters more. */
    if (in->cfg.format == ALP_AUDIO_FMT_S16_LE) {
        dc_block_s16((int16_t *)buf, want, in->cfg.channels, in->dc_x_prev, in->dc_y_prev);
    }

    if (out_frames != NULL) *out_frames = want;
    return ALP_OK;
#else
    (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
#endif
}

void alp_audio_in_close(alp_audio_in_t *in)
{
    if (in == NULL || !in->in_use) return;
#if defined(CONFIG_ALP_SDK_AUDIO_IN)
    if (in->started) {
        (void)dmic_trigger(in->dev, DMIC_TRIGGER_STOP);
        in->started = false;
    }
    audio_in_pool_release(in);
#endif
}

/* ================================================================== */
/* Audio output                                                        */
/* ================================================================== */

#if defined(CONFIG_ALP_SDK_AUDIO_OUT)

static alp_i2s_format_t audio_to_i2s_format(alp_audio_format_t f)
{
    (void)f;
    /* PCM I2S frames carry 16/24/32-bit slots; the alp_i2s wrapper
     * picks slot width from word_bits, not from format.  Stick with
     * the standard I2S phase regardless. */
    return ALP_I2S_FMT_I2S;
}

#endif /* CONFIG_ALP_SDK_AUDIO_OUT */

alp_audio_out_t *alp_audio_out_open(const alp_audio_config_t *cfg)
{
    alp_z_clear_last_error();

    if (cfg == NULL || cfg->channels == 0 || cfg->channels > 2 || cfg->frames_per_block == 0) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

#if defined(CONFIG_ALP_SDK_AUDIO_OUT)
    struct alp_audio_out *h = audio_out_pool_acquire();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }

    h->cfg                = *cfg;
    h->volume_q8          = 0x0100; /* unity */

    alp_i2s_config_t icfg = {
        .bus_id         = cfg->peripheral_id,
        .direction      = ALP_I2S_DIR_TX,
        .sample_rate_hz = cfg->sample_rate_hz,
        .channels       = cfg->channels,
        .word_bits      = pcm_width_for(cfg->format),
        .format         = audio_to_i2s_format(cfg->format),
        .block_frames   = cfg->frames_per_block,
    };
    h->i2s = alp_i2s_open(&icfg);
    if (h->i2s == NULL) {
        /* alp_i2s_open already stamped a precise last_error -- propagate. */
        audio_out_pool_release(h);
        return NULL;
    }

    h->started = false;
    return h;
#else
    alp_z_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
#endif
}

alp_status_t alp_audio_out_start(alp_audio_out_t *out)
{
    if (out == NULL || !out->in_use) return ALP_ERR_NOT_READY;
#if defined(CONFIG_ALP_SDK_AUDIO_OUT)
    alp_status_t s = alp_i2s_start(out->i2s);
    if (s == ALP_OK) out->started = true;
    return s;
#else
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_audio_out_stop(alp_audio_out_t *out)
{
    if (out == NULL || !out->in_use) return ALP_ERR_NOT_READY;
#if defined(CONFIG_ALP_SDK_AUDIO_OUT)
    alp_status_t s = alp_i2s_stop(out->i2s);
    if (s == ALP_OK) out->started = false;
    return s;
#else
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_audio_out_write(alp_audio_out_t *out, const void *buf, size_t frames,
                                 size_t *out_frames, uint32_t timeout_ms)
{
    if (out_frames != NULL) *out_frames = 0;
    if (out == NULL || !out->in_use) return ALP_ERR_NOT_READY;
    if (buf == NULL || frames == 0) return ALP_ERR_INVAL;

#if defined(CONFIG_ALP_SDK_AUDIO_OUT)
    size_t bytes = frames * bytes_per_frame(&out->cfg);

    /* Software volume.  Unity (0x0100) skips the loop -- common case
     * for codecs that have their own gain pin or bus-attenuator. */
    if (out->volume_q8 != 0x0100 && out->cfg.format == ALP_AUDIO_FMT_S16_LE) {
        /* Apply scale into a scratch block, chunked to keep the
         * stack footprint bounded across small targets. */
        const int16_t *src = (const int16_t *)buf;
        int16_t        chunk[256];
        size_t         remaining_frames = frames;
        size_t         chunk_frames     = sizeof(chunk) / sizeof(int16_t) / out->cfg.channels;
        if (chunk_frames == 0) chunk_frames = 1;

        size_t pushed = 0;
        while (remaining_frames > 0) {
            size_t n  = MIN(remaining_frames, chunk_frames);
            size_t ns = n * out->cfg.channels;
            for (size_t i = 0; i < ns; ++i) {
                int32_t scaled = ((int32_t)src[i] * out->volume_q8) >> 8;
                if (scaled > INT16_MAX) scaled = INT16_MAX;
                if (scaled < INT16_MIN) scaled = INT16_MIN;
                chunk[i] = (int16_t)scaled;
            }
            alp_status_t s =
                alp_i2s_write(out->i2s, chunk, n * bytes_per_frame(&out->cfg), timeout_ms);
            if (s != ALP_OK) {
                if (out_frames != NULL) *out_frames = pushed;
                return s;
            }
            src += ns;
            remaining_frames -= n;
            pushed += n;
        }
        if (out_frames != NULL) *out_frames = pushed;
        return ALP_OK;
    }

    alp_status_t s = alp_i2s_write(out->i2s, buf, bytes, timeout_ms);
    if (s == ALP_OK && out_frames != NULL) *out_frames = frames;
    return s;
#else
    (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_audio_out_set_volume(alp_audio_out_t *out, uint8_t vol)
{
    if (out == NULL || !out->in_use) return ALP_ERR_NOT_READY;
#if defined(CONFIG_ALP_SDK_AUDIO_OUT)
    /* Map 0..255 to 0..0x0100 (Q8.8 unity = 256).  255 maps to 256
     * for a clean unity ceiling -- avoids the off-by-one cliff. */
    out->volume_q8 = (uint16_t)vol + (uint16_t)(vol == 255);
    return ALP_OK;
#else
    (void)vol;
    return ALP_ERR_NOSUPPORT;
#endif
}

void alp_audio_out_close(alp_audio_out_t *out)
{
    if (out == NULL || !out->in_use) return;
#if defined(CONFIG_ALP_SDK_AUDIO_OUT)
    if (out->started) {
        (void)alp_i2s_stop(out->i2s);
        out->started = false;
    }
    if (out->i2s != NULL) {
        alp_i2s_close(out->i2s);
        out->i2s = NULL;
    }
    audio_out_pool_release(out);
#endif
}
