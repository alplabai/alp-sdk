/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * audio-wake-word -- always-on "Hey Alp" keyword spotting on the
 *                    E1M-AEN family's low-power AI subsystem.
 *
 * ─── The pitch ────────────────────────────────────────────────
 *
 *   PDM mic ──▶ MFCC features ──▶ tiny CNN (~30k params) ──▶ wake event
 *   16 kHz S16    13 coeffs/frame    Ethos-U55 burst @ <50 ms     │
 *                                                                  ▼
 *                                                          M55 HP wake-up
 *                                                          (heavy ASR / cloud)
 *
 * The Cortex-M55 HE ("High Efficiency") core sits at ~50 MHz
 * running this loop 24/7.  Each 50 ms inference window dispatches
 * the convolutions to the on-die Ethos-U55 NPU and parks the
 * M55 HE in WFI while the NPU runs -- the duty-cycle math
 * lands the average system power below 1 mW.  On a wake-word
 * hit the M55 HE asserts the WIC line to bring up the M55 HP
 * core for ASR / command parsing / network upload.
 *
 * E1M-V2N has no always-on AI subsystem (the A55 cluster + M33
 * lockstep doesn't have the Cortex-M55 HE / Ethos-U55 pairing),
 * so this demo is AEN-only.  Same source compiles cleanly on
 * native_sim with the CPU TFLM fallback for desk-side iteration.
 *
 * ─── What's stubbed in v0.5 ────────────────────────────────────
 *
 * - The real model weights aren't checked in -- s_model[] holds a
 *   placeholder.  TODO(v0.6) drops a Vela-compiled wake-word
 *   model into models/ and #includes the generated header.
 * - MFCC feature extraction is a stub that fills the input tensor
 *   with zeros.  The DSP chain (FFT + mel filterbank + DCT) lands
 *   in v0.6 once <alp/dsp.h>'s spectrum stage is wired through
 *   <alp/audio.h>.
 * - WIC / M55 HP wake-up is paper-correct -- the cross-core IRQ
 *   path lands with the mproc-mailbox v0.6 work.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "alp/audio.h"
#include "alp/e1m_pinout.h"
#include "alp/inference.h"

LOG_MODULE_REGISTER(audio_wake_word, LOG_LEVEL_INF);

/* ── Audio + feature constants ─────────────────────────────────
 *
 * 16 kHz mono S16 is the standard front-end for keyword spotters.
 * A 50 ms window @ 16 kHz = 800 frames; we round to 512 frames
 * (32 ms) for the inference cadence to align with a power-of-two
 * FFT length when the MFCC stage lands in v0.6.
 */
#define SR_HZ 16000
#define CHANNELS 1
#define BLOCK_FRAMES 512
#define MFCC_FRAMES 32
#define MFCC_COEFFS 13

/* ── Wake-word inference window ────────────────────────────────
 *
 * The CNN sees a 32-frame × 13-coeff spectrogram (~1 s of audio
 * once striding is taken into account).  ~30k parameters int8
 * quantised -- fits comfortably in 64 KiB of arena.
 */
#define WAKE_INFER_INTERVAL_MS 50
#define WAKE_LOOP_ITERATIONS 8

/* ── Demo budget ───────────────────────────────────────────────
 *
 * Real always-on builds loop forever.  Twister's console harness
 * needs the example to terminate so it can match the "[wake] done"
 * sentinel and tear the test process down; we cap the loop at
 * WAKE_LOOP_ITERATIONS for build_only verification.
 */

/* ── Placeholder model bytes ───────────────────────────────────
 *
 * TODO(v0.6): replace with the Vela-compiled wake-word model
 *   #include "models/hey_alp_vela.h"
 * The s_model array below is a sentinel so alp_inference_open
 * has something non-NULL to chew on under native_sim (the stub
 * backend returns NULL anyway; the surrounding code tolerates
 * that and prints "[wake] done" so twister can match).
 */
static const uint8_t s_model[] = { 0x00 };

/* Tensor arena -- 64 KiB matches board.yaml's default_arena_kib. */
static uint8_t s_arena[64 * 1024] __aligned(16);

/* PCM block buffer.  512 frames * 1 ch * 2 B = 1 KiB. */
static int16_t s_pcm[BLOCK_FRAMES * CHANNELS];

/* MFCC feature matrix that feeds the CNN's input tensor.  32 x 13
 * int8 = 416 B.  The extract step (v0.6) fills this from s_pcm. */
static int8_t s_mfcc[MFCC_FRAMES * MFCC_COEFFS];

/* Shared state between the audio thread and main(). */
static struct {
	alp_audio_in_t  *mic;
	alp_inference_t *inf;
	bool             mic_ok;
	bool             inf_ok;
	uint32_t         windows_run;
	uint32_t         detections;
} g_state;

/* ── Inference thread stack ────────────────────────────────────
 *
 * Low priority (K_PRIO_PREEMPT(10)) -- the whole point is that
 * the loop yields aggressively so the M55 HE can sit in WFI
 * between bursts.  Main() runs at default priority and stays
 * available for any cooperative housekeeping.
 */
K_THREAD_STACK_DEFINE(infer_stack, 4096);
static struct k_thread infer_thread;

/* ── MFCC feature stub ─────────────────────────────────────────
 *
 * Real implementation (v0.6): pre-emphasis -> windowed FFT ->
 * mel filterbank (40 bins) -> log -> DCT -> 13 coefficients.
 * Most of that work lands on the AEN's Helium MVE in the M55 HE
 * core directly; only the convolutions go to the NPU.
 */
static void extract_mfcc(const int16_t *pcm, size_t frames, int8_t *out)
{
	/* Silence the unused-arg warning until v0.6 wires this up. */
	(void)pcm;
	(void)frames;
	memset(out, 0, MFCC_FRAMES * MFCC_COEFFS);
}

/* ── Wake-word post-process stub ───────────────────────────────
 *
 * Real model output: 2-class softmax (silence vs "Hey Alp").
 * v0.5 stub returns false unconditionally so the demo's
 * "[wake] done" sentinel is the loop-exit trigger, not a
 * spurious detection.
 */
static bool decode_wake(alp_inference_t *inf)
{
	/* TODO(v0.6): alp_inference_get_output() -> int8 logits ->
     * dequantise via the tensor's scale/zero_point -> argmax. */
	(void)inf;
	return false;
}

/* ── Inference loop ────────────────────────────────────────────
 *
 * Runs in the low-priority worker thread.  Each iteration:
 *   1. Read a 512-frame PCM block from the PDM mic.
 *   2. Extract MFCC features (stub in v0.5).
 *   3. Copy MFCC into the model's input tensor.
 *   4. alp_inference_invoke() -- dispatches to Ethos-U55 on AEN,
 *      CPU on native_sim.
 *   5. decode_wake() -- if true, fire the wake event.
 *   6. k_sleep() -- park the core until the next 50 ms window.
 */
static void infer_loop(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (uint32_t i = 0; i < WAKE_LOOP_ITERATIONS; ++i) {
		/* 1. Read a PCM block.  On native_sim the mic is NULL and
         *    we just zero-fill the buffer so the rest of the
         *    pipeline still exercises. */
		if (g_state.mic_ok) {
			size_t got = 0;
			(void)alp_audio_in_read(g_state.mic, s_pcm, BLOCK_FRAMES, &got,
			                        /*timeout_ms=*/100);
		} else {
			memset(s_pcm, 0, sizeof(s_pcm));
		}

		/* 2. MFCC features. */
		extract_mfcc(s_pcm, BLOCK_FRAMES, s_mfcc);

		/* 3 + 4. Push features into the model + invoke. */
		if (g_state.inf_ok) {
			/* TODO(v0.6): copy s_mfcc into the model input tensor
             * via alp_inference_get_input() before invoking. */
			(void)alp_inference_invoke(g_state.inf);
		}

		/* 5. Detect. */
		if (decode_wake(g_state.inf)) {
			g_state.detections++;
			printf("[wake] hit! Hey Alp detected\n");
			/* TODO(v0.6): assert WIC line to wake the M55 HP core
             * via the mproc-mailbox cross-core IRQ. */
		}
		g_state.windows_run++;

		/* 6. Park until the next window.  k_sleep yields to the
         *    scheduler -- on a real AEN HE core the Zephyr idle
         *    thread drops to WFI which is what gets us the
         *    <1 mW average draw. */
		k_msleep(WAKE_INFER_INTERVAL_MS);
	}
}

int main(void)
{
	printf("[wake] audio-wake-word v0.5 -- always-on KWS on E1M-AEN\n");

	/* ── Open the PDM mic ──────────────────────────────────
     *
     * 16 kHz mono S16 is the keyword-spotting standard.  On
     * native_sim this returns NULL with last_err = NOSUPPORT;
     * the loop tolerates that and runs the inference path
     * against zero-fill PCM. */
	alp_audio_config_t mic_cfg = {
		.peripheral_id    = E1M_PDM0,
		.sample_rate_hz   = SR_HZ,
		.channels         = CHANNELS,
		.format           = ALP_AUDIO_FMT_S16_LE,
		.frames_per_block = BLOCK_FRAMES,
	};
	g_state.mic    = alp_audio_in_open(&mic_cfg);
	g_state.mic_ok = (g_state.mic != NULL);
	if (g_state.mic_ok) {
		printf("[wake]   alp_audio_in_open(PDM0)       ok\n");
		(void)alp_audio_in_start(g_state.mic);
	} else {
		printf("[wake]   alp_audio_in_open(PDM0)       skip (no DMIC, last_err=%d)\n",
		       (int)alp_last_error());
	}

	/* ── Open the inference backend ────────────────────────
     *
     * ETHOS_U dispatches to the on-die Ethos-U55 NPU on the
     * AEN.  AUTO would do the same but spelling out the
     * backend makes the AEN-specific intent explicit. */
	alp_inference_config_t inf_cfg = {
		.backend     = ALP_INFERENCE_BACKEND_ETHOS_U,
		.format      = ALP_INFERENCE_MODEL_VELA,
		.model_data  = s_model,
		.model_size  = sizeof(s_model),
		.arena       = s_arena,
		.arena_bytes = sizeof(s_arena),
	};
	g_state.inf    = alp_inference_open(&inf_cfg);
	g_state.inf_ok = (g_state.inf != NULL);
	if (g_state.inf_ok) {
		printf("[wake]   alp_inference_open(ETHOS_U)   ok\n");
	} else {
		printf("[wake]   alp_inference_open(ETHOS_U)   skip (last_err=%d)\n",
		       (int)alp_last_error());
	}

	/* ── Spawn the inference loop at low priority ──────────
     *
     * K_PRIO_PREEMPT(10) is well below main() -- the scheduler
     * lets main() (and any future supervisory work) take the
     * core whenever they have something to do.  Between
     * inference windows the loop is k_sleep'd which drops the
     * idle thread to WFI on the M55 HE. */
	k_thread_create(&infer_thread, infer_stack, K_THREAD_STACK_SIZEOF(infer_stack), infer_loop,
	                NULL, NULL, NULL, K_PRIO_PREEMPT(10), 0, K_NO_WAIT);
	k_thread_name_set(&infer_thread, "wake_infer");

	/* ── Wait for the loop to retire ───────────────────────
     *
     * On a real always-on deployment main() would either join
     * the worker forever or just return (Zephyr keeps the
     * worker alive).  For the twister build_only scenario we
     * block on the thread so the "[wake] done" sentinel only
     * fires after the loop has had a chance to exercise. */
	k_thread_join(&infer_thread, K_FOREVER);

	/* ── Teardown ──────────────────────────────────────────
     *
     * Real always-on apps skip this -- the worker never
     * exits.  We tear down here so the demo's resource
     * lifecycle is visible end-to-end for customers reading
     * the example. */
	if (g_state.inf_ok) {
		alp_inference_close(g_state.inf);
	}
	if (g_state.mic_ok) {
		(void)alp_audio_in_stop(g_state.mic);
		alp_audio_in_close(g_state.mic);
	}

	printf("[wake]   %u windows run, %u detections\n", g_state.windows_run, g_state.detections);
	printf("[wake] done\n");
	return 0;
}
