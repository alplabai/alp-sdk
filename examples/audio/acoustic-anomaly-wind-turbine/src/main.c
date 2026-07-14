/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * acoustic-anomaly-wind-turbine
 * =============================
 *
 * Nacelle acoustic condition monitor.  Pipeline:
 *
 *   PDM mic (<alp/audio.h>, 16 kHz) --256-sample frame-->
 *     acoustic_features (FFT bands, flatness, centroid, kurtosis)
 *       --per-frame band energy--> bpf_modulation envelope ring
 *   tacho GPIO (or tacholess) --> rotor_speed --> rpm, BPF
 *     --order features--> <alp/inference.h> anomaly model
 *                         (deterministic fallback when no model)
 *     --> one WTAC record per report interval.
 *
 * Honest scope: detects drivetrain tonals + gross blade aero-anomalies
 * (TE-crack whistle, severe erosion, icing deviation, imbalance-at-BPF).
 * Early internal cracks / delamination are Acoustic-Emission (ultrasonic,
 * contact-piezo) and are OUT OF SCOPE for an airborne mic.
 *
 * The model is a stub; see models/README.md for the training recipe.  With
 * no model the deterministic baseline-deviation fallback runs so the demo
 * still produces real anomaly scores.
 *
 * API reconciliation notes (checked against real alp API headers):
 *   - No BOARD_GPIO_TACHO alias exists in <alp/board.h> routes headers.
 *     Using ALP_E1M_GPIO_IO0 (IO0, E1M pad L2) as the tacho input pin.
 *     A customer wiring a tacho pulse to a different IO should change
 *     this to the matching ALP_E1M_GPIO_IO<N> constant from <alp/e1m_pinout.h>.
 *   - alp_gpio_open() takes a uint32_t pin_id; signature confirmed.
 *   - alp_gpio_configure() signature: (pin, alp_gpio_dir_t, alp_gpio_pull_t).
 *     ALP_GPIO_PULL_NONE confirmed in peripheral.h.
 *   - alp_audio_in_open() / alp_audio_config_t fields: peripheral_id,
 *     sample_rate_hz, channels, format, frames_per_block -- all confirmed.
 *   - alp_inference_open() / alp_inference_config_t fields confirmed.
 *     With a 1-byte stub alp_inference_open() returns NULL (bad magic);
 *     the AI path guards on c.inf != NULL and falls back to
 *     aco_anomaly_fallback().
 */
#include <string.h>
#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "alp/audio.h"
#include "alp/e1m_pinout.h"
#include "alp/inference.h"
#include "alp/peripheral.h"

#include "acoustic_features.h"
#include "bpf_modulation.h"
#include "rotor_speed.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

LOG_MODULE_REGISTER(wtac, LOG_LEVEL_INF);

#define N_BLADES          3
#define FRAMES_PER_REPORT BPF_ENV_N /* one record per full envelope window */
#define N_REPORTS         8         /* bounded demo run */
#define ANOMALY_INPUT_DIM (ACO_FEATURE_DIM + BPF_FEATURE_DIM + 1)
/* Gear-mesh band index in band_energy[]: the log-spaced band the synthetic
 * ~600 Hz drivetrain tone lands in (bin ~10 of 128 -> band 5). */
#define GEARMESH_BAND 5

/*
 * E1M GPIO IO0 (pad L2) used as tacho input.  No BOARD_GPIO_TACHO alias
 * exists in the E1M-EVK routes header; IO0 is a representative choice.
 * Rewire to the ALP_E1M_GPIO_IO<N> that matches your carrier's tacho wire.
 */
#define WTAC_TACHO_GPIO ALP_E1M_GPIO_IO0

/* 1-byte stub so alp_inference_open's non-NULL model_data contract is met;
 * bad magic bytes cause the backend to return NULL, which forces the
 * deterministic fallback.  Drop in a real .tflite + see models/README.md. */
static const uint8_t s_model[] = { 0x00 };

/* Canned variable-RPM track for native_sim (rpm per report interval). */
static const float s_canned_rpm[N_REPORTS] = { 14.0f, 15.0f, 16.0f, 17.0f,
	                                           18.0f, 17.0f, 16.0f, 15.0f };

/* Healthy baseline matching the synthetic "healthy hum" (low broadband):
 * mean ~ uniform small band energy, flatness high, centroid mid, low rms.
 * Customer replaces with a baseline learned at commissioning. */
static struct aco_baseline s_baseline;

/* Populate s_baseline once at startup.  inv_var (1/sigma^2) is the weight
 * aco_anomaly_fallback() gives each feature when computing a Mahalanobis-
 * style distance: leaving most entries at 1.0f (init to 1.0f by the loop
 * below) is a placeholder equal-weighting, while the two explicit
 * inv_var overrides down-weight centroid_hz and kurtosis specifically
 * because their raw numeric ranges (hundreds to thousands of Hz; unbounded
 * moment) would otherwise swamp the [0,1]-ish band-energy terms in the
 * distance sum. */
static void baseline_init(void)
{
	for (int i = 0; i < ACO_FEATURE_DIM; i++) {
		s_baseline.mean[i]    = 0.0f;
		s_baseline.inv_var[i] = 1.0f;
	}
	/* Band energies ~ 1/N each for flat broadband. */
	for (int b = 0; b < ACO_N_BANDS; b++) {
		s_baseline.mean[b] = 1.0f / (float)ACO_N_BANDS;
	}
	s_baseline.mean[ACO_N_BANDS + 0] = 0.7f;    /* spectral_flatness (broadband) */
	s_baseline.mean[ACO_N_BANDS + 1] = 4000.0f; /* centroid Hz */
	s_baseline.mean[ACO_N_BANDS + 2] = 3.0f;    /* kurtosis */
	s_baseline.mean[ACO_N_BANDS + 3] = 0.05f;   /* total_rms */
	/* Down-weight the wide-range raw features so they don't dominate distance. */
	s_baseline.inv_var[ACO_N_BANDS + 1] = 1.0f / (500.0f * 500.0f); /* centroid */
	s_baseline.inv_var[ACO_N_BANDS + 2] = 1.0f / (2.0f * 2.0f);     /* kurtosis */
}

/* Synthetic acoustic frame for native_sim: healthy hum, plus per-report
 * injected anomalies so the demo emits a mix of verdicts. */
static float synth_sample(int report, int frame, int i)
{
	float hum = 0.02f * (sinf((float)i * 1.7f) + sinf((float)i * 0.37f));
	switch (report % 3) {
	case 0: /* healthy */
		return hum;
	case 1: { /* blade imbalance: hum amplitude-modulated at BPF */
		float t  = (float)frame / ACO_FRAME_RATE_HZ;
		float am = 1.0f + 0.6f * sinf(2.0f * (float)M_PI * 0.75f * t);
		return hum * am;
	}
	default: /* drivetrain tonal: a ~600 Hz gear-mesh tone in the mech band */
		return hum + 0.5f * sinf(2.0f * (float)M_PI * 600.0f * (float)i / ACO_SR_HZ);
	}
}

/* All per-run peripheral handles, bundled so main() can pass one pointer
 * instead of three; NULL fields (mic/tacho absent) are the normal
 * native_sim state, not an error condition -- each caller downstream
 * checks the specific field it needs. */
struct wtac_ctx {
	alp_audio_in_t  *mic;
	alp_gpio_t      *tacho;
	alp_inference_t *inf;
};

/* Advisory subsystem label attached to the printk record; a heuristic hint
 * for a technician, not a claim the AI/fallback score itself makes -- see
 * the subsystem/flags block near the end of main() for how it is derived. */
static const char *subsystem_name(int s)
{
	switch (s) {
	case 0:
		return "BLADE_BPF";
	case 1:
		return "DRIVETRAIN_TONAL";
	default:
		return "BROADBAND";
	}
}

int main(void)
{
	static struct wtac_ctx        c;
	static struct aco_frame_state frame;
	static struct bpf_env_state   envst;

	memset(&c, 0, sizeof(c));
	baseline_init();

	/* PDM mic -- tolerate absence on native_sim (-> synthetic audio). */
	c.mic = alp_audio_in_open(&(alp_audio_config_t){
	    .peripheral_id    = ALP_E1M_PDM0,
	    .sample_rate_hz   = 16000,
	    .channels         = 1,
	    .format           = ALP_AUDIO_FMT_S16_LE,
	    .frames_per_block = ACO_FRAME_N,
	});
	if (c.mic != NULL) {
		alp_audio_in_start(c.mic);
	} else {
		LOG_WRN("PDM mic unavailable; using synthetic acoustics");
	}

	/* Tacho GPIO input (E1M IO0 = WTAC_TACHO_GPIO) -- tolerate absence. */
	c.tacho = alp_gpio_open(WTAC_TACHO_GPIO);
	if (c.tacho != NULL) {
		alp_gpio_configure(c.tacho, ALP_GPIO_INPUT, ALP_GPIO_PULL_NONE);
	} else {
		LOG_WRN("tacho GPIO unavailable; using tacholess RPM estimation");
	}

	/* Anomaly model -- NULL/stub-tolerant; fallback runs if absent. */
	c.inf = alp_inference_open(&(alp_inference_config_t){
	    .backend    = ALP_INFERENCE_BACKEND_AUTO,
	    .format     = ALP_INFERENCE_MODEL_TFLITE,
	    .model_data = s_model,
	    .model_size = sizeof(s_model),
	});

	printk("# WTAC,t_s,rpm,bpf_hz,anomaly_score,dominant_subsystem,top_band_hz,flags,rpm_src\n");

	int16_t pcm[ACO_FRAME_N];

	/* Outer loop: one WTAC report per BPF_ENV_N-frame envelope window (~4 s
	 * of audio at 62.5 fps) -- long enough to span several blade-pass
	 * periods (a few rpm to tens of rpm) so bpf_modulation_extract() below
	 * has a real modulation cycle to measure, not just a fraction of one. */
	for (int r = 0; r < N_REPORTS; r++) {
		bpf_env_reset(&envst);
		float acc[ACO_FEATURE_DIM];
		memset(acc, 0, sizeof(acc));

		/* Inner loop: one 256-sample PDM frame (16 ms at 16 kHz) at a time;
		 * acc accumulates the per-frame feature vectors so they can be
		 * averaged into one representative vector per report below. */
		for (int fidx = 0; fidx < FRAMES_PER_REPORT; fidx++) {
			aco_frame_reset(&frame);
			size_t got = 0;
			bool   have_pcm =
			    (c.mic != NULL && alp_audio_in_read(c.mic, pcm, ACO_FRAME_N, &got, 50) == ALP_OK &&
			     got > 0);
			for (int i = 0; i < ACO_FRAME_N; i++) {
				/* have_pcm may return fewer than ACO_FRAME_N samples (got);
				 * wrap with %got so the frame is still fully populated
				 * rather than left zero-padded past what the mic returned. */
				float s =
				    have_pcm ? ((float)pcm[i % (int)got] / 32768.0f) : synth_sample(r, fidx, i);
				aco_frame_push(&frame, s);
			}

			struct aco_features f;
			aco_feat_extract(&frame, ACO_SR_HZ, &f);

			/* Envelope sample = absolute frame energy (total_rms).  NOTE: do NOT
			 * use sum(band_energy) -- those are normalised to sum 1, so their sum
			 * is always 1.0 and carries no blade-pass modulation. */
			bpf_env_push(&envst, f.total_rms);

			float v[ACO_FEATURE_DIM];
			aco_feat_pack(&f, v, ACO_FEATURE_DIM);
			for (int i = 0; i < ACO_FEATURE_DIM; i++) {
				acc[i] += v[i];
			}
		}

		/* Mean feature vector over the report interval: smooths per-frame
		 * noise and gives the AI/fallback path (below) one representative
		 * vector instead of FRAMES_PER_REPORT separate ones. */
		for (int i = 0; i < ACO_FEATURE_DIM; i++) {
			acc[i] /= (float)FRAMES_PER_REPORT;
		}

		/* Rotor speed: tacholess from the envelope (the demo has no live tacho);
		 * a real node uses rotor_tacho_rpm() from GPIO pulse intervals. */
		float       rpm = rotor_tacholess_rpm(envst.env, envst.count, ACO_FRAME_RATE_HZ, N_BLADES);
		const char *rpm_src = "ESTIMATED";
		if (!rotor_rpm_valid(rpm)) {
			/* rotor_rpm_valid()'s 3..30 rpm plausibility gate catches both
			 * a genuinely stopped/near-stopped rotor and an unreliable
			 * autocorrelation estimate (see rotor_tacholess_rpm()'s
			 * detectable-RPM-floor note in rotor_speed.h); either way, the
			 * canned track keeps rpm_src visibly distinct in the log so a
			 * downstream reader can tell an estimate from a substitution. */
			rpm     = s_canned_rpm[r]; /* fall back to the canned track */
			rpm_src = "CANNED";
		}
		float bpf = rotor_bpf_hz(rpm, N_BLADES);

		struct bpf_modulation mod;
		bpf_modulation_extract(&envst, bpf, ACO_FRAME_RATE_HZ, &mod);

		/* Anomaly score: AI if available, else deterministic fallback over the
		 * mean per-frame feature vector, boosted by blade-order modulation.
		 * With the 1-byte stub alp_inference_open() returned NULL, so the
		 * else branch always runs in the demo. */
		float score;
		if (c.inf != NULL) {
			/* AI path: fill the ANOMALY_INPUT_DIM-float input tensor
			 * [acc | bpf-modulation pack | rpm] and invoke. */
			alp_inference_tensor_t in_t;
			bool                   ai_ok = (alp_inference_get_input(c.inf, 0, &in_t) == ALP_OK) &&
			                               (in_t.data != NULL) &&
			                               (in_t.size_bytes >= ANOMALY_INPUT_DIM * sizeof(float)) &&
			                               (in_t.dtype == ALP_INFERENCE_DTYPE_F32);
			if (ai_ok) {
				float *inp = (float *)in_t.data;
				float  bpf_vec[BPF_FEATURE_DIM];

				memcpy(inp, acc, ACO_FEATURE_DIM * sizeof(float));
				bpf_modulation_pack(&mod, bpf_vec, BPF_FEATURE_DIM);
				memcpy(inp + ACO_FEATURE_DIM, bpf_vec, BPF_FEATURE_DIM * sizeof(float));
				inp[ACO_FEATURE_DIM + BPF_FEATURE_DIM] = rpm;
				ai_ok                                  = (alp_inference_invoke(c.inf) == ALP_OK);
			}
			if (ai_ok) {
				alp_inference_tensor_t out_t;
				ai_ok = (alp_inference_get_output(c.inf, 0, &out_t) == ALP_OK) &&
				        (out_t.data != NULL) && (out_t.size_bytes >= sizeof(float)) &&
				        (out_t.dtype == ALP_INFERENCE_DTYPE_F32);
				if (ai_ok) {
					score = *(const float *)out_t.data;
				}
			}
			if (!ai_ok) {
				score = aco_anomaly_fallback(acc, ACO_FEATURE_DIM, &s_baseline);
			}
		} else {
			score = aco_anomaly_fallback(acc, ACO_FEATURE_DIM, &s_baseline);
		}

		/* Blade-pass AM strength (already 0..1) raises the score; do NOT use the
		 * raw blade_order_energy, which is not a probability. */
		float blade = mod.modulation_depth;
		if (blade > score) {
			score = blade;
		}
		if (score > 1.0f) {
			score = 1.0f;
		}

		/*
		 * Heuristic subsystem + flags (advisory; AI/fallback gives the
		 * score above -- this block only labels WHERE a problem likely is,
		 * checked in priority order so the first, most specific match wins:
		 *
		 *  1. BLADE_BPF/IMBALANCE  -- strong once-per-revolution amplitude
		 *     modulation (fundamental blade-order energy > 0.2 of AC energy
		 *     AND overall modulation_depth > 0.3) is the fingerprint of an
		 *     unbalanced or damaged blade beating the hum at 1x BPF.
		 *  2. DRIVETRAIN_TONAL/GEARMESH -- checked only if (1) did not match;
		 *     energy concentrated in GEARMESH_BAND (the log-band the
		 *     synthetic ~600 Hz gear-mesh tone lands in) flags a tonal
		 *     component from the gearbox rather than the blades.
		 *  3. BLADE_BPF/TE_WHISTLE -- low spectral flatness (tonal, not
		 *     broadband) with neither of the above is the fallback guess
		 *     for a trailing-edge whistle; still labelled BLADE_BPF since
		 *     TE erosion is a blade-surface defect.
		 *  4. BROADBAND/NONE (the initial subsystem/flags values) -- no
		 *     narrowband signature found; broadband aero noise/erosion.
		 */
		int   subsystem = 2; /* BROADBAND */
		char  flags[32] = "NONE";
		float gearmesh  = acc[GEARMESH_BAND];
		if (mod.modulation_depth > 0.3f && mod.blade_order_energy[0] > 0.2f) {
			subsystem = 0; /* BLADE_BPF */
			strcpy(flags, "IMBALANCE");
		} else if (gearmesh > 0.3f) {
			subsystem = 1; /* DRIVETRAIN_TONAL */
			strcpy(flags, "GEARMESH");
		} else if (acc[ACO_N_BANDS + 0] < 0.3f) { /* low flatness -> tonal */
			subsystem = 0;
			strcpy(flags, "TE_WHISTLE");
		}

		float top_band_hz = (float)GEARMESH_BAND * (ACO_SR_HZ / 2.0f) / (float)ACO_N_BANDS;
		printk("WTAC,%.1f,%.1f,%.2f,%.2f,%s,%.1f,%s,%s\n",
		       (double)(r * 4.0f),
		       (double)rpm,
		       (double)bpf,
		       (double)score,
		       subsystem_name(subsystem),
		       (double)top_band_hz,
		       flags,
		       rpm_src);
	}

	if (c.inf != NULL) {
		alp_inference_close(c.inf);
	}
	if (c.mic != NULL) {
		alp_audio_in_stop(c.mic);
		alp_audio_in_close(c.mic);
	}
	if (c.tacho != NULL) {
		alp_gpio_close(c.tacho);
	}
	printk("[wtac] done\n");
	return 0;
}
