/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * motion_features implementation -- see motion_features.h.
 */
#include "motion_features.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void mot_window_reset(struct mot_window_state *st)
{
	st->count = 0;
}

void mot_window_push(struct mot_window_state *st, struct mot_sample s)
{
	if (st->count < MOT_WINDOW_N) {
		st->s[st->count++] = s;
	}
}

bool mot_window_full(const struct mot_window_state *st)
{
	return st->count >= MOT_WINDOW_N;
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

void mot_feat_extract(const struct mot_window_state *st, float sr_hz, struct mot_features *out)
{
	const int n = (st->count < MOT_WINDOW_N) ? st->count : MOT_WINDOW_N;

	memset(out, 0, sizeof(*out));
	if (n <= 0) {
		return;
	}

	/* Per-axis means (accel + gyro) and |a| series. */
	float        mean_a[3] = { 0, 0, 0 }, mean_g[3] = { 0, 0, 0 };
	static float amag[MOT_WINDOW_N];
	float        mean_amag = 0.0f, sma = 0.0f;
	for (int i = 0; i < n; i++) {
		const struct mot_sample *s = &st->s[i];
		mean_a[0] += s->ax;
		mean_a[1] += s->ay;
		mean_a[2] += s->az;
		mean_g[0] += s->gx;
		mean_g[1] += s->gy;
		mean_g[2] += s->gz;
		amag[i] = sqrtf(s->ax * s->ax + s->ay * s->ay + s->az * s->az);
		mean_amag += amag[i];
		sma += fabsf(s->ax) + fabsf(s->ay) + fabsf(s->az);
	}
	for (int k = 0; k < 3; k++) {
		mean_a[k] /= (float)n;
		mean_g[k] /= (float)n;
	}
	mean_amag /= (float)n;
	out->sma = sma / (float)n;

	/* Per-axis AC RMS (accel + gyro), |gyro| RMS, |a| AC RMS, jerk RMS. */
	float sa[3] = { 0, 0, 0 }, sg[3] = { 0, 0, 0 };
	float s_amag = 0.0f, s_gmag = 0.0f, mean_gmag = 0.0f;
	for (int i = 0; i < n; i++) {
		const struct mot_sample *s = &st->s[i];
		float da[3]                = { s->ax - mean_a[0], s->ay - mean_a[1], s->az - mean_a[2] };
		float dg[3]                = { s->gx - mean_g[0], s->gy - mean_g[1], s->gz - mean_g[2] };
		for (int k = 0; k < 3; k++) {
			sa[k] += da[k] * da[k];
			sg[k] += dg[k] * dg[k];
		}
		float dm = amag[i] - mean_amag;
		s_amag += dm * dm;
		float gm = sqrtf(s->gx * s->gx + s->gy * s->gy + s->gz * s->gz);
		mean_gmag += gm;
	}
	mean_gmag /= (float)n;
	for (int i = 0; i < n; i++) {
		const struct mot_sample *s = &st->s[i];
		float gm = sqrtf(s->gx * s->gx + s->gy * s->gy + s->gz * s->gz) - mean_gmag;
		s_gmag += gm * gm;
	}
	for (int k = 0; k < 3; k++) {
		out->a_rms[k] = sqrtf(sa[k] / (float)n);
		out->g_rms[k] = sqrtf(sg[k] / (float)n);
	}
	out->amag_rms = sqrtf(s_amag / (float)n);
	out->gmag_rms = sqrtf(s_gmag / (float)n);

	float s_jerk = 0.0f;
	for (int i = 1; i < n; i++) {
		float d = (amag[i] - amag[i - 1]) * sr_hz; /* per-second jerk */
		s_jerk += d * d;
	}
	out->jerk_rms = (n > 1) ? sqrtf(s_jerk / (float)(n - 1)) : 0.0f;

	/* Tilt of the mean accel vector from vertical (Z). */
	out->tilt_deg = atan2f(sqrtf(mean_a[0] * mean_a[0] + mean_a[1] * mean_a[1]), mean_a[2]) *
	                180.0f / (float)M_PI;

	/* Dominant frequency of the |a| envelope via FFT (DC removed). */
	static float re[MOT_WINDOW_N];
	static float im[MOT_WINDOW_N];
	for (int i = 0; i < MOT_WINDOW_N; i++) {
		re[i] = (i < n) ? (amag[i] - mean_amag) : 0.0f;
		im[i] = 0.0f;
	}
	fft_radix2(re, im, MOT_WINDOW_N);
	const int half    = MOT_WINDOW_N / 2;
	int       dom_bin = 1;
	float     dom_val = -1.0f;
	for (int k = 1; k < half; k++) {
		float m2 = re[k] * re[k] + im[k] * im[k];
		if (m2 > dom_val) {
			dom_val = m2;
			dom_bin = k;
		}
	}
	out->dom_freq_hz = (float)dom_bin * sr_hz / (float)MOT_WINDOW_N;
}

size_t mot_feat_pack(const struct mot_features *f, float *vec, size_t cap)
{
	if (cap < (size_t)MOT_FEATURE_DIM) {
		return 0;
	}
	size_t i = 0;
	for (int k = 0; k < 3; k++) {
		vec[i++] = f->a_rms[k];
	}
	for (int k = 0; k < 3; k++) {
		vec[i++] = f->g_rms[k];
	}
	vec[i++] = f->amag_rms;
	vec[i++] = f->gmag_rms;
	vec[i++] = f->sma;
	vec[i++] = f->dom_freq_hz;
	vec[i++] = f->jerk_rms;
	vec[i++] = f->tilt_deg;
	return i; /* == MOT_FEATURE_DIM */
}

struct mot_verdict mot_activity_fallback(const struct mot_features *f)
{
	struct mot_verdict v = { ACT_IDLE, 0.0f };

	if (f->amag_rms < 0.05f) {
		v.cls        = ACT_IDLE;
		v.confidence = 0.8f;
	} else if (f->dom_freq_hz > 2.5f && f->amag_rms > 0.6f) {
		v.cls        = ACT_RUN;
		v.confidence = fminf(1.0f, f->amag_rms);
	} else {
		/* Moving but not running -> WALK (covers stairs too; the model splits). */
		v.cls        = ACT_WALK;
		v.confidence = 0.7f;
	}
	return v;
}

const char *mot_activity_name(mot_activity_t c)
{
	switch (c) {
	case ACT_IDLE:
		return "IDLE";
	case ACT_WALK:
		return "WALK";
	case ACT_RUN:
		return "RUN";
	case ACT_STAIRS:
		return "STAIRS";
	default:
		return "UNKNOWN";
	}
}
