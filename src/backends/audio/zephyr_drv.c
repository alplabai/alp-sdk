/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr backend for the <alp/audio.h> surface.  Owns both
 * directions -- input + output -- behind one ops vtable.
 *
 * Input half wraps Zephyr's `audio_dmic` API (CONFIG_AUDIO_DMIC) and
 * runs a DSP chain (1st-order DC-block IIR) on every delivered block
 * before handing it to the caller.  Output half delegates to
 * alp_i2s_* (already a portable wrapper) and applies a software
 * volume scale on the way through so apps without a separate codec
 * gain pin still get a usable output volume.
 *
 * Backend-owned state lives in module-static pools indexed via
 * state->be_data:
 *   - struct hw_in_be   (dmic device + k_mem_slab + DSP filter state)
 *   - struct hw_out_be  (alp_i2s_t handle + started flag + Q8.8 vol)
 *
 * The dispatcher (src/audio_dispatch.c) owns the public-facing
 * struct alp_audio_in / struct alp_audio_out pools; this backend
 * carries only the Zephyr-specific per-handle blobs.
 *
 * The portable-HW-offload audit rule (memory/feedback_portable_hw_
 * offload_with_sw_fallback.md) is satisfied because the chip-
 * specific dispatch happens inside Zephyr's audio_dmic / I2S driver
 * classes -- application code never sees a vendor name in
 * <alp/audio.h>.
 *
 * Registers as silicon_ref="*" at priority 100 -- mirrors the
 * mproc / TMU / USB / BLE / Wi-Fi / MQTT / RPC / security siblings.
 * Gated on CONFIG_ALP_SDK_AUDIO_IN / _OUT -- when OFF, the matching
 * direction's I/O ops return NOSUPPORT but the registry entry still
 * links so the dispatcher picks it ahead of sw_fallback on real
 * silicon builds with the audio_dmic / I2S driver classes in the
 * device-tree configuration.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <alp/audio.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/i2s.h>
#include <alp/peripheral.h>

#include "audio_ops.h"

#if defined(CONFIG_ALP_SDK_AUDIO_IN)
#include <zephyr/audio/dmic.h>
#endif

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
/* Backend-owned per-handle state                                      */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_ALP_SDK_AUDIO_IN)

struct hw_in_be {
	bool                 in_use;
	const struct device *dev;
	struct k_mem_slab    slab;
	uint8_t slab_buf[CONFIG_ALP_SDK_AUDIO_BLOCK_BYTES * CONFIG_ALP_SDK_AUDIO_IN_SLAB_BLOCKS];
	bool    started;
	/* 1st-order DC-block IIR state per channel.  alpha pinned at
     * 0.995 in 1.15 fixed-point (32 604/32 768) for a ~10 Hz cutoff
     * at 16 kHz -- inaudible but kills DC bias from PDM mics. */
	int32_t dc_x_prev[2];
	int32_t dc_y_prev[2];
};

static struct hw_in_be g_in_be_pool[CONFIG_ALP_SDK_MAX_AUDIO_IN_HANDLES];

#endif /* CONFIG_ALP_SDK_AUDIO_IN */

#if defined(CONFIG_ALP_SDK_AUDIO_OUT)

struct hw_out_be {
	bool       in_use;
	alp_i2s_t *i2s;
	bool       started;
	uint16_t   volume_q8; /* Q8.8 software gain, 0x0100 = unity */
};

static struct hw_out_be g_out_be_pool[CONFIG_ALP_SDK_MAX_AUDIO_OUT_HANDLES];

#endif /* CONFIG_ALP_SDK_AUDIO_OUT */

#if defined(CONFIG_ALP_SDK_AUDIO_IN)

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

#endif /* CONFIG_ALP_SDK_AUDIO_IN */

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

#endif /* AUDIO_IN || AUDIO_OUT */

/* ================================================================== */
/* DMIC device resolution                                              */
/* ================================================================== */

#if defined(CONFIG_ALP_SDK_AUDIO_IN)

#define ALP_PDM_DEV_OR_NULL(idx)                                                                   \
	COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_pdm, idx))),                                   \
	            (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_pdm, idx)))),                                  \
	            (NULL))

static const struct device *const alp_pdm_devs[] = {
	ALP_PDM_DEV_OR_NULL(0),
	ALP_PDM_DEV_OR_NULL(1),
};

static struct hw_in_be *alloc_in_be(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(g_in_be_pool); ++i) {
		if (!g_in_be_pool[i].in_use) {
			memset(&g_in_be_pool[i], 0, sizeof(g_in_be_pool[i]));
			g_in_be_pool[i].in_use = true;
			return &g_in_be_pool[i];
		}
	}
	return NULL;
}

static void free_in_be(struct hw_in_be *be)
{
	if (be != NULL) be->in_use = false;
}

/* DC-block: y[n] = x[n] - x[n-1] + alpha * y[n-1].  alpha = 0.995 in
 * Q15 (fixed point) so the multiply costs one >>15.  Operates in place
 * on signed 16-bit interleaved PCM. */
static void
dc_block_s16(int16_t *frames, size_t n_frames, uint8_t channels, int32_t *x_prev, int32_t *y_prev)
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
/* Audio input ops                                                     */
/* ================================================================== */

static alp_status_t z_in_open(const alp_audio_config_t     *cfg,
                              alp_audio_in_backend_state_t *state,
                              alp_capabilities_t           *caps_out)
{
	(void)caps_out;

	if (cfg->channels == 0 || cfg->channels > 2 || cfg->frames_per_block == 0) {
		return ALP_ERR_INVAL;
	}

#if defined(CONFIG_ALP_SDK_AUDIO_IN)
	if (cfg->peripheral_id >= ARRAY_SIZE(alp_pdm_devs)) {
		return ALP_ERR_INVAL;
	}
	const struct device *dev = alp_pdm_devs[cfg->peripheral_id];
	if (dev == NULL || !device_is_ready(dev)) {
		return ALP_ERR_NOT_READY;
	}

	size_t bf          = bytes_per_frame(cfg);
	size_t block_bytes = (size_t)cfg->frames_per_block * bf;
	if (block_bytes == 0 || block_bytes > CONFIG_ALP_SDK_AUDIO_BLOCK_BYTES) {
		return ALP_ERR_OUT_OF_RANGE;
	}

	struct hw_in_be *be = alloc_in_be();
	if (be == NULL) return ALP_ERR_NOMEM;

	be->dev = dev;
	int err =
	    k_mem_slab_init(&be->slab, be->slab_buf, block_bytes, CONFIG_ALP_SDK_AUDIO_IN_SLAB_BLOCKS);
	if (err != 0) {
		free_in_be(be);
		return errno_to_alp(err);
	}

	struct pcm_stream_cfg stream = {
		.pcm_rate   = cfg->sample_rate_hz,
		.pcm_width  = pcm_width_for(cfg->format),
		.block_size = block_bytes,
		.mem_slab   = &be->slab,
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
		free_in_be(be);
		return errno_to_alp(err);
	}

	be->started      = false;
	be->dc_x_prev[0] = be->dc_x_prev[1] = 0;
	be->dc_y_prev[0] = be->dc_y_prev[1] = 0;
	state->be_data                      = be;
	return ALP_OK;
#else
	(void)state;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_in_start(alp_audio_in_backend_state_t *state)
{
#if defined(CONFIG_ALP_SDK_AUDIO_IN)
	struct hw_in_be *be = (struct hw_in_be *)state->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;
	int err = dmic_trigger(be->dev, DMIC_TRIGGER_START);
	if (err == 0) be->started = true;
	return errno_to_alp(err);
#else
	(void)state;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_in_stop(alp_audio_in_backend_state_t *state)
{
#if defined(CONFIG_ALP_SDK_AUDIO_IN)
	struct hw_in_be *be = (struct hw_in_be *)state->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;
	int err = dmic_trigger(be->dev, DMIC_TRIGGER_STOP);
	if (err == 0) be->started = false;
	return errno_to_alp(err);
#else
	(void)state;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_in_read(alp_audio_in_backend_state_t *state,
                              void                         *buf,
                              size_t                        frames,
                              size_t                       *out_frames,
                              uint32_t                      timeout_ms)
{
#if defined(CONFIG_ALP_SDK_AUDIO_IN)
	struct hw_in_be *be = (struct hw_in_be *)state->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;

	void  *block = NULL;
	size_t got   = 0;
	int    err   = dmic_read(be->dev, 0, &block, &got, (int32_t)timeout_ms);
	if (err != 0) return errno_to_alp(err);

	size_t bf       = bytes_per_frame(&state->cfg);
	size_t got_frms = got / bf;
	size_t want     = MIN(got_frms, frames);
	size_t cpy      = want * bf;
	memcpy(buf, block, cpy);
	k_mem_slab_free(&be->slab, block);

	/* DSP chain -- DC-block runs in place on the caller's buffer.
     * S16 only in v0.2; S24/S32 paths land alongside the v0.3
     * AGC/resample additions when the chain matters more. */
	if (state->cfg.format == ALP_AUDIO_FMT_S16_LE) {
		dc_block_s16((int16_t *)buf, want, state->cfg.channels, be->dc_x_prev, be->dc_y_prev);
	}

	if (out_frames != NULL) *out_frames = want;
	return ALP_OK;
#else
	(void)state;
	(void)buf;
	(void)frames;
	(void)out_frames;
	(void)timeout_ms;
	return ALP_ERR_NOSUPPORT;
#endif
}

static void z_in_close(alp_audio_in_backend_state_t *state)
{
#if defined(CONFIG_ALP_SDK_AUDIO_IN)
	struct hw_in_be *be = (struct hw_in_be *)state->be_data;
	if (be == NULL) return;
	if (be->started) {
		(void)dmic_trigger(be->dev, DMIC_TRIGGER_STOP);
		be->started = false;
	}
	free_in_be(be);
	state->be_data = NULL;
#else
	(void)state;
#endif
}

/* ================================================================== */
/* Audio output ops                                                    */
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

static struct hw_out_be *alloc_out_be(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(g_out_be_pool); ++i) {
		if (!g_out_be_pool[i].in_use) {
			memset(&g_out_be_pool[i], 0, sizeof(g_out_be_pool[i]));
			g_out_be_pool[i].in_use = true;
			return &g_out_be_pool[i];
		}
	}
	return NULL;
}

static void free_out_be(struct hw_out_be *be)
{
	if (be != NULL) be->in_use = false;
}

#endif /* CONFIG_ALP_SDK_AUDIO_OUT */

static alp_status_t z_out_open(const alp_audio_config_t      *cfg,
                               alp_audio_out_backend_state_t *state,
                               alp_capabilities_t            *caps_out)
{
	(void)caps_out;

	if (cfg->channels == 0 || cfg->channels > 2 || cfg->frames_per_block == 0) {
		return ALP_ERR_INVAL;
	}

#if defined(CONFIG_ALP_SDK_AUDIO_OUT)
	struct hw_out_be *be = alloc_out_be();
	if (be == NULL) return ALP_ERR_NOMEM;

	be->volume_q8 = 0x0100; /* unity */

	alp_i2s_config_t icfg = {
		.bus_id         = cfg->peripheral_id,
		.direction      = ALP_I2S_DIR_TX,
		.sample_rate_hz = cfg->sample_rate_hz,
		.channels       = cfg->channels,
		.word_bits      = pcm_width_for(cfg->format),
		.format         = audio_to_i2s_format(cfg->format),
		.block_frames   = cfg->frames_per_block,
	};
	be->i2s = alp_i2s_open(&icfg);
	if (be->i2s == NULL) {
		/* alp_i2s_open already stamped a precise last_error -- propagate. */
		alp_status_t le = alp_last_error();
		free_out_be(be);
		return (le != ALP_OK) ? le : ALP_ERR_IO;
	}

	be->started    = false;
	state->be_data = be;
	return ALP_OK;
#else
	(void)state;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_out_start(alp_audio_out_backend_state_t *state)
{
#if defined(CONFIG_ALP_SDK_AUDIO_OUT)
	struct hw_out_be *be = (struct hw_out_be *)state->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;
	alp_status_t s = alp_i2s_start(be->i2s);
	if (s == ALP_OK) be->started = true;
	return s;
#else
	(void)state;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_out_stop(alp_audio_out_backend_state_t *state)
{
#if defined(CONFIG_ALP_SDK_AUDIO_OUT)
	struct hw_out_be *be = (struct hw_out_be *)state->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;
	alp_status_t s = alp_i2s_stop(be->i2s);
	if (s == ALP_OK) be->started = false;
	return s;
#else
	(void)state;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_out_write(alp_audio_out_backend_state_t *state,
                                const void                    *buf,
                                size_t                         frames,
                                size_t                        *out_frames,
                                uint32_t                       timeout_ms)
{
#if defined(CONFIG_ALP_SDK_AUDIO_OUT)
	struct hw_out_be *be = (struct hw_out_be *)state->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;

	size_t bytes = frames * bytes_per_frame(&state->cfg);

	/* Software volume.  Unity (0x0100) skips the loop -- common case
     * for codecs that have their own gain pin or bus-attenuator. */
	if (be->volume_q8 != 0x0100 && state->cfg.format == ALP_AUDIO_FMT_S16_LE) {
		/* Apply scale into a scratch block, chunked to keep the
         * stack footprint bounded across small targets. */
		const int16_t *src = (const int16_t *)buf;
		int16_t        chunk[256];
		size_t         remaining_frames = frames;
		size_t         chunk_frames     = sizeof(chunk) / sizeof(int16_t) / state->cfg.channels;
		if (chunk_frames == 0) chunk_frames = 1;

		size_t pushed = 0;
		while (remaining_frames > 0) {
			size_t n  = MIN(remaining_frames, chunk_frames);
			size_t ns = n * state->cfg.channels;
			for (size_t i = 0; i < ns; ++i) {
				int32_t scaled = ((int32_t)src[i] * be->volume_q8) >> 8;
				if (scaled > INT16_MAX) scaled = INT16_MAX;
				if (scaled < INT16_MIN) scaled = INT16_MIN;
				chunk[i] = (int16_t)scaled;
			}
			alp_status_t s =
			    alp_i2s_write(be->i2s, chunk, n * bytes_per_frame(&state->cfg), timeout_ms);
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

	alp_status_t s = alp_i2s_write(be->i2s, buf, bytes, timeout_ms);
	if (s == ALP_OK && out_frames != NULL) *out_frames = frames;
	return s;
#else
	(void)state;
	(void)buf;
	(void)frames;
	(void)out_frames;
	(void)timeout_ms;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_out_set_volume(alp_audio_out_backend_state_t *state, uint8_t vol)
{
#if defined(CONFIG_ALP_SDK_AUDIO_OUT)
	struct hw_out_be *be = (struct hw_out_be *)state->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;
	/* Map 0..255 to 0..0x0100 (Q8.8 unity = 256).  255 maps to 256
     * for a clean unity ceiling -- avoids the off-by-one cliff. */
	be->volume_q8 = (uint16_t)vol + (uint16_t)(vol == 255);
	return ALP_OK;
#else
	(void)state;
	(void)vol;
	return ALP_ERR_NOSUPPORT;
#endif
}

static void z_out_close(alp_audio_out_backend_state_t *state)
{
#if defined(CONFIG_ALP_SDK_AUDIO_OUT)
	struct hw_out_be *be = (struct hw_out_be *)state->be_data;
	if (be == NULL) return;
	if (be->started) {
		(void)alp_i2s_stop(be->i2s);
		be->started = false;
	}
	if (be->i2s != NULL) {
		alp_i2s_close(be->i2s);
		be->i2s = NULL;
	}
	free_out_be(be);
	state->be_data = NULL;
#else
	(void)state;
#endif
}

/* ---------- Registration ---------- */

static const alp_audio_ops_t _ops = {
	.in_open        = z_in_open,
	.in_start       = z_in_start,
	.in_stop        = z_in_stop,
	.in_read        = z_in_read,
	.in_close       = z_in_close,
	.out_open       = z_out_open,
	.out_start      = z_out_start,
	.out_stop       = z_out_stop,
	.out_write      = z_out_write,
	.out_set_volume = z_out_set_volume,
	.out_close      = z_out_close,
};

ALP_BACKEND_REGISTER(audio,
                     zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
