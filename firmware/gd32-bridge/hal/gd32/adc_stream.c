/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GD32G5x3 bridge HAL backend -- DMA-paced ADC streaming + DSP chain pool.
 * Split move-only from hal/bridge_hw_gd32.c (fw v0.2.8); see
 * hal/gd32/init.c for the backend-wide implementation notes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_hw.h"
#include "gd32g5x3.h"

#include "gd32_common.h"

/* Stream slots; layout + sizing doc in gd32_common.h. */
adc_stream_state_t adc_streams[BRIDGE_ADC_STREAM_COUNT];

/* TRIGSEL route target for an ADC peripheral's routine-group trigger. */
static trigsel_periph_enum adc_stream_routrg(uint32_t adc_periph)
{
    if (adc_periph == ADC1) return TRIGSEL_OUTPUT_ADC1_ROUTRG;
    if (adc_periph == ADC2) return TRIGSEL_OUTPUT_ADC2_ROUTRG;
    if (adc_periph == ADC3) return TRIGSEL_OUTPUT_ADC3_ROUTRG;
    return TRIGSEL_OUTPUT_ADC0_ROUTRG;
}

/* DMA write-cursor read.  The DMA channel counter counts DOWN from
 * the configured transfer length; converting to a write index uses
 * `ring_samples - remaining`.  Wraps naturally via the circular-mode
 * reload. */
static uint16_t adc_stream_write_index(const adc_stream_state_t *s)
{
    const uint32_t remaining =
        dma_transfer_number_get(s->dma_periph, (dma_channel_enum)s->dma_channel);
    if (remaining > BRIDGE_ADC_STREAM_RING_SAMPLES) return 0u;
    return (uint16_t)(BRIDGE_ADC_STREAM_RING_SAMPLES - remaining);
}

int bridge_hw_adc_stream_begin(uint8_t stream_id, uint8_t channel, uint32_t sample_rate_hz)
{
    if (stream_id >= BRIDGE_ADC_STREAM_COUNT) return BRIDGE_HW_ERR_RANGE;
    if (channel >= ADC_CHANNEL_MAP_COUNT) return BRIDGE_HW_ERR_RANGE;
    if (sample_rate_hz == 0u) return BRIDGE_HW_ERR_INVAL;
    if (sample_rate_hz > BRIDGE_ADC_STREAM_RATE_MAX_HZ) return BRIDGE_HW_ERR_RANGE;
    if (!vref_ready_check()) return BRIDGE_HW_ERR_IO; /* dead reference -- fail loud */

    adc_stream_state_t *s = &adc_streams[stream_id];
    if (s->in_use) return BRIDGE_HW_ERR_INVAL; /* stream already running */

    const gd32_adc_ch_t *ch = &adc_channels_map[channel];

    /* One stream per ADC converter: both streams sharing a peripheral
     * would fight over routine rank 0 AND the TRIGSEL routine-trigger
     * route -- the second begin would silently re-pace and re-point
     * the first.  Refuse honestly instead. */
    for (uint8_t i = 0u; i < BRIDGE_ADC_STREAM_COUNT; ++i) {
        if (i != stream_id && adc_streams[i].in_use &&
            adc_channels_map[adc_streams[i].channel].periph == ch->periph) {
            return BRIDGE_HW_ERR_INVAL;
        }
    }

    /* Stream 0 -> DMA0, stream 1 -> DMA1.  Channel 0 of each DMA
     * controller is the first free slot in the GD32G5x3 dma_channel
     * enum; bridge brings up no other DMA users today so collisions
     * are not a concern. */
    s->dma_periph  = (stream_id == 0u) ? DMA0 : DMA1;
    s->dma_channel = (uint8_t)DMA_CH0;
    s->pace_timer  = (stream_id == 0u) ? TIMER5 : TIMER6;

    /* The DMAMUX request routing dma_init() writes below lands on a
     * clock-gated register unless the mux clock is up.  The SPI
     * transport happens to enable it first at boot today -- own the
     * dependency here instead of relying on bring-up order (silicon
     * 2026-06-04 audit: an I2C-only build would stream zero samples). */
    rcu_periph_clock_enable(RCU_DMAMUX);
    rcu_periph_clock_enable((stream_id == 0u) ? RCU_DMA0 : RCU_DMA1);
    dma_deinit(s->dma_periph, (dma_channel_enum)s->dma_channel);

    dma_parameter_struct init;
    dma_struct_para_init(&init); /* all fields defined before the explicit set */
    init.periph_addr  = (uint32_t)(uintptr_t)&ADC_RDATA(ch->periph);
    init.memory_addr  = (uint32_t)(uintptr_t)s->ring;
    init.direction    = DMA_PERIPHERAL_TO_MEMORY;
    init.number       = BRIDGE_ADC_STREAM_RING_SAMPLES;
    init.periph_inc   = DMA_PERIPH_INCREASE_DISABLE;
    init.memory_inc   = DMA_MEMORY_INCREASE_ENABLE;
    init.periph_width = DMA_PERIPHERAL_WIDTH_16BIT;
    init.memory_width = DMA_MEMORY_WIDTH_16BIT;
    init.priority     = DMA_PRIORITY_MEDIUM;
    /* DMAMUX request: route the channel to this ADC instance.  Without
     * this the request id is left uninitialised and the channel triggers
     * on the wrong (or no) source. */
    init.request = (ch->periph == ADC1)   ? DMA_REQUEST_ADC1
                   : (ch->periph == ADC2) ? DMA_REQUEST_ADC2
                   : (ch->periph == ADC3) ? DMA_REQUEST_ADC3
                                          : DMA_REQUEST_ADC0;
    dma_init(s->dma_periph, (dma_channel_enum)s->dma_channel, &init);

    /* Circular mode -- DMA reloads `number` after each cycle so the
     * channel keeps running without firmware re-arms.  Combined with
     * adc_dma_mode_enable below this produces a steady-state
     * peripheral-to-ring pipeline with no firmware in the hot path. */
    dma_circulation_enable(s->dma_periph, (dma_channel_enum)s->dma_channel);

    /* Reconfigure the converter for streaming with ADCON CLEAR, in
     * the vendor's proven order (Examples/ADC/ADC0_routine_channel_
     * with_DMA): mode + trigger + DMA controls all land BEFORE the
     * enable.  Programming CTL1 on an already-running converter is
     * exactly how the v0.2.3 stream silently produced zero samples.
     * Calibration is NOT redone here: an ADCON toggle preserves the
     * boot calibration from adc_periph_init, and recalibrating would
     * be an unbounded vendor spin inside the CS-EXTI handler. */
    adc_disable(ch->periph);
    adc_routine_channel_config(ch->periph, 0u, ch->channel, adc_sample_cycles_cache[channel]);

    /* Each pacing-timer TRGO edge starts exactly ONE routine
     * conversion -- the honest realisation of `sample_rate_hz`.  No
     * continuous mode: the silicon ignores trigger edges that land
     * mid-conversion, so an over-asked rate degrades to the channel's
     * achievable rate instead of corrupting the ring. */
    adc_external_trigger_config(ch->periph, ADC_ROUTINE_CHANNEL, EXTERNAL_TRIGGER_RISING);

    /* THE v0.2.3 got==0 root cause: CTL1.DMA alone stops issuing
     * requests after one DMA run.  CTL1.DDM (request-after-last) keeps
     * the request line live so the circular channel refills forever --
     * the vendor reference enables BOTH, in this order. */
    adc_dma_request_after_last_enable(ch->periph);
    adc_dma_mode_enable(ch->periph);

    /* Clear any End-Of-Conversion left by a prior single-shot
     * bridge_hw_adc_read on this peripheral BEFORE the converter
     * re-enables -- a stale EOC otherwise fires one spurious DMA
     * beat the moment the request unmasks, depositing a phantom
     * zeroth sample and desynchronising the ring cursor. */
    adc_flag_clear(ch->periph, ADC_FLAG_EOC);
    adc_enable(ch->periph);
    for (volatile uint32_t stab = 0u; stab < 4096u; ++stab) {
        /* tSTAB dwell after ADCON, same bound adc_periph_init uses */
    }

    dma_channel_enable(s->dma_periph, (dma_channel_enum)s->dma_channel);

    /* Route the pacing timer's update-event TRGO0 to this converter's
     * routine trigger, then run the timer at the requested rate.  Two
     * prescaler regimes keep the 16-bit period in range: a 1 MHz tick
     * covers 16 Hz..100 kHz exactly where it matters; below 16 Hz a
     * 10 kHz tick stretches to 1 Hz.  Division truncates -- worst-case
     * quantisation is one tick (documented in the protocol spec). */
    rcu_periph_clock_enable(RCU_TRIGSEL);
    trigsel_init(adc_stream_routrg(ch->periph),
                 (stream_id == 0u) ? TRIGSEL_INPUT_TIMER5_TRGO0 : TRIGSEL_INPUT_TIMER6_TRGO0);

    rcu_periph_clock_enable((stream_id == 0u) ? RCU_TIMER5 : RCU_TIMER6);
    timer_deinit(s->pace_timer);
    uint32_t psc, period_ticks;
    if (sample_rate_hz >= 16u) {
        psc          = (BRIDGE_ADC_PACE_CLK_HZ / 1000000u) - 1u; /* 1 MHz tick  */
        period_ticks = 1000000u / sample_rate_hz;                /* 10..62500   */
    } else {
        psc          = (BRIDGE_ADC_PACE_CLK_HZ / 10000u) - 1u; /* 10 kHz tick */
        period_ticks = 10000u / sample_rate_hz;                /* 667..10000  */
    }
    timer_parameter_struct tp;
    timer_struct_para_init(&tp);
    tp.prescaler = (uint16_t)psc;
    tp.period    = period_ticks - 1u;
    timer_init(s->pace_timer, &tp);
    timer_master_output0_trigger_source_select(s->pace_timer, TIMER_TRI_OUT0_SRC_UPDATE);
    timer_enable(s->pace_timer);

    s->in_use       = true;
    s->channel      = channel;
    s->read_idx     = 0u;
    s->dsp_chain_id = 0u;
    s->dsp_bound    = false;
    return BRIDGE_HW_OK;
}

int bridge_hw_adc_stream_read(uint8_t stream_id, uint8_t max_samples, uint8_t *got_samples,
                              uint16_t *mv)
{
    if (got_samples == 0) return BRIDGE_HW_ERR_INVAL;
    *got_samples = 0u;
    if (mv == 0) return BRIDGE_HW_ERR_INVAL;
    if (stream_id >= BRIDGE_ADC_STREAM_COUNT) return BRIDGE_HW_ERR_RANGE;

    adc_stream_state_t *s = &adc_streams[stream_id];
    if (!s->in_use) return BRIDGE_HW_ERR_INVAL;

    /* Drain as many fresh samples as the host asked for, capped by
     * what the DMA has actually deposited since the last read.  The
     * DMA cursor walks forward through the ring; producer/consumer
     * indices wrap independently, and a write cursor that has lapped
     * the reader is detected via the unsigned-arithmetic delta. */
    const uint16_t w = adc_stream_write_index(s);
    uint16_t       avail;
    if (w >= s->read_idx) {
        avail = (uint16_t)(w - s->read_idx);
    } else {
        avail = (uint16_t)((BRIDGE_ADC_STREAM_RING_SAMPLES - s->read_idx) + w);
    }
    if (avail == 0u) return BRIDGE_HW_OK; /* empty ring; host should poll later */

    uint16_t to_emit = (avail < max_samples) ? avail : max_samples;
    for (uint16_t i = 0u; i < to_emit; ++i) {
        uint32_t code = s->ring[s->read_idx];
        if (code > ADC_FULL_SCALE) code = ADC_FULL_SCALE;
        mv[i]       = (uint16_t)((code * ADC_VREF_MV) / ADC_FULL_SCALE);
        s->read_idx = (uint16_t)((s->read_idx + 1u) % BRIDGE_ADC_STREAM_RING_SAMPLES);
    }
    *got_samples = (uint8_t)to_emit;
    return BRIDGE_HW_OK;
}

int bridge_hw_adc_stream_end(uint8_t stream_id)
{
    if (stream_id >= BRIDGE_ADC_STREAM_COUNT) return BRIDGE_HW_ERR_RANGE;
    adc_stream_state_t *s = &adc_streams[stream_id];
    if (!s->in_use) return BRIDGE_HW_OK; /* idempotent */

    /* Stop the trigger SOURCE first (pacing timer), then disarm the
     * ADC's DMA request generation, then the DMA channel -- the other
     * order can leave one in-flight transfer landing after the
     * channel is disabled. */
    const gd32_adc_ch_t *ch = &adc_channels_map[s->channel];
    timer_disable(s->pace_timer);
    timer_deinit(s->pace_timer);
    adc_dma_request_after_last_disable(ch->periph);
    adc_dma_mode_disable(ch->periph);
    dma_channel_disable(s->dma_periph, (dma_channel_enum)s->dma_channel);

    /* A trigger edge may have started a conversion just before the
     * timer stopped.  Dwell past one conversion time (~6.3 us healthy;
     * the spin below is comfortably longer) so it lands, then clear
     * EOC unconditionally -- whether the last EOC went to the DMA or
     * is still latched, the converter must idle CLEAN.  A leftover
     * conversion/EOC straddling into the next single-shot read on the
     * same peripheral is what started the 2026-06-04 link-rot chain. */
    for (volatile uint32_t settle = 0u; settle < 8192u; ++settle) {
        /* fixed dwell, ~tens of microseconds */
    }

    /* Full single-shot restore: deinit + reconfigure + recalibrate
     * (calibration BOUNDED -- this runs in the CS-EXTI handler).  This
     * puts EXTERNAL_TRIGGER_DISABLE, routine length 1 and a fresh
     * calibration back so a following bridge_hw_adc_read sees the
     * exact converter state adc_periph_init promised it -- the same
     * self-heal shape the read path's timeout branch uses.  The stream
     * state clears regardless of the restore verdict (the stream IS
     * over); a calibration that never completed reports IO so the host
     * knows the converter came back in an unproven state. */
    const bool restored = adc_periph_init(ch->periph);

    s->in_use           = false;
    s->dsp_bound        = false;
    return restored ? BRIDGE_HW_OK : BRIDGE_HW_ERR_IO;
}

/* ----------------------------------------------------------------- */
/* v0.5 (§2B wave-2) -- chunked DSP-chain upload                     */
/* ----------------------------------------------------------------- */

/* Pool sizing -- mirrors the constants in `<alp/chips/gd32g553.h>`
 * so the host's view of "what fits" agrees with the firmware's
 * actual buffer reservation.  These local copies avoid pulling the
 * SDK header into the firmware tree (which would drag in alp_status_t
 * + supplementary ALP types the firmware doesn't otherwise consume).
 * Bumping any of them requires a coordinated edit on both sides --
 * see `docs/gd32-bridge-protocol.md` §3.x for the wire-format
 * implications. */
#define BRIDGE_DSP_MAX_CHAINS 4u
#define BRIDGE_DSP_MAX_STAGES 4u
#define BRIDGE_DSP_MAX_STAGE_BYTES 260u

/* Valid `kind` byte range -- alp_dsp_stage_kind_t mirrors the wire
 * encoding: 0 FIR, 1 IIR, 2 WINDOW, 3 FFT.  Anything outside this
 * range rejects at stage_push so a typo from the host is caught
 * before any bytes hit the per-stage buffer. */
#define BRIDGE_DSP_KIND_MAX 3u

typedef struct {
    uint8_t  kind;           /* alp_dsp_stage_kind_t (valid when total_size > 0) */
    uint16_t total_size;     /* declared in first chunk; locks for the stage    */
    uint16_t bytes_received; /* running count toward total_size                  */
    bool     complete;       /* bytes_received == total_size                     */
    uint8_t  data[BRIDGE_DSP_MAX_STAGE_BYTES];
} adc_dsp_stage_t;

typedef struct {
    bool            in_use;
    bool            bound;
    adc_dsp_stage_t stages[BRIDGE_DSP_MAX_STAGES];
} adc_dsp_chain_t;

/* 4 chains x 4 stages x 260 B = 4160 bytes of stage-data RAM + ~80
 * bytes of metadata; well inside the GD32G553's 128 KB SRAM. */
static adc_dsp_chain_t adc_dsp_chains[BRIDGE_DSP_MAX_CHAINS];

int                    bridge_hw_adc_dsp_chain_open(uint8_t *chain_id)
{
    if (chain_id == 0) return BRIDGE_HW_ERR_INVAL;
    *chain_id = 0u;

    /* First-fit search over the chain pool.  The pool is small (4
     * entries today) so the linear scan is comfortably faster than
     * any free-list bookkeeping would be; if the pool grows, this
     * function is the natural place to add a free-list head. */
    for (uint8_t i = 0u; i < BRIDGE_DSP_MAX_CHAINS; ++i) {
        if (!adc_dsp_chains[i].in_use) {
            /* Zero the chain state so a previously-released chain
             * doesn't leak stale stage data into the new allocation. */
            for (uint8_t s = 0u; s < BRIDGE_DSP_MAX_STAGES; ++s) {
                adc_dsp_chains[i].stages[s].kind           = 0u;
                adc_dsp_chains[i].stages[s].total_size     = 0u;
                adc_dsp_chains[i].stages[s].bytes_received = 0u;
                adc_dsp_chains[i].stages[s].complete       = false;
            }
            adc_dsp_chains[i].in_use = true;
            adc_dsp_chains[i].bound  = false;
            *chain_id                = i;
            return BRIDGE_HW_OK;
        }
    }
    /* Pool exhaustion.  Protocol layer maps BRIDGE_HW_ERR_NOTIMPL to
     * STATUS_NOSUPPORT today; a STATUS_NOMEM-equivalent would be
     * more accurate but doesn't exist on the wire yet. */
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_adc_dsp_stage_push(uint8_t chain_id, uint8_t stage_index, uint8_t kind,
                                 uint16_t chunk_offset, uint16_t chunk_total_size,
                                 const uint8_t *chunk_data, size_t chunk_data_len)
{
    if (chain_id >= BRIDGE_DSP_MAX_CHAINS) return BRIDGE_HW_ERR_RANGE;
    if (stage_index >= BRIDGE_DSP_MAX_STAGES) return BRIDGE_HW_ERR_RANGE;
    if (kind > BRIDGE_DSP_KIND_MAX) return BRIDGE_HW_ERR_INVAL;
    if (chunk_total_size == 0u) return BRIDGE_HW_ERR_INVAL;
    if (chunk_total_size > BRIDGE_DSP_MAX_STAGE_BYTES) return BRIDGE_HW_ERR_RANGE;
    if (chunk_data_len == 0u || chunk_data == 0) return BRIDGE_HW_ERR_INVAL;
    /* `chunk_offset + chunk_data_len <= chunk_total_size` -- guard
     * against integer overflow on the addition (both inputs are
     * 16-bit-bounded above) by doing the subtraction. */
    if (chunk_data_len > (size_t)(chunk_total_size - chunk_offset)) return BRIDGE_HW_ERR_RANGE;

    adc_dsp_chain_t *chain = &adc_dsp_chains[chain_id];
    if (!chain->in_use) return BRIDGE_HW_ERR_INVAL;
    if (chain->bound) return BRIDGE_HW_ERR_INVAL; /* mutation after bind */

    adc_dsp_stage_t *st = &chain->stages[stage_index];

    if (chunk_offset == 0u) {
        /* First chunk of this stage.  Seed `kind` + `total_size`;
         * any subsequent chunks must agree with these values so a
         * mid-upload re-target of the stage is caught as INVAL. */
        st->kind           = kind;
        st->total_size     = chunk_total_size;
        st->bytes_received = 0u;
        st->complete       = false;
    } else {
        /* Continuation chunk.  The host must keep the same kind +
         * total_size as the first chunk of this (chain, stage)
         * pair -- otherwise the buffer would be a mix of two
         * different stage payloads. */
        if (st->total_size == 0u) return BRIDGE_HW_ERR_INVAL; /* stage not yet opened */
        if (st->kind != kind) return BRIDGE_HW_ERR_INVAL;
        if (st->total_size != chunk_total_size) return BRIDGE_HW_ERR_INVAL;
        if (st->complete) return BRIDGE_HW_ERR_INVAL; /* already done */
    }

    for (size_t i = 0u; i < chunk_data_len; ++i) {
        st->data[chunk_offset + i] = chunk_data[i];
    }
    st->bytes_received += (uint16_t)chunk_data_len;
    if (st->bytes_received == st->total_size) {
        st->complete = true;
    }
    return BRIDGE_HW_OK;
}

int bridge_hw_adc_dsp_chain_bind(uint8_t chain_id, uint8_t stream_id)
{
    if (chain_id >= BRIDGE_DSP_MAX_CHAINS) return BRIDGE_HW_ERR_RANGE;
    if (stream_id >= BRIDGE_ADC_STREAM_COUNT) return BRIDGE_HW_ERR_RANGE;

    adc_dsp_chain_t *chain = &adc_dsp_chains[chain_id];
    if (!chain->in_use) return BRIDGE_HW_ERR_INVAL;
    if (chain->bound) return BRIDGE_HW_ERR_INVAL; /* already attached */

    /* Validate every populated stage is complete + the chain
     * follows the ordering rules documented in
     * `bridge_hw_adc_dsp_chain_bind`'s contract:
     *   - FFT must be the terminal stage (no stage after it),
     *   - WINDOW must immediately precede FFT,
     *   - empty stages (total_size == 0) are allowed only at
     *     contiguous tail positions -- not interleaved with
     *     populated stages. */
    uint8_t fft_index            = BRIDGE_DSP_MAX_STAGES;
    uint8_t window_index         = BRIDGE_DSP_MAX_STAGES;
    uint8_t last_populated_index = BRIDGE_DSP_MAX_STAGES;
    for (uint8_t i = 0u; i < BRIDGE_DSP_MAX_STAGES; ++i) {
        adc_dsp_stage_t *st = &chain->stages[i];
        if (st->total_size == 0u) continue;
        if (!st->complete) return BRIDGE_HW_ERR_INVAL; /* mid-upload */
        if (last_populated_index != BRIDGE_DSP_MAX_STAGES &&
            (uint8_t)(i - last_populated_index) != 1u) {
            return BRIDGE_HW_ERR_INVAL; /* gap in stage list */
        }
        last_populated_index = i;
        if (st->kind == 3u /* FFT */) {
            if (fft_index != BRIDGE_DSP_MAX_STAGES) return BRIDGE_HW_ERR_INVAL;
            fft_index = i;
        } else if (st->kind == 2u /* WINDOW */) {
            if (window_index != BRIDGE_DSP_MAX_STAGES) return BRIDGE_HW_ERR_INVAL;
            window_index = i;
        }
    }
    if (last_populated_index == BRIDGE_DSP_MAX_STAGES) {
        return BRIDGE_HW_ERR_INVAL; /* empty chain */
    }
    if (fft_index != BRIDGE_DSP_MAX_STAGES) {
        /* FFT must be terminal -- no populated stage after it. */
        if (fft_index != last_populated_index) return BRIDGE_HW_ERR_INVAL;
        /* WINDOW (if present) must directly precede the FFT. */
        if (window_index != BRIDGE_DSP_MAX_STAGES &&
            (fft_index == 0u || window_index != fft_index - 1u)) {
            return BRIDGE_HW_ERR_INVAL;
        }
    } else if (window_index != BRIDGE_DSP_MAX_STAGES) {
        /* WINDOW without a terminating FFT has no defined meaning in
         * the filtered-samples path -- reject per docs/gd32-bridge-
         * protocol.md §3.x. */
        return BRIDGE_HW_ERR_INVAL;
    }

    adc_stream_state_t *s = &adc_streams[stream_id];
    if (!s->in_use) return BRIDGE_HW_ERR_INVAL;   /* stream not running */
    if (s->dsp_bound) return BRIDGE_HW_ERR_INVAL; /* stream already has a chain */

    /* Attachment is a state flip on both halves.  Runtime DSP
     * application happens inside stream_read once the wave-2 FFT/FAC
     * dispatch lands; for now the bound chain simply rides alongside
     * the raw stream and the host sees raw mV values until the
     * dispatcher hook ships. */
    s->dsp_chain_id = chain_id;
    s->dsp_bound    = true;
    chain->bound    = true;
    return BRIDGE_HW_OK;
}
