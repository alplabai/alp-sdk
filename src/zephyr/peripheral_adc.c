/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/adc.h>.
 *
 * Channel resolution.  Each studio-resolved channel_id (0..7) maps to
 * the `alp-adcN` DT alias.  The alias must point at a node with
 * `io-channels` plus the Zephyr ADC channel-config properties
 * (`zephyr,resolution`, `zephyr,reference`, `zephyr,gain`,
 * `zephyr,acquisition-time`):
 *
 *     adc_user0: adc_user_0 {
 *         io-channels = <&adc0 0>;
 *         zephyr,resolution  = <12>;
 *         zephyr,reference   = "ADC_REF_INTERNAL";
 *         zephyr,gain        = "ADC_GAIN_1";
 *         zephyr,acquisition-time = <ADC_ACQ_TIME_DEFAULT>;
 *         zephyr,vref-mv     = <3300>;
 *     };
 *     aliases { alp-adc0 = &adc_user0; };
 *
 * Capability validation.  The active SoC's documented ADC max
 * resolution is exposed by `<alp/soc_caps.h>` as
 * `ALP_SOC_ADC_MAX_RESOLUTION_BITS`.  Asking for a higher resolution
 * than the SoC supports returns NULL with last_error =
 * ALP_ERR_OUT_OF_RANGE — for example, a 16-bit request on an Alif E3
 * (12-bit hardware) is rejected before any I/O.
 *
 * Conditional spec construction.  ADC_DT_SPEC_GET fails to expand
 * when given DT_INVALID_NODE (it interrogates the node's properties
 * unconditionally), so we can't just COND_CODE_1 on alias existence.
 * Per-index #if blocks emit either ADC_DT_SPEC_GET or a NULL spec,
 * and the array is built from those.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "alp/adc.h"
#include "alp/dsp.h"
#include "alp/soc_caps.h"
#include "handles.h"
#include "v2n_supervisor.h"

/* On V2N every E1M ADC channel is GD32-driven (Renesas SoC's
 * ALP_SOC_ADC_COUNT = 24 but the carrier exposes the 8 E1M channels
 * via the GD32 IO MCU per gd32-io-mcu-map.tsv).  The bridge already
 * returns mV-corrected readings, so the V2N path uses mV as the
 * "raw" value (16-bit unsigned, sign-extended into int32) and
 * alp_adc_read_uv multiplies by 1000 to honour the public contract. */
#if defined(CONFIG_ALP_SDK_V2N_SUPERVISOR)
#define ALP_ADC_HAS_BRIDGE_PATH 1
#else
#define ALP_ADC_HAS_BRIDGE_PATH 0
#endif

#if DT_NODE_EXISTS(DT_ALIAS(alp_adc0))
#define ALP_ADC_SPEC_0_INIT  ADC_DT_SPEC_GET(DT_ALIAS(alp_adc0))
#else
#define ALP_ADC_SPEC_0_INIT  {.dev = NULL}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc1))
#define ALP_ADC_SPEC_1_INIT  ADC_DT_SPEC_GET(DT_ALIAS(alp_adc1))
#else
#define ALP_ADC_SPEC_1_INIT  {.dev = NULL}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc2))
#define ALP_ADC_SPEC_2_INIT  ADC_DT_SPEC_GET(DT_ALIAS(alp_adc2))
#else
#define ALP_ADC_SPEC_2_INIT  {.dev = NULL}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc3))
#define ALP_ADC_SPEC_3_INIT  ADC_DT_SPEC_GET(DT_ALIAS(alp_adc3))
#else
#define ALP_ADC_SPEC_3_INIT  {.dev = NULL}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc4))
#define ALP_ADC_SPEC_4_INIT  ADC_DT_SPEC_GET(DT_ALIAS(alp_adc4))
#else
#define ALP_ADC_SPEC_4_INIT  {.dev = NULL}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc5))
#define ALP_ADC_SPEC_5_INIT  ADC_DT_SPEC_GET(DT_ALIAS(alp_adc5))
#else
#define ALP_ADC_SPEC_5_INIT  {.dev = NULL}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc6))
#define ALP_ADC_SPEC_6_INIT  ADC_DT_SPEC_GET(DT_ALIAS(alp_adc6))
#else
#define ALP_ADC_SPEC_6_INIT  {.dev = NULL}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_adc7))
#define ALP_ADC_SPEC_7_INIT  ADC_DT_SPEC_GET(DT_ALIAS(alp_adc7))
#else
#define ALP_ADC_SPEC_7_INIT  {.dev = NULL}
#endif

static const struct adc_dt_spec alp_adcs[] = {
    ALP_ADC_SPEC_0_INIT,
    ALP_ADC_SPEC_1_INIT,
    ALP_ADC_SPEC_2_INIT,
    ALP_ADC_SPEC_3_INIT,
    ALP_ADC_SPEC_4_INIT,
    ALP_ADC_SPEC_5_INIT,
    ALP_ADC_SPEC_6_INIT,
    ALP_ADC_SPEC_7_INIT,
};

static alp_status_t errno_to_alp(int err) {
    switch (err) {
    case 0:           return ALP_OK;
    case -EINVAL:     return ALP_ERR_INVAL;
    case -EBUSY:      return ALP_ERR_BUSY;
    case -ENOTSUP:
    case -ENOSYS:     return ALP_ERR_NOSUPPORT;
    default:          return ALP_ERR_IO;
    }
}

#if ALP_ADC_HAS_BRIDGE_PATH
/* Stream-slot bitmap.  The GD32G553 firmware exposes exactly
 * GD32G553_BRIDGE_ADC_STREAM_COUNT (= 2) DMA-backed streams; the
 * portable surface tracks allocation locally so alp_adc_stream_open
 * doesn't have to probe the firmware with a speculative
 * STREAM_BEGIN that could collide with another caller's existing
 * slot.  Same bitmap covers V2N and V2N-M1 (shared supervisor). */
static struct k_mutex bridge_stream_lock;
static uint8_t        bridge_streams_used;

static int            bridge_stream_lock_init(void)
{
    k_mutex_init(&bridge_stream_lock);
    return 0;
}
SYS_INIT(bridge_stream_lock_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

static int bridge_stream_alloc_slot(void)
{
    k_mutex_lock(&bridge_stream_lock, K_FOREVER);
    int slot = -1;
    for (int i = 0; i < (int)GD32G553_BRIDGE_ADC_STREAM_COUNT; ++i) {
        if (!(bridge_streams_used & (1u << i))) {
            bridge_streams_used |= (uint8_t)(1u << i);
            slot = i;
            break;
        }
    }
    k_mutex_unlock(&bridge_stream_lock);
    return slot;
}

static void bridge_stream_free_slot(uint8_t slot)
{
    k_mutex_lock(&bridge_stream_lock, K_FOREVER);
    bridge_streams_used &= (uint8_t) ~(1u << slot);
    k_mutex_unlock(&bridge_stream_lock);
}

static alp_adc_t *bridge_open(const alp_adc_config_t *cfg) {
    /* The bridge advertises a fixed 12-bit / ~3.3 V reference DAC;
     * its ADC_READ replies are already mV-corrected, so the host
     * presents the channel to callers as a 16-bit-resolution
     * mV-encoded ADC and skips the Zephyr adc_raw_to_millivolts
     * conversion. */
    if (cfg->channel_id >= 8u) {
        /* The E1M spec reserves 8 ADC channels and the bridge
         * advertises exactly that many in gd32-io-mcu-map.tsv. */
        alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
        return NULL;
    }

    /* Probe the supervisor up-front so the first read doesn't surface
     * the bus-open failure as a runtime error, AND -- when the caller
     * asked for any v0.3 tuning knob -- push the configuration in
     * before returning the handle.  Holding the mutex for the whole
     * window keeps the probe + configure atomic from the supervisor's
     * point of view. */
    gd32g553_t *ctx = NULL;
    alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);
    if (s != ALP_OK) {
        alp_z_set_last_error(s);
        return NULL;
    }
    if (cfg->oversampling_ratio != 0u || cfg->sample_cycles != 0u || cfg->resolution_bits != 0u) {
        s = gd32g553_adc_configure(ctx, (uint8_t)cfg->channel_id, cfg->oversampling_ratio,
                                   cfg->sample_cycles, cfg->resolution_bits);
        if (s != ALP_OK) {
            alp_z_v2n_supervisor_release();
            alp_z_set_last_error(s);
            return NULL;
        }
    }
    alp_z_v2n_supervisor_release();

    struct alp_adc *h = alp_z_adc_pool_acquire();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }

    h->channel_id = cfg->channel_id;
    h->dev        = NULL;                      /* bridge sentinel */
    h->channel    = (uint8_t)cfg->channel_id;  /* bridge-side index == E1M index */
    h->resolution = 16u;                       /* mV fits in u16 across 3.3 V rails */
    h->vref_mv    = 3300u;                     /* documentation only -- the
                                                * mV value already encodes any
                                                * reference / gain choice. */
    return h;
}
#endif  /* ALP_ADC_HAS_BRIDGE_PATH */

alp_adc_t *alp_adc_open(const alp_adc_config_t *cfg) {
    alp_z_clear_last_error();

    if (cfg == NULL) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    /* Top-level channel_id bound applies to both paths; keeping the
     * ARRAY_SIZE reference here also stops -Werror=unused-const-variable
     * from flagging alp_adcs[] when ALP_ADC_HAS_BRIDGE_PATH=1
     * (`bridge_open` does its own < 8 check but doesn't touch the
     * DT-spec array). */
    if (cfg->channel_id >= ARRAY_SIZE(alp_adcs)) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

#if ALP_ADC_HAS_BRIDGE_PATH
    return bridge_open(cfg);
#else
    /* Capability check — reject configs the active SoC's documented
     * hardware can't honour.  This catches the canonical
     * "16-bit ADC requested on a 12-bit SoC" case before any
     * runtime I/O. */
    if (cfg->resolution_bits != 0 &&
        cfg->resolution_bits > ALP_SOC_ADC_MAX_RESOLUTION_BITS) {
        alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
        return NULL;
    }
    if (cfg->channel_id >= ALP_SOC_ADC_COUNT) {
        alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
        return NULL;
    }

    const struct adc_dt_spec *spec = &alp_adcs[cfg->channel_id];
    if (spec->dev == NULL || !device_is_ready(spec->dev)) {
        alp_z_set_last_error(ALP_ERR_NOT_READY);
        return NULL;
    }

    /* Cross-check against the runtime device's DT-declared maximum.
     * The SoC-cap macro is the documented limit; the DT spec is
     * what's actually wired on this board. */
    if (cfg->resolution_bits != 0 &&
        cfg->resolution_bits > spec->resolution) {
        alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
        return NULL;
    }

    struct alp_adc *h = alp_z_adc_pool_acquire();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }

    h->channel_id = cfg->channel_id;
    h->dev        = spec->dev;
    h->channel    = spec->channel_id;
    h->resolution = (cfg->resolution_bits != 0)
                      ? cfg->resolution_bits
                      : (uint8_t)spec->resolution;
    h->vref_mv    = spec->vref_mv;

    int err = adc_channel_setup_dt(spec);
    if (err != 0) {
        alp_z_set_last_error(errno_to_alp(err));
        alp_z_adc_pool_release(h);
        return NULL;
    }
    return h;
#endif  /* ALP_ADC_HAS_BRIDGE_PATH */
}

static alp_status_t one_shot(struct alp_adc *h, int32_t *raw_out) {
    struct adc_sequence seq = {
        .channels    = BIT(h->channel),
        .buffer      = &h->sample_buf,
        .buffer_size = sizeof h->sample_buf,
        .resolution  = h->resolution,
    };
    int err = adc_read(h->dev, &seq);
    if (err != 0) return errno_to_alp(err);
    *raw_out = (int32_t)h->sample_buf;
    return ALP_OK;
}

#if ALP_ADC_HAS_BRIDGE_PATH
static alp_status_t bridge_read_mv(struct alp_adc *h, uint16_t *mv_out) {
    gd32g553_t *ctx = NULL;
    alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);
    if (s != ALP_OK) return s;
    /* Ask for a single sample -- the firmware averages internally;
     * callers that want N-sample averaging on the host side can
     * issue a sequence of read_raw / read_uv calls. */
    s = gd32g553_adc_read(ctx, h->channel, 1u, mv_out);
    alp_z_v2n_supervisor_release();
    return s;
}
#endif

alp_status_t alp_adc_read_raw(alp_adc_t *adc, int32_t *raw_out) {
    if (adc == NULL || !adc->in_use) return ALP_ERR_NOT_READY;
    if (raw_out == NULL) return ALP_ERR_INVAL;
#if ALP_ADC_HAS_BRIDGE_PATH
    if (adc->dev == NULL) {
        /* Bridge already reports mV; the V2N "raw" value is mV
         * sign-extended into int32 -- there's no lower-level code
         * the host can recover. */
        uint16_t mv = 0u;
        alp_status_t s = bridge_read_mv(adc, &mv);
        if (s != ALP_OK) return s;
        *raw_out = (int32_t)mv;
        return ALP_OK;
    }
#endif
    return one_shot(adc, raw_out);
}

alp_status_t alp_adc_read_uv(alp_adc_t *adc, int32_t *uv_out) {
    if (adc == NULL || !adc->in_use) return ALP_ERR_NOT_READY;
    if (uv_out == NULL) return ALP_ERR_INVAL;
#if ALP_ADC_HAS_BRIDGE_PATH
    if (adc->dev == NULL) {
        uint16_t mv = 0u;
        alp_status_t s = bridge_read_mv(adc, &mv);
        if (s != ALP_OK) return s;
        *uv_out = (int32_t)mv * 1000;
        return ALP_OK;
    }
#endif

    int32_t raw = 0;
    alp_status_t s = one_shot(adc, &raw);
    if (s != ALP_OK) return s;

    int32_t mv = raw;
    int err = adc_raw_to_millivolts(adc->vref_mv,
                                    /* gain */ ADC_GAIN_1,
                                    adc->resolution,
                                    &mv);
    if (err != 0) {
        /* Fallback: raw passthrough as a μV proxy. */
        *uv_out = raw;
        return ALP_OK;
    }
    *uv_out = mv * 1000;
    return ALP_OK;
}

void alp_adc_close(alp_adc_t *adc) {
    alp_z_adc_pool_release(adc);
}

/* ====================================================================== */
/* Streaming ADC -- DMA-backed continuous acquisition.                     */
/*                                                                         */
/* V2N family (V2N + V2N-M1): both SoMs carry the GD32G553 supervisor MCU, */
/* whose firmware exposes GD32G553_BRIDGE_ADC_STREAM_COUNT concurrent      */
/* DMA-backed streams (one slot per DMA controller).  The portable        */
/* surface wraps STREAM_BEGIN / STREAM_READ / STREAM_END via the shared    */
/* supervisor singleton.                                                   */
/*                                                                         */
/* Other SoMs: the Zephyr `adc_*` driver class has no portable streaming   */
/* primitive that matches this contract, so alp_adc_stream_open returns    */
/* NULL with last-error = ALP_ERR_NOSUPPORT.  A future polling-thread      */
/* software fallback (timer + ring buffer) lives on the wave-2 roadmap.    */
/* ====================================================================== */

alp_adc_stream_t *alp_adc_stream_open(const alp_adc_stream_config_t *cfg)
{
    alp_z_clear_last_error();

    if (cfg == NULL) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

#if ALP_ADC_HAS_BRIDGE_PATH
    if (cfg->channel_id >= 8u) {
        alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
        return NULL;
    }
    if (cfg->sample_rate_hz == 0u) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

    /* Reserve a backend slot before touching the supervisor.  Returns
     * -1 when both DMA-backed streams are already in use. */
    int slot = bridge_stream_alloc_slot();
    if (slot < 0) {
        alp_z_set_last_error(ALP_ERR_BUSY);
        return NULL;
    }

    gd32g553_t  *ctx = NULL;
    alp_status_t s   = alp_z_v2n_supervisor_acquire(&ctx);
    if (s != ALP_OK) {
        bridge_stream_free_slot((uint8_t)slot);
        alp_z_set_last_error(s);
        return NULL;
    }
    s = gd32g553_adc_stream_begin(ctx, (uint8_t)slot, (uint8_t)cfg->channel_id,
                                  cfg->sample_rate_hz);
    alp_z_v2n_supervisor_release();

    if (s != ALP_OK) {
        bridge_stream_free_slot((uint8_t)slot);
        alp_z_set_last_error(s);
        return NULL;
    }

    struct alp_adc_stream *h = alp_z_adc_stream_pool_acquire();
    if (h == NULL) {
        /* Roll the bridge stream back so the slot is reusable. */
        if (alp_z_v2n_supervisor_acquire(&ctx) == ALP_OK) {
            (void)gd32g553_adc_stream_end(ctx, (uint8_t)slot);
            alp_z_v2n_supervisor_release();
        }
        bridge_stream_free_slot((uint8_t)slot);
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }

    h->via_bridge     = true;
    h->stream_id      = (uint8_t)slot;
    h->channel        = (uint8_t)cfg->channel_id;
    h->channel_id     = cfg->channel_id;
    h->sample_rate_hz = cfg->sample_rate_hz;
    return h;
#else
    alp_z_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
#endif /* ALP_ADC_HAS_BRIDGE_PATH */
}

alp_status_t alp_adc_stream_read(alp_adc_stream_t *stream, uint16_t *mv, size_t cap, size_t *got)
{
    if (got == NULL) return ALP_ERR_INVAL;
    *got = 0u;
    if (stream == NULL || !stream->in_use) return ALP_ERR_NOT_READY;
    if (mv == NULL) return ALP_ERR_INVAL;
    if (cap == 0u) return ALP_OK;

#if ALP_ADC_HAS_BRIDGE_PATH
    if (stream->via_bridge) {
        /* Backend caps per-call at GD32G553_BRIDGE_ADC_STREAM_READ_MAX
         * (= 32); callers wanting more loop in their own thread. */
        const uint8_t want     = (cap > (size_t)GD32G553_BRIDGE_ADC_STREAM_READ_MAX)
                                     ? (uint8_t)GD32G553_BRIDGE_ADC_STREAM_READ_MAX
                                     : (uint8_t)cap;
        uint8_t       got_this = 0u;

        gd32g553_t   *ctx      = NULL;
        alp_status_t  s        = alp_z_v2n_supervisor_acquire(&ctx);
        if (s != ALP_OK) return s;
        s = gd32g553_adc_stream_read(ctx, stream->stream_id, want, &got_this, mv);
        alp_z_v2n_supervisor_release();
        if (s != ALP_OK) return s;
        *got = got_this;
        return ALP_OK;
    }
#endif
    return ALP_ERR_NOSUPPORT;
}

void alp_adc_stream_close(alp_adc_stream_t *stream)
{
    if (stream == NULL) return;
#if ALP_ADC_HAS_BRIDGE_PATH
    if (stream->via_bridge) {
        gd32g553_t *ctx = NULL;
        if (alp_z_v2n_supervisor_acquire(&ctx) == ALP_OK) {
            (void)gd32g553_adc_stream_end(ctx, stream->stream_id);
            alp_z_v2n_supervisor_release();
        }
        bridge_stream_free_slot(stream->stream_id);
    }
#endif
    alp_z_adc_stream_pool_release(stream);
}

/* ====================================================================== */
/* Streaming ADC with DSP pipeline (wave-2)                                */
/*                                                                         */
/* alp_adc_filter_t composes alp_adc_stream_t + alp_dsp_chain_t under one  */
/* caller-facing handle.  The chain runs on the host today; the GD32-side */
/* bridge-offload path (CMD_ADC_STREAM_CONFIGURE_DSP, opcode 0x36 reserved */
/* in v0.5.0) lands in v0.5.x once the wire payload format finalises.  On */
/* SoMs without a streaming ADC backend the open returns NULL with        */
/* last-error = ALP_ERR_NOSUPPORT, same as alp_adc_stream_open.            */
/* ====================================================================== */

/* The filter impl needs the DSP chain machinery + a streaming ADC
 * backend.  When either is absent, fall back to NOSUPPORT stubs --
 * the symbols are exported unconditionally so apps linking against
 * <alp/adc.h> stay link-clean. */
#if defined(CONFIG_ALP_SDK_DSP) && ALP_ADC_HAS_BRIDGE_PATH
#define ALP_ADC_HAS_FILTER_PATH 1
#else
#define ALP_ADC_HAS_FILTER_PATH 0
#endif

#if ALP_ADC_HAS_FILTER_PATH

#define ALP_ADC_FILTER_POOL_SIZE 2u

struct alp_adc_filter {
    bool              in_use;
    alp_adc_stream_t *stream;
    alp_dsp_chain_t  *chain;
};

static struct alp_adc_filter alp_adc_filter_pool[ALP_ADC_FILTER_POOL_SIZE];

static struct alp_adc_filter *alp_adc_filter_pool_acquire(void)
{
    for (size_t i = 0u; i < ALP_ADC_FILTER_POOL_SIZE; i++) {
        if (!alp_adc_filter_pool[i].in_use) {
            return &alp_adc_filter_pool[i];
        }
    }
    return NULL;
}

static void alp_adc_filter_pool_release(struct alp_adc_filter *f)
{
    if (f == NULL) return;
    f->in_use = false;
    f->stream = NULL;
    f->chain  = NULL;
}

alp_adc_filter_t *alp_adc_filter_open(const alp_adc_filter_config_t *cfg)
{
    alp_z_clear_last_error();

    if (cfg == NULL || cfg->stages == NULL || cfg->n_stages == 0u) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

    struct alp_adc_filter *f = alp_adc_filter_pool_acquire();
    if (f == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }

    /* Open the DSP chain first.  Validation rejects FFT-terminated
     * chains via the apply_samples probe below; an FFT chain returns
     * ALP_ERR_NOSUPPORT and we surface that as ALP_ERR_INVAL because
     * the caller used the wrong open() entry point. */
    alp_dsp_chain_t *chain = alp_dsp_chain_open(cfg->stages, cfg->n_stages);
    if (chain == NULL) {
        /* alp_last_error already stamped by alp_dsp_chain_open. */
        alp_adc_filter_pool_release(f);
        return NULL;
    }
    int16_t      probe_in  = 0;
    int16_t      probe_out = 0;
    size_t       probe_got = 0u;
    alp_status_t s = alp_dsp_chain_apply_samples(chain, &probe_in, 1u, &probe_out, 1u, &probe_got);
    if (s == ALP_ERR_NOSUPPORT) {
        /* Caller passed an FFT-terminated chain. */
        alp_dsp_chain_close(chain);
        alp_adc_filter_pool_release(f);
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

    /* Open the underlying stream. */
    const alp_adc_stream_config_t scfg = {
        .channel_id     = cfg->channel_id,
        .sample_rate_hz = cfg->sample_rate_hz,
    };
    alp_adc_stream_t *stream = alp_adc_stream_open(&scfg);
    if (stream == NULL) {
        /* alp_last_error stamped by alp_adc_stream_open. */
        alp_dsp_chain_close(chain);
        alp_adc_filter_pool_release(f);
        return NULL;
    }

    f->stream = stream;
    f->chain  = chain;
    f->in_use = true;
    return f;
}

alp_status_t alp_adc_filter_read(alp_adc_filter_t *filter, int16_t *out_mv, size_t cap, size_t *got)
{
    if (got == NULL) return ALP_ERR_INVAL;
    *got = 0u;
    if (filter == NULL || !filter->in_use) return ALP_ERR_NOT_READY;
    if (out_mv == NULL && cap > 0u) return ALP_ERR_INVAL;
    if (cap == 0u) return ALP_OK;

    /* Drain raw samples in chunks bounded by the backend ceiling. */
    uint16_t     raw[GD32G553_BRIDGE_ADC_STREAM_READ_MAX];
    const size_t want =
        (cap < GD32G553_BRIDGE_ADC_STREAM_READ_MAX) ? cap : GD32G553_BRIDGE_ADC_STREAM_READ_MAX;
    size_t       got_raw = 0u;
    alp_status_t s = alp_adc_stream_read(filter->stream, raw, want, &got_raw);
    if (s != ALP_OK) return s;
    if (got_raw == 0u) return ALP_OK;

    /* Convert uint16 mV samples (0..3300 typical) to int16. */
    int16_t in_buf[GD32G553_BRIDGE_ADC_STREAM_READ_MAX];
    for (size_t i = 0u; i < got_raw; i++) {
        in_buf[i] = (int16_t)raw[i];
    }

    /* Run the chain; chain.apply_samples writes int16 mV out. */
    size_t got_filtered = 0u;
    s = alp_dsp_chain_apply_samples(filter->chain, in_buf, got_raw, out_mv, cap, &got_filtered);
    if (s != ALP_OK) return s;
    *got = got_filtered;
    return ALP_OK;
}

void alp_adc_filter_close(alp_adc_filter_t *filter)
{
    if (filter == NULL) return;
    if (filter->stream != NULL) {
        alp_adc_stream_close(filter->stream);
    }
    if (filter->chain != NULL) {
        alp_dsp_chain_close(filter->chain);
    }
    alp_adc_filter_pool_release(filter);
}

#else /* !ALP_ADC_HAS_FILTER_PATH */

alp_adc_filter_t *alp_adc_filter_open(const alp_adc_filter_config_t *cfg)
{
    alp_z_clear_last_error();
    /* Argument validation first so callers passing bad cfg get a
     * precise INVAL even on SoMs without a bridge backend. */
    if (cfg == NULL || cfg->stages == NULL || cfg->n_stages == 0u) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    alp_z_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
}

alp_status_t alp_adc_filter_read(alp_adc_filter_t *filter, int16_t *out_mv, size_t cap, size_t *got)
{
    /* Mirror the bridge-path contract's pre-checks even when the
     * backend isn't wired -- callers passing a NULL got / NULL
     * handle deserve the precise diagnosis, not a NOSUPPORT smear. */
    if (got == NULL) return ALP_ERR_INVAL;
    *got = 0u;
    if (filter == NULL) return ALP_ERR_NOT_READY;
    (void)out_mv;
    (void)cap;
    return ALP_ERR_NOSUPPORT;
}

void alp_adc_filter_close(alp_adc_filter_t *filter)
{
    (void)filter;
}

#endif /* ALP_ADC_HAS_FILTER_PATH */

/* ====================================================================== */
/* alp_adc_spectrum_t -- FFT-terminated chain (wave-2 §2B.1(c))            */
/*                                                                         */
/* Composes alp_adc_stream_t + alp_dsp_chain_t (FFT-terminated) under one  */
/* handle.  Internally accumulates N samples (N = the chain's FFT          */
/* n_points) before running chain.apply_bins for one non-overlapping       */
/* block per read.  On V2N the chain runs on the host today; the GD32-    */
/* side HW-FFT offload path (CMD_ADC_STREAM_CONFIGURE_DSP) lands once the */
/* wire payload format finalises.  Off-V2N or without CONFIG_ALP_SDK_DSP: */
/* surfaces NOSUPPORT after the INVAL pre-checks.                          */
/* ====================================================================== */

#if ALP_ADC_HAS_FILTER_PATH

#define ALP_ADC_SPECTRUM_POOL_SIZE 2u

struct alp_adc_spectrum {
    bool                 in_use;
    alp_adc_stream_t    *stream;
    alp_dsp_chain_t     *chain;
    uint16_t             fft_n_points;
    alp_dsp_fft_output_t fft_output;
    size_t               accumulated;
    int16_t              samples[ALP_DSP_MAX_FFT_POINTS];
};

static struct alp_adc_spectrum  alp_adc_spectrum_pool[ALP_ADC_SPECTRUM_POOL_SIZE];

static struct alp_adc_spectrum *alp_adc_spectrum_pool_acquire(void)
{
    for (size_t i = 0u; i < ALP_ADC_SPECTRUM_POOL_SIZE; i++) {
        if (!alp_adc_spectrum_pool[i].in_use) {
            return &alp_adc_spectrum_pool[i];
        }
    }
    return NULL;
}

static void alp_adc_spectrum_pool_release(struct alp_adc_spectrum *s)
{
    if (s == NULL) return;
    s->in_use      = false;
    s->stream      = NULL;
    s->chain       = NULL;
    s->accumulated = 0u;
}

alp_adc_spectrum_t *alp_adc_spectrum_open(const alp_adc_spectrum_config_t *cfg)
{
    alp_z_clear_last_error();

    if (cfg == NULL || cfg->stages == NULL || cfg->n_stages == 0u) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

    /* The terminal stage MUST be FFT for spectrum_open.  Chain
     * validation itself rejects FFT-not-terminal and WINDOW-not-
     * before-FFT, so probing the caller's last-stage kind here
     * catches the wrong-entry-point case early (before allocating
     * a chain slot). */
    const alp_dsp_stage_t *last = &cfg->stages[cfg->n_stages - 1u];
    if (last->kind != ALP_DSP_STAGE_FFT) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

    struct alp_adc_spectrum *s = alp_adc_spectrum_pool_acquire();
    if (s == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }

    alp_dsp_chain_t *chain = alp_dsp_chain_open(cfg->stages, cfg->n_stages);
    if (chain == NULL) {
        /* alp_last_error stamped by alp_dsp_chain_open. */
        alp_adc_spectrum_pool_release(s);
        return NULL;
    }

    const alp_adc_stream_config_t scfg = {
        .channel_id     = cfg->channel_id,
        .sample_rate_hz = cfg->sample_rate_hz,
    };
    alp_adc_stream_t *stream = alp_adc_stream_open(&scfg);
    if (stream == NULL) {
        /* alp_last_error stamped by alp_adc_stream_open. */
        alp_dsp_chain_close(chain);
        alp_adc_spectrum_pool_release(s);
        return NULL;
    }

    s->stream       = stream;
    s->chain        = chain;
    s->fft_n_points = last->u.fft.n_points;
    s->fft_output   = last->u.fft.output_format;
    s->accumulated  = 0u;
    s->in_use       = true;
    return s;
}

alp_status_t alp_adc_spectrum_read_bins(alp_adc_spectrum_t *spec, float *bins, size_t cap,
                                        size_t *got)
{
    if (got == NULL) return ALP_ERR_INVAL;
    *got = 0u;
    if (spec == NULL || !spec->in_use) return ALP_ERR_NOT_READY;
    if (bins == NULL) return ALP_ERR_INVAL;

    /* Required output element count per block.  Reject early if the
     * caller's buffer can't hold one block. */
    const size_t need = (spec->fft_output == ALP_DSP_FFT_OUTPUT_COMPLEX)
                            ? (size_t)(2u * spec->fft_n_points)
                            : (size_t)spec->fft_n_points;
    if (cap < need) return ALP_ERR_OUT_OF_RANGE;

    /* Drain raw mV samples into the accumulator until we have a
     * full FFT block.  If the stream's backend ring is empty, this
     * pass produces no bins (got = 0). */
    while (spec->accumulated < spec->fft_n_points) {
        const size_t want_total = spec->fft_n_points - spec->accumulated;
        const size_t want = (want_total < GD32G553_BRIDGE_ADC_STREAM_READ_MAX)
                                ? want_total
                                : (size_t)GD32G553_BRIDGE_ADC_STREAM_READ_MAX;
        uint16_t     raw[GD32G553_BRIDGE_ADC_STREAM_READ_MAX];
        size_t       got_raw = 0u;
        alp_status_t s       = alp_adc_stream_read(spec->stream, raw, want, &got_raw);
        if (s != ALP_OK) return s;
        if (got_raw == 0u) {
            /* Backend ring was empty; caller should poll again
             * later.  Not an error -- partial accumulation persists
             * across calls. */
            return ALP_OK;
        }
        for (size_t i = 0u; i < got_raw; i++) {
            spec->samples[spec->accumulated + i] = (int16_t)raw[i];
        }
        spec->accumulated += got_raw;
    }

    /* Run the chain over the accumulated block. */
    size_t       got_bins = 0u;
    alp_status_t s        = alp_dsp_chain_apply_bins(spec->chain, spec->samples, spec->fft_n_points,
                                                     bins, cap, &got_bins);
    /* Reset the accumulator for the next non-overlapping block. */
    spec->accumulated = 0u;
    if (s != ALP_OK) return s;
    *got = got_bins;
    return ALP_OK;
}

void alp_adc_spectrum_close(alp_adc_spectrum_t *spec)
{
    if (spec == NULL) return;
    if (spec->stream != NULL) {
        alp_adc_stream_close(spec->stream);
    }
    if (spec->chain != NULL) {
        alp_dsp_chain_close(spec->chain);
    }
    alp_adc_spectrum_pool_release(spec);
}

#else /* !ALP_ADC_HAS_FILTER_PATH */

alp_adc_spectrum_t *alp_adc_spectrum_open(const alp_adc_spectrum_config_t *cfg)
{
    alp_z_clear_last_error();
    if (cfg == NULL || cfg->stages == NULL || cfg->n_stages == 0u) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    /* Mirror the filter-side wrong-entry-point detection so callers
     * get the same INVAL when they pass a filter-terminated chain to
     * the spectrum surface, regardless of whether the bridge path is
     * built. */
    const alp_dsp_stage_t *last = &cfg->stages[cfg->n_stages - 1u];
    if (last->kind != ALP_DSP_STAGE_FFT) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    alp_z_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
}

alp_status_t alp_adc_spectrum_read_bins(alp_adc_spectrum_t *spec, float *bins, size_t cap,
                                        size_t *got)
{
    /* Mirror the bridge-path contract's pre-checks for diagnostic
     * fidelity on backends without HW dispatch. */
    if (got == NULL) return ALP_ERR_INVAL;
    *got = 0u;
    if (spec == NULL) return ALP_ERR_NOT_READY;
    (void)bins;
    (void)cap;
    return ALP_ERR_NOSUPPORT;
}

void alp_adc_spectrum_close(alp_adc_spectrum_t *spec)
{
    (void)spec;
}

#endif /* ALP_ADC_HAS_FILTER_PATH */
