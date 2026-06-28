/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * acoustic-safety-events
 * ======================
 *
 * Always-listening safety/security node.  Pipeline:
 *
 *   PDM mic (<alp/audio.h>, 16 kHz) --512-sample frame-->
 *     acoustic_event (bands/centroid/flatness/rolloff/crest/zcr/rms)
 *       -> <alp/inference.h> 4-class event classifier (deterministic fallback)
 *     --> one ASE record per frame.
 *
 * Honest scope: detects loud, acoustically-distinct events (glass-break /
 * alarm / scream / ambient).  NOT a certified security or life-safety sensor;
 * confounders (music, TV, clattering) cause false positives.  The model is a
 * stub (see models/README.md); with no model the deterministic fallback runs.
 *
 * Two-path mic loop:
 *   - REAL PATH (hardware): alp_audio_in_open returns non-NULL -> read live PCM
 *     from the PDM mic via the Zephyr DMIC audio subsystem.
 *   - SYNTHETIC PATH (native_sim / no mic): open returns NULL -> synth_sample()
 *     fills the frame with a deterministic per-frame waveform that cycles the
 *     four event types (ambient / glass / alarm / scream) so every code path
 *     in the classifier is exercised without hardware.
 *
 * The 1-byte model stub guarantees the AI path falls back to the deterministic
 * rules: the stub's single byte is not a valid TFLite magic, so
 * alp_inference_open() returns NULL, and classify() always calls
 * ase_classify_fallback().  See models/README.md for the training recipe.
 */

/* Standard headers: memcpy for tensor fill, sinf/expf for synthetic audio. */
#include <string.h>
#include <math.h>

/* Zephyr runtime: kernel for k_* APIs, logging for LOG_WRN. */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/*
 * Portable ALP SDK headers (no vendor names visible to application code):
 *   alp/audio.h      -- alp_audio_in_open / _start / _read / _stop / _close
 *   alp/e1m_pinout.h -- E1M_PDM0 (= 0), the first PDM microphone instance
 *   alp/inference.h  -- alp_inference_open / get_input / get_output / _invoke / _close
 * Local header:
 *   acoustic_event.h -- ase_frame_state, ase_features, all ase_* functions
 */
#include "alp/audio.h"
#include "alp/e1m_pinout.h"
#include "alp/inference.h"
#include "acoustic_event.h"

#ifndef M_PI
/* M_PI is a POSIX extension exposed by _GNU_SOURCE.  The #define here ensures
 * it is available even on bare C99 environments without _GNU_SOURCE. */
#define M_PI 3.14159265358979323846
#endif

/* Zephyr log module -- all LOG_* calls in this file carry the "ase" prefix. */
LOG_MODULE_REGISTER(ase, LOG_LEVEL_INF);

/*
 * Number of analysis frames to run in the demo.
 *
 * Each frame is ASE_FRAME_N (512) samples at 16 kHz = 32 ms of audio.
 * 8 frames = ~256 ms total, long enough to cycle all four synthetic event
 * types (ambient / glass-break / alarm / scream) twice.  This finite cap
 * lets twister's console harness match "[ase] done" and tear the process
 * down.  Always-on production builds loop forever (no N_FRAMES cap).
 */
#define N_FRAMES 8

/*
 * 1-byte model stub -- forces the deterministic fallback on every run.
 *
 * alp_inference_open() requires model_data to be non-NULL, so a 1-byte array
 * satisfies that contract.  The byte 0x00 is NOT the TFLite flatbuffer magic
 * (which starts with the offset 0x18 0x00 0x00 0x00 + "TFLITE3"), so the
 * backend's parser rejects it immediately and open() returns NULL.
 *
 * classify() below tests `inf != NULL` before entering the AI path; when
 * inf is NULL it falls straight through to ase_classify_fallback() which
 * applies the deterministic rule-based thresholds.
 *
 * To replace: train a 4-class classifier per models/README.md, compile with
 * Vela, and substitute the compiled blob for s_model[].
 */
static const uint8_t s_model[] = { 0x00 };

/*
 * synth_sample -- deterministic per-event waveform for the synthetic audio path.
 *
 * Returns a float sample in [-1, 1] for synthetic audio frame `frame` at
 * sample index `i` within the frame.  Called when the PDM mic is unavailable
 * (native_sim, CI, bench-disconnected).
 *
 * Each waveform is designed to match the discriminator thresholds in
 * ase_classify_fallback() so the expected event class is produced:
 *
 *   frame % 4 == 0 -> AMBIENT:
 *     A very low-amplitude (~0.002) slow sinusoid.  RMS < 0.02 triggers the
 *     ambient gate (cheapest check, fast exit).
 *
 *   frame % 4 == 1 -> GLASS_BREAK:
 *     A 6 kHz tone with a fast exponential decay envelope (env = e^{-0.02*i}).
 *     High centroid (>4 kHz), high ZCR (broadband HF), and impulsive crest
 *     factor (peak >> RMS because the signal decays rapidly).  All three
 *     conditions of the glass-break gate fire simultaneously.
 *
 *   frame % 4 == 2 -> ALARM:
 *     A steady 3 kHz sine at amplitude 0.3 (RMS ~0.21).  Nearly pure tone
 *     -> spectral flatness < 0.2.  Centroid lands at 3000 Hz, within the
 *     2500--4000 Hz smoke/CO alarm band (EN 54-3 specifies 3150 +/- 500 Hz).
 *
 *   frame % 4 == 3 -> SCREAM:
 *     Voiced harmonic stack: fundamental 800 Hz + 1600 + 2400 + 3200 Hz.
 *     Sustained amplitude -> RMS > 0.1.  Centroid lands in 800--2500 Hz
 *     (human voice band).  Both SCREAM discriminators fire.
 */
static float synth_sample(int frame, int i)
{
	/* t is the time offset in seconds within the 512-sample frame. */
	float t = (float)i / ASE_SR_HZ;
	switch (frame % 4) {
	case 0: /* AMBIENT: amplitude 0.002 keeps RMS well below the 0.02 gate. */
		return 0.002f * sinf((float)i * 0.3f);
	case 1: { /* GLASS_BREAK: 6 kHz burst with fast exponential decay envelope. */
		/* Envelope decays to e^{-1} (~0.37) by sample index 50, giving a
		 * short impulsive burst with high crest factor (peak >> RMS). */
		float env = expf(-(float)i * 0.02f);
		return env * sinf(2.0f * (float)M_PI * 6000.0f * t);
	}
	case 2: /* ALARM: steady 3 kHz pure tone, amplitude 0.3 -> RMS ~ 0.21. */
		return 0.3f * sinf(2.0f * (float)M_PI * 3000.0f * t);
	default: /* SCREAM: four-harmonic voiced stack at 800 / 1600 / 2400 / 3200 Hz.
	          * Amplitude 0.25 per the fundamental; decaying harmonic series. */
		return 0.25f * (1.0f * sinf(2.0f * (float)M_PI * 800.0f * t) +
		                0.6f * sinf(2.0f * (float)M_PI * 1600.0f * t) +
		                0.4f * sinf(2.0f * (float)M_PI * 2400.0f * t) +
		                0.3f * sinf(2.0f * (float)M_PI * 3200.0f * t));
	}
}

/*
 * classify -- AI-then-fallback acoustic event classification.
 *
 * Tries the loaded AI model first; falls back to the deterministic
 * ase_classify_fallback() when:
 *   (a) inf == NULL  -- model open failed (e.g. the 1-byte stub is rejected).
 *   (b) Input tensor mismatch -- wrong dtype, buffer too small, or data NULL.
 *   (c) alp_inference_invoke() returns an error (NPU fault, not ready).
 *   (d) Output tensor can't be read or doesn't have ASE_EVENT_COUNT F32 scores.
 *
 * AI path (when all checks pass):
 *   1. Pack the 14-element feature vector into a flat float buffer.
 *   2. Copy it into the model's input tensor via alp_inference_get_input().
 *   3. alp_inference_invoke() dispatches to the NPU (or CPU TFLM).
 *   4. Read the 4-class softmax output via alp_inference_get_output().
 *   5. Return argmax(scores) as the predicted event with its score as confidence.
 *
 * Deterministic fallback path (ase_classify_fallback):
 *   Applies rule-based thresholds on crest, centroid_hz, zcr, flatness, rms.
 *   See acoustic_event.c for the full threshold table and branch-order rationale.
 *
 * Both paths return struct ase_verdict { ev, confidence }.
 */
static struct ase_verdict classify(alp_inference_t *inf, const struct ase_features *f)
{
	if (inf != NULL) {
		/* Serialise the 14 features into a local flat vector for the model. */
		float vec[ASE_FEATURE_DIM];
		(void)ase_feat_pack(f, vec, ASE_FEATURE_DIM);

		/* Retrieve the model's F32 input tensor; verify dtype and size. */
		alp_inference_tensor_t in = { 0 };
		if (alp_inference_get_input(inf, 0, &in) == ALP_OK && in.dtype == ALP_INFERENCE_DTYPE_F32 &&
		    in.data != NULL && in.size_bytes >= sizeof(vec)) {
			/* Fill the backend-owned tensor buffer with our feature vector. */
			memcpy(in.data, vec, sizeof(vec));
			if (alp_inference_invoke(inf) == ALP_OK) {
				/* Read the 4-class softmax scores from the output tensor. */
				alp_inference_tensor_t out = { 0 };
				if (alp_inference_get_output(inf, 0, &out) == ALP_OK &&
				    out.dtype == ALP_INFERENCE_DTYPE_F32 && out.data != NULL &&
				    out.size_bytes >= ASE_EVENT_COUNT * sizeof(float)) {
					/* Argmax: pick the class with the highest score. */
					const float *sc   = (const float *)out.data;
					int          best = 0;
					float        bv   = sc[0];
					for (int k = 1; k < ASE_EVENT_COUNT; k++) {
						if (sc[k] > bv) {
							bv   = sc[k];
							best = k;
						}
					}
					return (struct ase_verdict){ (ase_event_t)best, bv };
				}
			}
		}
	}
	/* AI path unavailable or all checks failed: fall back to threshold rules. */
	return ase_classify_fallback(f);
}

int main(void)
{
	/*
	 * Frame accumulator and PCM staging buffer.
	 *
	 * ase_frame_state holds the float sample ring for one 32 ms frame.
	 * The `static` qualifier keeps it in BSS (not on the ISR stack),
	 * since the struct contains 512 floats = 2048 bytes.  pcm[] is the
	 * S16 staging buffer for one DMA block; 512 * 2 B = 1024 bytes on stack.
	 */
	static struct ase_frame_state frame;
	int16_t                       pcm[ASE_FRAME_N];

	/*
	 * Open the PDM microphone via <alp/audio.h>.
	 *
	 * Field-by-field explanation:
	 *
	 *   .peripheral_id    = E1M_PDM0 (= 0u)
	 *       The first PDM microphone instance on the E1M connector.  E1M_PDM0
	 *       is defined in <alp/e1m_pinout.h> as 0u; it maps to the PDM_MIC0
	 *       devicetree alias on every E1M-conformant board.
	 *
	 *   .sample_rate_hz   = 16000
	 *       16 kHz is the standard front-end for narrow-band audio classifiers.
	 *       Nyquist at 8 kHz covers the glass-break HF burst (6 kHz peak),
	 *       the alarm band (3150 Hz), and the scream voice band (800--2500 Hz).
	 *       Higher rates (24/48 kHz) would add no useful information for these
	 *       four classes and increase FFT cost proportionally.
	 *
	 *   .channels         = 1
	 *       Mono.  Most MEMS PDM microphones are single-element.  The acoustic
	 *       classifier does not use inter-channel phase information, so stereo
	 *       would double the buffer cost without benefiting accuracy.
	 *
	 *   .format           = ALP_AUDIO_FMT_S16_LE
	 *       16-bit signed little-endian PCM.  The loop converts S16 to float
	 *       in [-1, 1] by dividing by 32768.0f before feeding ase_frame_push().
	 *
	 *   .frames_per_block = ASE_FRAME_N (512)
	 *       DMA block = one analysis frame, so each alp_audio_in_read() call
	 *       delivers exactly the samples needed for one ase_feat_extract() pass.
	 *       512 frames at 16 kHz = 32 ms latency per classification.
	 *
	 * On native_sim (no DMIC device in the DT) the backend returns NULL.
	 * The loop detects mic == NULL and switches to synth_sample() automatically.
	 */
	alp_audio_in_t *mic = alp_audio_in_open(&(alp_audio_config_t){
	    .peripheral_id    = E1M_PDM0,
	    .sample_rate_hz   = 16000,
	    .channels         = 1,
	    .format           = ALP_AUDIO_FMT_S16_LE,
	    .frames_per_block = ASE_FRAME_N,
	});
	if (mic != NULL) {
		/* Start the DMA pipeline; frames flow into the driver ring buffer. */
		alp_audio_in_start(mic);
	} else {
		/* Expected on native_sim; the synthetic path substitutes gracefully. */
		LOG_WRN("PDM mic unavailable; using synthetic event audio");
	}

	/*
	 * Open the inference backend via <alp/inference.h>.
	 *
	 *   .backend    = ALP_INFERENCE_BACKEND_AUTO
	 *       Let the SDK pick the best NPU: Ethos-U85 on AEN E8, DX-M1 on V2N-M1,
	 *       or CPU TFLM when no NPU is available.  Using AUTO keeps the source
	 *       portable -- flipping board.yaml's som.sku retargets the backend.
	 *
	 *   .format     = ALP_INFERENCE_MODEL_TFLITE
	 *       The model file format.  The stub has no valid magic bytes, so the
	 *       parser rejects it and open() returns NULL, forcing the fallback path.
	 *
	 *   .model_data = s_model, .model_size = sizeof(s_model) (= 1)
	 *       The 1-byte stub satisfies the non-NULL pointer pre-condition but is
	 *       not a parseable TFLite flatbuffer.  classify() handles NULL inf.
	 *
	 *   .arena / .arena_bytes omitted (zero)
	 *       The backend uses its built-in default arena (board.yaml specifies
	 *       64 KiB via inference.default_arena_kib).
	 */
	alp_inference_t *inf = alp_inference_open(&(alp_inference_config_t){
	    .backend    = ALP_INFERENCE_BACKEND_AUTO,
	    .format     = ALP_INFERENCE_MODEL_TFLITE,
	    .model_data = s_model,
	    .model_size = sizeof(s_model),
	});

	/* CSV header: each column is described to help the reader parse logs.
	 *   t_s          -- wall time in seconds (frame index * 0.032 s/frame)
	 *   event        -- ASE_* label (AMBIENT, GLASS_BREAK, ALARM, SCREAM)
	 *   confidence   -- 0..1 from the AI model's softmax or the fallback rules
	 *   centroid_hz  -- spectral brightness; high -> HF content (glass break)
	 *   rms          -- AC signal level after DC removal; low -> ambient/silence */
	printk("# ASE,t_s,event,confidence,centroid_hz,rms\n");

	/*
	 * Main analysis loop -- N_FRAMES iterations, one CSV record each.
	 *
	 * Per iteration:
	 *   1. Reset the frame accumulator (count = 0).
	 *   2. Attempt a real PCM read (50 ms timeout); set have_pcm if successful.
	 *   3. Push ASE_FRAME_N samples: normalise S16 to float or use synth_sample().
	 *      When got < ASE_FRAME_N (short read), index into pcm with i % got to
	 *      cycle the partial block rather than access out-of-bounds memory.
	 *   4. ase_feat_extract() computes the 14-element feature vector.
	 *   5. classify() returns the event label and confidence.
	 *   6. printk() emits one CSV record.
	 */
	for (int w = 0; w < N_FRAMES; w++) {
		ase_frame_reset(&frame); /* clear the sample ring for this frame */
		size_t got = 0;
		/* Real PCM path: read one DMA block; fall through to synth on failure. */
		bool have_pcm = (mic != NULL &&
		                 alp_audio_in_read(mic, pcm, ASE_FRAME_N, &got, 50) == ALP_OK && got > 0);
		for (int i = 0; i < ASE_FRAME_N; i++) {
			/* Normalise S16 -> float [-1, 1], or generate synthetic sample. */
			float s = have_pcm ? ((float)pcm[i % (int)got] / 32768.0f) : synth_sample(w, i);
			ase_frame_push(&frame, s);
		}

		/* Extract the 14 acoustic features from the completed 32 ms frame. */
		struct ase_features f;
		ase_feat_extract(&frame, ASE_SR_HZ, &f);

		/* Classify: AI model if loaded, otherwise deterministic fallback. */
		struct ase_verdict v = classify(inf, &f);

		/* One CSV record per frame.  Columns: time, event, confidence,
		 * centroid_hz, rms.  Cast to double for portability with %f. */
		printk("ASE,%.2f,%s,%.2f,%.1f,%.2f\n",
		       (double)(w * 0.032f),  /* frame centre time in seconds */
		       ase_event_name(v.ev),  /* upper-case event label */
		       (double)v.confidence,  /* 0..1 classifier confidence */
		       (double)f.centroid_hz, /* spectral brightness, Hz */
		       (double)f.rms);        /* AC RMS signal level */
	}

	/* Teardown: release inference backend (NULL-safe). */
	if (inf != NULL) {
		alp_inference_close(inf);
	}
	/* Stop the DMA pipeline and release the mic handle (NULL-safe). */
	if (mic != NULL) {
		alp_audio_in_stop(mic);
		alp_audio_in_close(mic);
	}
	/* Sentinel matched by twister console harness to signal test completion. */
	printk("[ase] done\n");
	return 0;
}
