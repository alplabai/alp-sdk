/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * current_features implementation -- see current_features.h.
 */
#include "current_features.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void curr_window_reset(struct curr_window_state *st)
{
	st->count = 0;
}

void curr_window_push(struct curr_window_state *st, struct curr_sample s)
{
	if (st->count < CURR_WINDOW_N) {
		st->s[st->count++] = s;
	}
}

bool curr_window_full(const struct curr_window_state *st)
{
	return st->count >= CURR_WINDOW_N;
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

void curr_feat_extract(const struct curr_window_state *st, float sr_hz, struct curr_features *out)
{
	const int n = (st->count < CURR_WINDOW_N) ? st->count : CURR_WINDOW_N;

	memset(out, 0, sizeof(*out));
	if (n <= 0) {
		return;
	}

	float sum_i = 0.0f, sum_p = 0.0f, sum_v = 0.0f;
	for (int i = 0; i < n; i++) {
		sum_i += st->s[i].current_a;
		sum_p += st->s[i].power_w;
		sum_v += st->s[i].bus_v;
	}
	out->mean_current_a = sum_i / (float)n;
	out->mean_power_w   = sum_p / (float)n;
	out->mean_bus_v     = sum_v / (float)n;

	float sum2 = 0.0f, peak = 0.0f;
	for (int i = 0; i < n; i++) {
		float ac = st->s[i].current_a - out->mean_current_a;
		sum2 += ac * ac;
		if (fabsf(ac) > peak) {
			peak = fabsf(ac);
		}
	}
	out->rms_ac_a = sqrtf(sum2 / (float)n);
	out->crest    = (out->rms_ac_a > 1e-6f) ? (peak / out->rms_ac_a) : 0.0f;

	/* Slope: last-quarter mean minus first-quarter mean (inrush -> negative). */
	int q = n / 4;
	if (q < 1) {
		q = 1;
	}
	float first = 0.0f, last = 0.0f;
	for (int i = 0; i < q; i++) {
		first += st->s[i].current_a;
		last += st->s[n - 1 - i].current_a;
	}
	out->slope_a = (last - first) / (float)q;

	/* Dominant ripple frequency via FFT of the AC current. */
	static float re[CURR_WINDOW_N];
	static float im[CURR_WINDOW_N];
	for (int i = 0; i < CURR_WINDOW_N; i++) {
		re[i] = (i < n) ? (st->s[i].current_a - out->mean_current_a) : 0.0f;
		im[i] = 0.0f;
	}
	fft_radix2(re, im, CURR_WINDOW_N);
	const int half    = CURR_WINDOW_N / 2;
	int       dom_bin = 1;
	float     dom_val = -1.0f;
	for (int k = 1; k < half; k++) {
		float m2 = re[k] * re[k] + im[k] * im[k];
		if (m2 > dom_val) {
			dom_val = m2;
			dom_bin = k;
		}
	}
	out->ripple_freq_hz = (float)dom_bin * sr_hz / (float)CURR_WINDOW_N;
}

size_t curr_feat_pack(const struct curr_features *f, float *vec, size_t cap)
{
	if (cap < (size_t)CURR_FEATURE_DIM) {
		return 0;
	}
	size_t i = 0;
	vec[i++] = f->mean_current_a;
	vec[i++] = f->rms_ac_a;
	vec[i++] = f->crest;
	vec[i++] = f->slope_a;
	vec[i++] = f->mean_power_w;
	vec[i++] = f->mean_bus_v;
	vec[i++] = f->ripple_freq_hz;
	return i; /* == CURR_FEATURE_DIM */
}

curr_state_t current_classify(const struct curr_features *f, const struct curr_config *cfg)
{
	if (f->mean_current_a < cfg->off_a) {
		return CURR_OFF;
	}
	if (f->slope_a < -cfg->inrush_slope_a) {
		return CURR_INRUSH; /* current decaying from a startup spike */
	}
	if (f->mean_current_a > cfg->overload_a) {
		return (f->rms_ac_a < cfg->ripple_min_a) ? CURR_STALL : CURR_OVERLOAD;
	}
	return CURR_NORMAL;
}

const char *curr_state_name(curr_state_t s)
{
	switch (s) {
	case CURR_OFF:
		return "OFF";
	case CURR_NORMAL:
		return "NORMAL";
	case CURR_INRUSH:
		return "INRUSH";
	case CURR_OVERLOAD:
		return "OVERLOAD";
	case CURR_STALL:
		return "STALL";
	default:
		return "UNKNOWN";
	}
}

float curr_anomaly_fallback(const struct curr_features *f, const struct curr_config *cfg)
{
	float score = 0.0f;
	if (f->mean_current_a > cfg->overload_a && cfg->overload_a > 1e-6f) {
		score = (f->mean_current_a - cfg->overload_a) / cfg->overload_a;
	}
	/* High current with no ripple = stalled rotor: a strong anomaly. */
	if (f->mean_current_a > cfg->overload_a && f->rms_ac_a < cfg->ripple_min_a) {
		score = fmaxf(score, 0.9f);
	}
	if (score < 0.0f) {
		score = 0.0f;
	}
	if (score > 1.0f) {
		score = 1.0f;
	}
	return score;
}
