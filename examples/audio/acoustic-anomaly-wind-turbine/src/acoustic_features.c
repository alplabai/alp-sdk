/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * acoustic_features implementation -- see acoustic_features.h.
 */
#include "acoustic_features.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void aco_frame_reset(struct aco_frame_state *st)
{
	st->count = 0;
}

void aco_frame_push(struct aco_frame_state *st, float sample)
{
	if (st->count < ACO_FRAME_N) {
		st->samples[st->count++] = sample;
	}
}

bool aco_frame_full(const struct aco_frame_state *st)
{
	return st->count >= ACO_FRAME_N;
}

/* In-place iterative radix-2 FFT (same proven routine as the rail example). */
static void fft_radix2(float *re, float *im, int n)
{
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

void aco_feat_extract(const struct aco_frame_state *st, float sr_hz, struct aco_features *out)
{
	const int n = (st->count < ACO_FRAME_N) ? st->count : ACO_FRAME_N;

	memset(out, 0, sizeof(*out));
	if (n <= 0) {
		return;
	}

	/* DC removal + time-domain moments. */
	float mean = 0.0f;
	for (int i = 0; i < n; i++) {
		mean += st->samples[i];
	}
	mean /= (float)n;

	float sum2 = 0.0f, sum4 = 0.0f;
	for (int i = 0; i < n; i++) {
		float x = st->samples[i] - mean;
		sum2 += x * x;
		sum4 += x * x * x * x;
	}
	float var      = sum2 / (float)n;
	out->total_rms = sqrtf(var);
	out->kurtosis  = (var > 1e-12f) ? ((sum4 / (float)n) / (var * var)) : 0.0f;

	/* Spectrum. */
	static float re[ACO_FRAME_N];
	static float im[ACO_FRAME_N];
	for (int i = 0; i < ACO_FRAME_N; i++) {
		re[i] = (i < n) ? (st->samples[i] - mean) : 0.0f;
		im[i] = 0.0f;
	}
	fft_radix2(re, im, ACO_FRAME_N);

	const int half = ACO_FRAME_N / 2;
	float     mag[ACO_FRAME_N / 2];
	float     mag_total = 0.0f, centroid_num = 0.0f;
	float     log_sum = 0.0f, lin_sum = 0.0f;
	int       active = 0;
	for (int k = 1; k < half; k++) {
		float m = sqrtf(re[k] * re[k] + im[k] * im[k]);
		mag[k]  = m;
		float f = (float)k * sr_hz / (float)ACO_FRAME_N;
		mag_total += m;
		centroid_num += f * m;
		/* Spectral flatness on a small-epsilon floored magnitude. */
		float me = m + 1e-9f;
		log_sum += logf(me);
		lin_sum += me;
		active++;
	}
	out->spectral_centroid_hz = (mag_total > 1e-12f) ? (centroid_num / mag_total) : 0.0f;
	if (active > 0 && lin_sum > 1e-12f) {
		float geo              = expf(log_sum / (float)active);
		float arith            = lin_sum / (float)active;
		out->spectral_flatness = geo / arith;
	} else {
		out->spectral_flatness = 0.0f;
	}

	/* Log-spaced band energies over bins 1..half-1, normalised to sum 1. */
	float mag2_total = 0.0f;
	for (int k = 1; k < half; k++) {
		mag2_total += mag[k] * mag[k];
	}
	if (mag2_total < 1e-20f) {
		return;
	}
	for (int k = 1; k < half; k++) {
		float pos = logf((float)k) / logf((float)half);
		int   b   = (int)(pos * (float)ACO_N_BANDS);
		if (b < 0) {
			b = 0;
		}
		if (b >= ACO_N_BANDS) {
			b = ACO_N_BANDS - 1;
		}
		out->band_energy[b] += mag[k] * mag[k];
	}
	for (int b = 0; b < ACO_N_BANDS; b++) {
		out->band_energy[b] /= mag2_total;
	}
}

size_t aco_feat_pack(const struct aco_features *f, float *vec, size_t cap)
{
	if (cap < (size_t)ACO_FEATURE_DIM) {
		return 0;
	}
	size_t i = 0;
	for (int b = 0; b < ACO_N_BANDS; b++) {
		vec[i++] = f->band_energy[b];
	}
	vec[i++] = f->spectral_flatness;
	vec[i++] = f->spectral_centroid_hz;
	vec[i++] = f->kurtosis;
	vec[i++] = f->total_rms;
	return i; /* == ACO_FEATURE_DIM */
}

float aco_anomaly_fallback(const float *vec, size_t n, const struct aco_baseline *base)
{
	float d2 = 0.0f;
	for (size_t i = 0; i < n; i++) {
		float dx = vec[i] - base->mean[i];
		d2 += dx * dx * base->inv_var[i];
	}
	/* Squash to [0,1): grows with normalised distance, saturates smoothly. */
	float score = 1.0f - expf(-d2 / (float)ACO_FEATURE_DIM);
	if (score < 0.0f) {
		score = 0.0f;
	}
	if (score > 1.0f) {
		score = 1.0f;
	}
	return score;
}
