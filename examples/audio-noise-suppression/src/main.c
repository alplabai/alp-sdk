/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * audio-noise-suppression
 * =======================
 *
 * Real-time noise suppression for the headset / hearable /
 * conferencing market.  Mic -> DSP preprocess -> small AI
 * denoiser -> speaker, end-to-end ~10 ms.
 *
 *   ┌─────────────────────┐  I2S0 RX  ┌──────────────────────────┐
 *   │ TAS2563 codec       │ ◀────────│ <alp/audio.h>            │
 *   │ (mic-in path)       │  240 fr   │  alp_audio_in_read       │
 *   └─────────────────────┘  block    │  -> 10 ms @ 24 kHz S16   │
 *                                      └────────────┬─────────────┘
 *                                                   │
 *                                                   ▼
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │ <alp/dsp.h>  Hann window + 256-pt FFT (magnitude bins)       │
 *   │   - on V2N M33  the chain offloads to the GD32G553 bridge's  │
 *   │     FFT/FAC blocks (no native math accelerator on RZ/V2N --  │
 *   │     see project memory `v2n_no_dedicated_math_accelerator`)  │
 *   │   - on A55 / native_sim  CMSIS-DSP runs in-host on Neon /    │
 *   │     reference C                                              │
 *   │ Output: per-bin magnitudes feed the noise-floor estimator.   │
 *   └────────────────────────────┬─────────────────────────────────┘
 *                                │
 *                                ▼
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │ <alp/inference.h>  RNNoise-style denoiser (gain mask per     │
 *   │ bin)                                                          │
 *   │   - AUTO routes to DX-M1 on V2N-M1, CPU TFLM kernels on V2N. │
 *   │   - ~50k params, int8 quantised, fits in 96 KiB arena.       │
 *   │ Output: per-bin gain mask -> applied to FFT bins -> inverse  │
 *   │ FFT (TODO v0.6) -> clean PCM block.                          │
 *   └────────────────────────────┬─────────────────────────────────┘
 *                                │
 *                                ▼ I2S0 TX
 *                       ┌─────────────────┐
 *                       │ TAS2563 codec   │
 *                       │ (speaker-out)   │
 *                       └─────────────────┘
 *
 *
 * ── Why this demo matters ─────────────────────────────────────
 *
 *   1. "Can we ship a USB-C headset / earbud with cloud-grade
 *      noise suppression at <10 ms latency?"  Yes -- the V2N's
 *      M33 + GD32 bridge FFT path keeps the spectral work on
 *      silicon close to the codec, and the on-die NPU (V2N-M1)
 *      runs the gain-mask model below the human-perceivable
 *      glass-to-glass threshold.
 *   2. "Same source for the desktop / conferencing rig?"  Yes --
 *      flipping `som.sku` to a V2H / V2N-M1 retargets the
 *      inference backend without touching app code.
 *   3. "Where does the SW fallback live?"  CMSIS-DSP on the host
 *      CPU.  On native_sim that's the reference C kernels, on a
 *      real V2N A55 cluster the Neon path.  The portable
 *      <alp/dsp.h> chain swap-in costs the customer zero lines.
 *
 *
 * ── Latency budget @ 24 kHz, 240-frame blocks (10 ms each) ────
 *
 *     Mic -> I2S RX DMA:        ~2.0 ms   (one block worth)
 *     <alp/dsp.h> FFT pipe:     ~1.5 ms   (GD32 bridge offload)
 *     <alp/inference.h> invoke: ~3.0 ms   (DX-M1 NPU burst)
 *     Mask apply + IFFT:        ~1.5 ms   (TODO v0.6)
 *     I2S TX DMA push:          ~2.0 ms
 *     ────────────────────────────────
 *     Total:                    ~10.0 ms  (one block period)
 *
 * The block period IS the latency budget -- we ping-pong DMA
 * such that each step touches the previous block while the I2S
 * peripheral grabs the next one.  Missing the budget shows up
 * as DMA underrun (audible glitch); the loop's read/write
 * timeouts (100 ms) are wider than the budget so a HiL test can
 * still report what happened instead of silently stalling.
 *
 *
 * ── What's stubbed in v0.5 ────────────────────────────────────
 *
 * - The real RNNoise-style model isn't checked in -- s_model[]
 *   holds a 1-byte placeholder.  TODO(v0.6) drops a TFLite
 *   noise-suppression model into models/ and #includes the
 *   generated header.
 * - DSP-chain weights / FFT bin-count / hop length / mask-apply
 *   weights are all TODO(v0.6).  The framing path here uses a
 *   Hann window + 256-pt FFT to validate the chain shape only.
 * - The IFFT + overlap-add stage that turns the bin-mask back
 *   into PCM is TODO(v0.6) -- v0.5 ships the bin-magnitudes ->
 *   denoiser dispatch direction.  v0.5 writes the mic block to
 *   the speaker (passthrough) so HiL can still hear the loop.
 * - Native_sim has neither codec nor inference NPU.  Every
 *   *_open returns NULL with NOSUPPORT and the loop tolerates
 *   it -- "[ns] done" still fires for twister.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "alp/audio.h"
#include "alp/dsp.h"
#include "alp/e1m_pinout.h"
#include "alp/inference.h"
#include "alp/peripheral.h"

LOG_MODULE_REGISTER(noise_suppress, LOG_LEVEL_INF);

/* ── Audio block geometry ──────────────────────────────────────
 *
 * 24 kHz mono S16 is the headset / conferencing standard for
 * speech-band denoisers (Nyquist at 12 kHz comfortably covers
 * the speech band; trades aliasing risk for lower data rate
 * versus 48 kHz music-grade).  240 frames per block = 10 ms,
 * which sets BOTH the DMA cadence and the perceived latency
 * budget.  See the header comment for the per-step breakdown.
 */
#define SR_HZ         24000
#define CHANNELS      1
#define BLOCK_FRAMES  240
#define BLOCK_MS      10
#define FFT_POINTS    256
#define DEMO_BLOCKS   50    /* ~500 ms of audio for HiL audibility. */

/* ── Placeholder denoiser model ────────────────────────────────
 *
 * TODO(v0.6): replace with the TFLite-compiled RNNoise-style
 * model
 *     #include "models/rnnoise_v06_int8.h"
 * The 1-byte stub is enough for the v0.5 framing path -- on
 * native_sim alp_inference_open returns NULL anyway; on a real
 * V2N-M1 the loader-emitted backend tolerates a stub model for
 * the bring-up scenario.
 */
static const uint8_t s_model[] = {0x00};

/* Tensor arena -- 96 KiB matches board.yaml's default_arena_kib.
 * Sized for a ~50k-param int8 CNN with two-frame context (current
 * + previous block's bin magnitudes). */
static uint8_t s_arena[96 * 1024] __aligned(16);

/* PCM block buffers.  Two of them so the DSP / inference path can
 * touch the previous block while the I2S DMA grabs the next one
 * (ping-pong, see the latency budget table). */
static int16_t s_pcm_in [BLOCK_FRAMES * CHANNELS];
static int16_t s_pcm_out[BLOCK_FRAMES * CHANNELS];

/* FFT magnitude bins -- 256-pt FFT with MAGNITUDE output emits
 * `n_points` floats (not 2*n; that's the COMPLEX path).  See
 * <alp/dsp.h> for the contract. */
static float s_bin_mag[FFT_POINTS];

/* ── DSP chain: Hann window + 256-pt FFT ───────────────────────
 *
 * Two stages:
 *   1. WINDOW  -- Hann shape, length implied by the next FFT.
 *   2. FFT     -- 256 points, magnitude output (per-bin scalars).
 *
 * On V2N M33 the §D.lib loader routes the chain through the
 * GD32G553 bridge's FFT block (the M33 itself has no math
 * accelerator -- see project memory).  On A55 Yocto + native_sim
 * the chain runs in CMSIS-DSP on the host CPU.
 *
 * TODO(v0.6): tune n_points + window shape to whatever the
 * trained denoiser model expects on its bin input.  Most
 * RNNoise-derivatives use 480-sample frames + 50% overlap; we
 * use 256-pt here as a power-of-two starting point that the
 * <alp/dsp.h> validator accepts today.
 */
static const alp_dsp_stage_t s_dsp_stages[] = {
    {
        .kind = ALP_DSP_STAGE_WINDOW,
        .u.window = { .shape = ALP_DSP_WINDOW_HANN },
    },
    {
        .kind = ALP_DSP_STAGE_FFT,
        .u.fft = {
            .n_points      = FFT_POINTS,
            .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE,
        },
    },
};

/* ── Shared state ──────────────────────────────────────────────
 *
 * Compact bag of handles so the loop function doesn't need a long
 * argument list.  Booleans track which backends actually opened
 * so the loop can degrade gracefully on native_sim (no codec, no
 * NPU) and still produce a "[ns] done" sentinel for twister.
 */
static struct {
    alp_audio_in_t  *mic;
    alp_audio_out_t *spk;
    alp_dsp_chain_t *dsp;
    alp_inference_t *inf;
    bool             mic_ok;
    bool             spk_ok;
    bool             dsp_ok;
    bool             inf_ok;
    uint32_t         blocks_run;
} g_state;

/* ── Push bin magnitudes into the model's input tensor ─────────
 *
 * RNNoise-style denoisers take the current frame's spectrum as
 * the per-frame input.  We dequantise on the float -> int8 hop
 * via the tensor descriptor's scale + zero_point (the loader
 * fills those from the model's quantisation table).
 *
 * Falls through silently on a stub backend that emits rank-0
 * tensors so the framing path keeps running.
 *
 * TODO(v0.6): the real model likely wants a log-mel projection
 * of the bins, not the raw magnitudes.  That preprocess lands as
 * a third DSP stage (mel filterbank) once <alp/dsp.h>'s stage
 * vocabulary grows.
 */
static void push_bins_into_model(alp_inference_t *inf,
                                 const float *bins, size_t n)
{
    alp_inference_tensor_t in = {0};
    if (alp_inference_get_input(inf, 0, &in) != ALP_OK) {
        return;
    }
    if (in.data == NULL || in.size_bytes == 0) {
        return;  /* Stub backend; nothing to fill. */
    }
    if (in.dtype == ALP_INFERENCE_DTYPE_F32) {
        size_t bytes = n * sizeof(float);
        if (bytes > in.size_bytes) bytes = in.size_bytes;
        memcpy(in.data, bins, bytes);
    } else if (in.dtype == ALP_INFERENCE_DTYPE_INT8) {
        int8_t *q = (int8_t *)in.data;
        size_t  cap = (in.size_bytes < n) ? in.size_bytes : n;
        for (size_t i = 0; i < cap; i++) {
            float v  = bins[i] / (in.scale > 0.0f ? in.scale : 1.0f);
            int32_t qv = (int32_t)(v + 0.5f) + in.zero_point;
            if (qv >  127) qv =  127;
            if (qv < -128) qv = -128;
            q[i] = (int8_t)qv;
        }
    }
    /* TODO(v0.6): handle UINT8 / INT16 quantisation flavours
     * once the v0.6 quantisation matrix is locked. */
}

/* ── One block of the audio loop ───────────────────────────────
 *
 * Sequenced left-to-right per the latency-budget diagram:
 *   1. Read a 10 ms PCM block from the mic.
 *   2. Run the DSP chain to get 256 magnitude bins.
 *   3. Push bins into the denoiser, invoke.
 *   4. (TODO v0.6) apply the per-bin gain mask + IFFT.
 *   5. Write a 10 ms PCM block to the speaker.
 *
 * Step 4 is stubbed in v0.5 -- we passthrough s_pcm_in -> s_pcm_out
 * so HiL still hears the loop.  Once the inverse-FFT + overlap-add
 * stage lands in <alp/dsp.h> v0.6, that becomes a third chain
 * (synthesis chain) and the masking step slides between the two.
 */
static void process_one_block(void)
{
    /* 1. Mic-in. */
    if (g_state.mic_ok) {
        size_t got = 0;
        (void)alp_audio_in_read(g_state.mic, s_pcm_in, BLOCK_FRAMES,
                                &got, /*timeout_ms=*/100);
    } else {
        memset(s_pcm_in, 0, sizeof(s_pcm_in));
    }

    /* 2. DSP preprocess (window + FFT -> magnitude bins). */
    if (g_state.dsp_ok) {
        size_t got = 0;
        (void)alp_dsp_chain_apply_bins(g_state.dsp,
                                       s_pcm_in, BLOCK_FRAMES,
                                       s_bin_mag, FFT_POINTS, &got);
    }

    /* 3. Inference (per-bin gain mask).  The output tensor is the
     *    mask itself in the trained model; v0.5 just invokes for
     *    framing-path correctness. */
    if (g_state.inf_ok) {
        push_bins_into_model(g_state.inf, s_bin_mag, FFT_POINTS);
        (void)alp_inference_invoke(g_state.inf);
        /* TODO(v0.6): alp_inference_get_output() -> gain mask ->
         * multiply against the FFT bins (we'd keep the COMPLEX
         * output format then so phase survives) -> IFFT -> OLA. */
    }

    /* 4. v0.5 passthrough -- TODO(v0.6) replace with the
     *    masked + IFFT + overlap-add synthesis path. */
    memcpy(s_pcm_out, s_pcm_in, sizeof(s_pcm_out));

    /* 5. Speaker-out. */
    if (g_state.spk_ok) {
        size_t pushed = 0;
        (void)alp_audio_out_write(g_state.spk, s_pcm_out, BLOCK_FRAMES,
                                  &pushed, /*timeout_ms=*/100);
    }

    g_state.blocks_run++;
}

int main(void)
{
    printf("[ns] audio-noise-suppression v0.5 -- mic -> DSP -> AI -> speaker\n");
    printf("[ns]   block=%d frames @ %d Hz mono = %d ms latency target\n",
           BLOCK_FRAMES, SR_HZ, BLOCK_MS);

    /* ── Open mic + speaker on the same I2S link ───────────
     *
     * The TAS2563 codec on the EVK exposes both directions on
     * I2S0 (the codec handles the PDM-to-I2S conversion on its
     * own pins).  E1M_I2S0 is the portable bus ID; the §D.lib
     * loader wires it to whichever physical I2S instance the SoM
     * preset documented in its peripheral map.  On native_sim
     * neither open() succeeds; the loop tolerates that. */
    alp_audio_config_t cfg = {
        .peripheral_id    = E1M_I2S0,
        .sample_rate_hz   = SR_HZ,
        .channels         = CHANNELS,
        .format           = ALP_AUDIO_FMT_S16_LE,
        .frames_per_block = BLOCK_FRAMES,
    };
    g_state.mic = alp_audio_in_open(&cfg);
    g_state.mic_ok = (g_state.mic != NULL);
    if (g_state.mic_ok) {
        printf("[ns]   alp_audio_in_open(I2S0)         ok\n");
        (void)alp_audio_in_start(g_state.mic);
    } else {
        printf("[ns]   alp_audio_in_open(I2S0)         skip (last_err=%d)\n",
               (int)alp_last_error());
    }

    g_state.spk = alp_audio_out_open(&cfg);
    g_state.spk_ok = (g_state.spk != NULL);
    if (g_state.spk_ok) {
        printf("[ns]   alp_audio_out_open(I2S0)        ok\n");
        (void)alp_audio_out_set_volume(g_state.spk, 0xA0);
        (void)alp_audio_out_start(g_state.spk);
    } else {
        printf("[ns]   alp_audio_out_open(I2S0)        skip (last_err=%d)\n",
               (int)alp_last_error());
    }

    /* ── Open the DSP chain (window + FFT) ─────────────────
     *
     * The chain validator (see <alp/dsp.h>) enforces that
     * WINDOW immediately precedes FFT and that FFT is the
     * terminal stage.  Our two-stage descriptor satisfies
     * both.  Open should succeed even on native_sim (the chain
     * implementation is the portable C/CMSIS-DSP fallback). */
    g_state.dsp = alp_dsp_chain_open(s_dsp_stages,
                                     sizeof(s_dsp_stages) /
                                         sizeof(s_dsp_stages[0]));
    g_state.dsp_ok = (g_state.dsp != NULL);
    if (g_state.dsp_ok) {
        printf("[ns]   alp_dsp_chain_open(hann+fft256) ok\n");
    } else {
        printf("[ns]   alp_dsp_chain_open(hann+fft256) skip (last_err=%d)\n",
               (int)alp_last_error());
    }

    /* ── Open the inference backend ────────────────────────
     *
     * AUTO routes to whatever the §D.lib loader resolved from
     * the SoM preset -- DX-M1 on V2N-M1, CPU TFLM on V2N
     * (no NPU), DX-M2 on V2H once that SKU lands.  App source
     * doesn't change. */
    alp_inference_config_t inf_cfg = {
        .backend     = ALP_INFERENCE_BACKEND_AUTO,
        .format      = ALP_INFERENCE_MODEL_TFLITE,
        .model_data  = s_model,
        .model_size  = sizeof(s_model),
        .arena       = s_arena,
        .arena_bytes = sizeof(s_arena),
    };
    g_state.inf = alp_inference_open(&inf_cfg);
    g_state.inf_ok = (g_state.inf != NULL);
    if (g_state.inf_ok) {
        printf("[ns]   alp_inference_open(AUTO)        ok\n");
    } else {
        printf("[ns]   alp_inference_open(AUTO)        skip (last_err=%d)\n",
               (int)alp_last_error());
    }

    /* ── Steady-state loop ─────────────────────────────────
     *
     * On HiL we loop DEMO_BLOCKS * 10 ms = ~500 ms of audio --
     * enough for a customer to hear "the noise dropped" on a
     * real bench.  On native_sim every backend is NULL, the
     * loop is effectively a memcpy chain, and we exit fast so
     * twister's console harness sees [ns] done within the
     * build timeout.
     *
     * A production deployment runs this forever in a dedicated
     * thread (priority just above the idle thread) -- the M33
     * supervisor sits in WFI between DMA-ready IRQs so the
     * average power is dominated by the codec + DMA engines,
     * not the CPU. */
    printf("[ns]   streaming %d blocks (%d ms total) ...\n",
           DEMO_BLOCKS, DEMO_BLOCKS * BLOCK_MS);
    for (int b = 0; b < DEMO_BLOCKS; ++b) {
        process_one_block();
#ifdef CONFIG_BOARD_NATIVE_SIM
        /* One iteration is enough for native_sim framing. */
        if (b == 0) break;
#endif
    }

    /* ── Teardown ──────────────────────────────────────────
     *
     * All close() calls tolerate NULL so we don't need per-handle
     * guards.  Order: speaker first (so any in-flight DMA drains
     * cleanly), then mic, then DSP chain, then inference. */
    if (g_state.spk_ok) (void)alp_audio_out_stop(g_state.spk);
    if (g_state.mic_ok) (void)alp_audio_in_stop (g_state.mic);
    alp_audio_out_close(g_state.spk);
    alp_audio_in_close (g_state.mic);
    alp_dsp_chain_close(g_state.dsp);
    alp_inference_close(g_state.inf);

    printf("[ns]   ran %u blocks\n", (unsigned)g_state.blocks_run);
    printf("[ns] done\n");
    return 0;
}
