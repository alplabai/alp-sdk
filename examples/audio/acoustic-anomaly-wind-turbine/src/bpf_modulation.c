/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * bpf_modulation implementation -- see bpf_modulation.h.
 */
#include "bpf_modulation.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------------------------------------------------------------------
 * Envelope ring buffer helpers.
 *
 * The ring stores BPF_ENV_N per-frame energy summaries (e.g. total_rms from
 * the acoustic feature extractor).  At 62.5 fps this covers ~4 seconds --
 * long enough to resolve blade-pass periodicities down to ~0.25 Hz (15 rpm
 * with 1 blade), while staying within a modest static allocation.
 *
 * The ring is written with a power-of-two-safe modulo head pointer.  'count'
 * saturates at BPF_ENV_N; until it reaches BPF_ENV_N the buffer is not yet
 * full and the valid region is env[0..count-1] starting at index 0.
 * ---------------------------------------------------------------------------
 */
void bpf_env_reset(struct bpf_env_state *st)
{
	st->head  = 0;
	st->count = 0;
}

void bpf_env_push(struct bpf_env_state *st, float frame_energy)
{
	/* Write the new value at 'head' and advance head with wrap-around. */
	st->env[st->head] = frame_energy;
	st->head          = (uint16_t)((st->head + 1) % BPF_ENV_N);
	/* count saturates at BPF_ENV_N once the buffer is full. */
	if (st->count < BPF_ENV_N) {
		st->count++;
	}
}

/* ---------------------------------------------------------------------------
 * goertzel_power -- |X(f)|^2 at an arbitrary target frequency via Goertzel.
 *
 * WHY THIS STAYS HAND-ROLLED (not swapped for an FFT / CMSIS-DSP call):
 *   acoustic_features.c moved its full-spectrum FFT onto the portable
 *   <alp/dsp.h> chain (CMSIS-DSP arm_rfft_fast_f32 backend) because it
 *   needs every bin.  This file needs exactly ONE bin -- the live
 *   blade-pass harmonic k*bpf_hz, which shifts every call with the
 *   current RPM estimate and therefore never lands on a fixed FFT bin
 *   boundary.  An N-point FFT computes O(N log N) work to produce N
 *   bins and then discards all but one; the O(N) single-bin Goertzel
 *   recurrence below computes only the bin actually needed, at an
 *   arbitrary (non-bin-aligned) frequency.  CMSIS-DSP ships no Goertzel
 *   kernel to call instead, so this stays a portable loop -- it is the
 *   textbook-correct, cheaper tool for this job, not a gap.
 *
 * The Goertzel algorithm evaluates a single DFT bin without computing the full
 * FFT.  For a sequence x[0..n-1] and a target frequency target_hz at sample
 * rate fs, the normalised digital frequency is:
 *
 *     omega = 2*pi * target_hz / fs
 *     coeff = 2 * cos(omega)
 *
 * The algorithm runs the second-order IIR recurrence:
 *
 *     s0[i] = x[i] + coeff * s1[i-1] - s2[i-2]   (s[-1] = s[-2] = 0)
 *
 * After n samples the squared magnitude is:
 *
 *     |X|^2 = s1^2 + s2^2 - coeff * s1 * s2
 *
 * where s1 and s2 are the last two state values.  This form avoids computing
 * sin/cos a second time for the final output; it holds exactly because:
 *
 *     |X|^2 = |s1 - W^(-1) * s2|^2,  W = exp(j*omega)
 *
 * expanded with W^(-1) = cos(omega) - j*sin(omega) and sin^2 + cos^2 = 1.
 *
 * KEY PROPERTY: the target frequency is passed in as the *current* bpf_hz,
 * which is re-computed every extraction call from the live RPM estimate.
 * This makes the feature RPM-invariant: we always evaluate at the blade-pass
 * harmonic of the current shaft speed, not at a fixed Hz bin.
 * ---------------------------------------------------------------------------
 */
static float goertzel_power(const float *x, int n, float target_hz, float fs)
{
	if (target_hz <= 0.0f || fs <= 0.0f) {
		return 0.0f;
	}
	float w     = 2.0f * (float)M_PI * target_hz / fs;
	float coeff = 2.0f * cosf(w);
	/* Second-order IIR state; s1 is the most recent, s2 is one step older. */
	float s1 = 0.0f, s2 = 0.0f;
	for (int i = 0; i < n; i++) {
		float s0 = x[i] + coeff * s1 - s2;
		s2       = s1;
		s1       = s0;
	}
	/* |X|^2 = s1^2 + s2^2 - coeff*s1*s2  (real-only Goertzel output power). */
	return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

/* ---------------------------------------------------------------------------
 * bpf_modulation_extract -- extract blade-order energies and modulation depth.
 *
 * The function operates on the seconds-long envelope ring (bpf_env_state) and
 * extracts two classes of feature:
 *
 *   1. Blade-order energies (BPF_N_HARMONICS = 4 values):
 *      The Goertzel algorithm evaluates the envelope's AC energy at each
 *      harmonic k * bpf_hz (k = 1..4).  A healthy blade produces a clean 1x
 *      BPF signal; an aerodynamic fault or pitch imbalance raises higher
 *      harmonics disproportionately.
 *
 *   2. Modulation depth:
 *      modulation_depth = (max - min) / (max + min)
 *      This is the traditional AM depth metric applied to the energy envelope.
 *      It captures slow amplitude modulation independent of BPF frequency and
 *      is complementary to the Goertzel features.
 *
 * NORMALISATION of blade-order energies:
 *   By Parseval's theorem, sum_k |X_k|^2 = n * sum_i x[i]^2.  The raw
 *   Goertzel output |X(k*bpf)|^2 therefore scales with both n and the AC
 *   envelope power.  Dividing by (n * total) -- where total = sum x[i]^2 --
 *   maps each output to a bounded fraction in roughly [0, 1] that is
 *   independent of window length and signal amplitude, making it comparable
 *   across frames.
 *
 * RING LINEARISATION:
 *   When the ring is full (count == BPF_ENV_N) the oldest sample is at
 *   index 'head' (the next write position); the newest is one position before
 *   'head' in the wrapped sense.  The linearisation loop reads
 *   env[(start + i) % BPF_ENV_N] for i = 0..n-1, writing oldest→newest into
 *   the temporary buf[].  When the ring is not yet full (count < BPF_ENV_N)
 *   the data runs from index 0 without wrapping, so start = 0.
 * ---------------------------------------------------------------------------
 */
void bpf_modulation_extract(const struct bpf_env_state *st,
                            float                       bpf_hz,
                            float                       frame_rate_hz,
                            struct bpf_modulation      *out)
{
	memset(out, 0, sizeof(*out));
	const int n = (st->count < BPF_ENV_N) ? st->count : BPF_ENV_N;
	if (n < 8) {
		return;
	}

	/* Linearise the ring (oldest..newest) into buf[], mean-removing in place.
	 * When the ring is full, start = head (oldest slot); otherwise start = 0. */
	static float buf[BPF_ENV_N];
	float        mean  = 0.0f;
	int          start = (st->count < BPF_ENV_N) ? 0 : st->head;
	for (int i = 0; i < n; i++) {
		buf[i] = st->env[(start + i) % BPF_ENV_N];
		mean += buf[i];
	}
	mean /= (float)n;

	/* Scan for peak and valley before subtracting the mean.
	 * emin/emax are used for modulation_depth on the original (non-mean-removed)
	 * envelope; buf[] is then mean-removed for the Goertzel passes. */
	float emin = buf[0], emax = buf[0];
	for (int i = 0; i < n; i++) {
		if (buf[i] < emin) {
			emin = buf[i];
		}
		if (buf[i] > emax) {
			emax = buf[i];
		}
		buf[i] -= mean;
	}

	/* Total AC (mean-removed) envelope power for Goertzel normalisation.
	 * After mean removal, sum x[i]^2 is the AC signal energy. */
	float total = 0.0f;
	for (int i = 0; i < n; i++) {
		total += buf[i] * buf[i];
	}
	/* |X(k)|^2 sums to n*Sigma|x|^2 (Parseval); divide by (n*total) so each
	 * blade-order energy is a bounded fraction of the envelope AC energy. */
	float norm = (total > 1e-12f) ? (1.0f / ((float)n * total)) : 0.0f;

	/* Evaluate Goertzel at each BPF harmonic k*bpf_hz (k=1..BPF_N_HARMONICS).
	 * Passing the live bpf_hz (derived from current RPM) keeps each feature
	 * pinned to the blade-pass harmonic regardless of shaft speed variation. */
	for (int k = 1; k <= BPF_N_HARMONICS; k++) {
		float p                        = goertzel_power(buf, n, (float)k * bpf_hz, frame_rate_hz);
		out->blade_order_energy[k - 1] = p * norm;
	}
	/* Modulation depth: (peak - trough) / (peak + trough).
	 * Guard against a near-zero denominator (constant / silent envelope). */
	out->modulation_depth = (emax + emin > 1e-9f) ? ((emax - emin) / (emax + emin)) : 0.0f;
}

/* Pack blade-order energies and modulation_depth into a flat float vector for
 * the downstream model.  Layout: blade_order_energy[0..3], modulation_depth.
 * Returns the number of elements written (== BPF_FEATURE_DIM) or 0 if cap
 * is too small. */
size_t bpf_modulation_pack(const struct bpf_modulation *m, float *vec, size_t cap)
{
	if (cap < (size_t)BPF_FEATURE_DIM) {
		return 0;
	}
	size_t i = 0;
	for (int k = 0; k < BPF_N_HARMONICS; k++) {
		vec[i++] = m->blade_order_energy[k];
	}
	vec[i++] = m->modulation_depth;
	return i; /* == BPF_FEATURE_DIM */
}
