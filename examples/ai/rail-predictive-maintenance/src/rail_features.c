/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rail_features implementation -- see rail_features.h.
 */
#include "rail_features.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void rail_feat_state_reset(struct rail_feat_state *st)
{
	st->count = 0;
}

void rail_feat_window_push(struct rail_feat_state *st, float sample)
{
	if (st->count < RAIL_WINDOW_N) {
		st->samples[st->count++] = sample;
	}
}

bool rail_feat_window_full(const struct rail_feat_state *st)
{
	return st->count >= RAIL_WINDOW_N;
}

/* In-place iterative radix-2 FFT, N = RAIL_WINDOW_N, re/im length N. */
static void fft_radix2(float *re, float *im, int n)
{
	/* Bit-reversal permutation. */
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			float tr = re[i];
			re[i]    = re[j];
			re[j]    = tr;
			float ti = im[i];
			im[i]    = im[j];
			im[j]    = ti;
		}
	}
	for (int len = 2; len <= n; len <<= 1) {
		float ang = -2.0f * (float)M_PI / (float)len;
		float wlr = cosf(ang);
		float wli = sinf(ang);
		for (int i = 0; i < n; i += len) {
			float wr = 1.0f, wi = 0.0f;
			for (int k = 0; k < len / 2; k++) {
				int   a  = i + k;
				int   b  = i + k + len / 2;
				float tr = wr * re[b] - wi * im[b];
				float ti = wr * im[b] + wi * re[b];
				re[b]    = re[a] - tr;
				im[b]    = im[a] - ti;
				re[a] += tr;
				im[a] += ti;
				float nwr = wr * wlr - wi * wli;
				wi        = wr * wli + wi * wlr;
				wr        = nwr;
			}
		}
	}
}

void rail_feat_extract(const struct rail_feat_state *st,
                       float                         odr_hz,
                       float                         speed_mps,
                       struct rail_features         *out)
{
	const int n = (st->count < RAIL_WINDOW_N) ? st->count : RAIL_WINDOW_N;

	memset(out, 0, sizeof(*out));
	if (n <= 0) {
		return;
	}

	/* Mean (DC) removal. */
	float mean = 0.0f;
	for (int i = 0; i < n; i++) {
		mean += st->samples[i];
	}
	mean /= (float)n;

	/* Time-domain moments: RMS, peak, kurtosis. */
	float sum2 = 0.0f, peak = 0.0f, sum4 = 0.0f;
	for (int i = 0; i < n; i++) {
		float x  = st->samples[i] - mean;
		float ax = fabsf(x);
		sum2 += x * x;
		sum4 += x * x * x * x;
		if (ax > peak) {
			peak = ax;
		}
	}
	float var         = sum2 / (float)n;
	out->rms          = sqrtf(var);
	out->crest_factor = (out->rms > 1e-9f) ? (peak / out->rms) : 0.0f;
	out->kurtosis     = (var > 1e-12f) ? ((sum4 / (float)n) / (var * var)) : 0.0f;

	/* Spectrum over a fixed RAIL_WINDOW_N FFT (zero-pad a short window). */
	static float re[RAIL_WINDOW_N];
	static float im[RAIL_WINDOW_N];
	for (int i = 0; i < RAIL_WINDOW_N; i++) {
		re[i] = (i < n) ? (st->samples[i] - mean) : 0.0f;
		im[i] = 0.0f;
	}
	fft_radix2(re, im, RAIL_WINDOW_N);

	/* Magnitude-squared over the first half; find the dominant bin. */
	const int half = RAIL_WINDOW_N / 2;
	float     mag2[RAIL_WINDOW_N / 2];
	int       dom_bin = 1;
	float     dom_val = -1.0f;
	for (int k = 0; k < half; k++) {
		mag2[k] = re[k] * re[k] + im[k] * im[k];
		if (k >= 1 && mag2[k] > dom_val) { /* skip DC bin 0 */
			dom_val = mag2[k];
			dom_bin = k;
		}
	}
	out->dom_freq_hz = (float)dom_bin * odr_hz / (float)RAIL_WINDOW_N;
	out->rail_wavelength_m =
	    (out->dom_freq_hz > 1e-6f && speed_mps > 1e-6f) ? (speed_mps / out->dom_freq_hz) : 0.0f;

	/* Log-spaced band energies over bins 1..half-1, normalised to sum 1. */
	float total = 0.0f;
	for (int k = 1; k < half; k++) {
		total += mag2[k];
	}
	if (total < 1e-20f) {
		return; /* bands stay zero */
	}
	for (int k = 1; k < half; k++) {
		/* Map bin -> band by log position across [1, half). */
		float pos = logf((float)k) / logf((float)half);
		int   b   = (int)(pos * (float)RAIL_N_BANDS);
		if (b < 0) {
			b = 0;
		}
		if (b >= RAIL_N_BANDS) {
			b = RAIL_N_BANDS - 1;
		}
		out->band_energy[b] += mag2[k];
	}
	for (int b = 0; b < RAIL_N_BANDS; b++) {
		out->band_energy[b] /= total;
	}
}

size_t rail_feat_pack(const struct rail_features *f, float *vec, size_t cap)
{
	if (cap < (size_t)RAIL_FEATURE_DIM) {
		return 0;
	}
	size_t i = 0;
	vec[i++] = f->rms;
	vec[i++] = f->crest_factor;
	vec[i++] = f->kurtosis;
	for (int b = 0; b < RAIL_N_BANDS; b++) {
		vec[i++] = f->band_energy[b];
	}
	vec[i++] = f->dom_freq_hz;
	vec[i++] = f->rail_wavelength_m;
	return i; /* == RAIL_FEATURE_DIM */
}

struct rail_verdict rail_classify_fallback(const struct rail_features *f)
{
	struct rail_verdict v = { RAIL_HEALTHY, 0.0f };

	/* Narrowband ratio: fraction of spectral energy in the strongest band. */
	float band_max = 0.0f;
	for (int b = 0; b < RAIL_N_BANDS; b++) {
		if (f->band_energy[b] > band_max) {
			band_max = f->band_energy[b];
		}
	}

	if (f->crest_factor > 6.0f && f->kurtosis > 5.0f) {
		v.cls      = RAIL_JOINT_WELD;
		v.severity = fminf(1.0f, (f->crest_factor - 6.0f) / 6.0f);
	} else if (band_max > 0.5f) {
		v.cls      = RAIL_CORRUGATION;
		v.severity = fminf(1.0f, band_max);
	} else if (f->rms > 0.30f) {
		v.cls      = RAIL_ROUGH_RCF;
		v.severity = fminf(1.0f, (f->rms - 0.30f) / 0.30f);
	} else {
		v.cls      = RAIL_HEALTHY;
		v.severity = 0.0f;
	}
	return v;
}

const char *rail_class_name(rail_class_t c)
{
	switch (c) {
	case RAIL_HEALTHY:
		return "HEALTHY";
	case RAIL_CORRUGATION:
		return "CORRUGATION";
	case RAIL_JOINT_WELD:
		return "JOINT_WELD";
	case RAIL_ROUGH_RCF:
		return "ROUGH_RCF";
	default:
		return "UNKNOWN";
	}
}
