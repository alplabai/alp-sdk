/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * acoustic_event implementation -- see acoustic_event.h.
 */
#include "acoustic_event.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void ase_frame_reset(struct ase_frame_state *st)
{
	st->count = 0;
}

void ase_frame_push(struct ase_frame_state *st, float sample)
{
	if (st->count < ASE_FRAME_N) {
		st->samples[st->count++] = sample;
	}
}

bool ase_frame_full(const struct ase_frame_state *st)
{
	return st->count >= ASE_FRAME_N;
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

void ase_feat_extract(const struct ase_frame_state *st, float sr_hz, struct ase_features *out)
{
	const int n = (st->count < ASE_FRAME_N) ? st->count : ASE_FRAME_N;

	memset(out, 0, sizeof(*out));
	if (n <= 0) {
		return;
	}

	/* DC removal + time-domain features (RMS, crest, ZCR). */
	float mean = 0.0f;
	for (int i = 0; i < n; i++) {
		mean += st->samples[i];
	}
	mean /= (float)n;

	float sum2 = 0.0f, peak = 0.0f;
	int   zc   = 0;
	float prev = st->samples[0] - mean;
	for (int i = 0; i < n; i++) {
		float x = st->samples[i] - mean;
		sum2 += x * x;
		if (fabsf(x) > peak) {
			peak = fabsf(x);
		}
		if (i > 0 && ((x < 0.0f) != (prev < 0.0f))) {
			zc++;
		}
		prev = x;
	}
	out->rms   = sqrtf(sum2 / (float)n);
	out->crest = (out->rms > 1e-6f) ? (peak / out->rms) : 0.0f;
	out->zcr   = (float)zc / (float)n;

	/* Spectrum. */
	static float re[ASE_FRAME_N];
	static float im[ASE_FRAME_N];
	for (int i = 0; i < ASE_FRAME_N; i++) {
		re[i] = (i < n) ? (st->samples[i] - mean) : 0.0f;
		im[i] = 0.0f;
	}
	fft_radix2(re, im, ASE_FRAME_N);

	const int half = ASE_FRAME_N / 2;
	float     mag[ASE_FRAME_N / 2];
	float     mag_total = 0.0f, centroid_num = 0.0f, log_sum = 0.0f, lin_sum = 0.0f;
	int       active = 0;
	for (int k = 1; k < half; k++) {
		float m  = sqrtf(re[k] * re[k] + im[k] * im[k]);
		mag[k]   = m;
		float fr = (float)k * sr_hz / (float)ASE_FRAME_N;
		mag_total += m;
		centroid_num += fr * m;
		float me = m + 1e-9f;
		log_sum += logf(me);
		lin_sum += me;
		active++;
	}
	out->centroid_hz = (mag_total > 1e-12f) ? (centroid_num / mag_total) : 0.0f;
	out->flatness    = (active > 0 && lin_sum > 1e-12f)
	                       ? (expf(log_sum / (float)active) / (lin_sum / (float)active))
	                       : 0.0f;

	/* Spectral rolloff: lowest freq where cumulative |X|^2 >= 85% of total. */
	float total2 = 0.0f;
	for (int k = 1; k < half; k++) {
		total2 += mag[k] * mag[k];
	}
	float cum       = 0.0f;
	out->rolloff_hz = 0.0f;
	for (int k = 1; k < half; k++) {
		cum += mag[k] * mag[k];
		if (total2 > 1e-20f && cum >= 0.85f * total2) {
			out->rolloff_hz = (float)k * sr_hz / (float)ASE_FRAME_N;
			break;
		}
	}

	/* Log-spaced band energies over bins 1..half-1, normalised to sum 1. */
	if (total2 < 1e-20f) {
		return;
	}
	for (int k = 1; k < half; k++) {
		float pos = logf((float)k) / logf((float)half);
		int   b   = (int)(pos * (float)ASE_N_BANDS);
		if (b < 0) {
			b = 0;
		}
		if (b >= ASE_N_BANDS) {
			b = ASE_N_BANDS - 1;
		}
		out->band_energy[b] += mag[k] * mag[k];
	}
	for (int b = 0; b < ASE_N_BANDS; b++) {
		out->band_energy[b] /= total2;
	}
}

size_t ase_feat_pack(const struct ase_features *f, float *vec, size_t cap)
{
	if (cap < (size_t)ASE_FEATURE_DIM) {
		return 0;
	}
	size_t i = 0;
	for (int b = 0; b < ASE_N_BANDS; b++) {
		vec[i++] = f->band_energy[b];
	}
	vec[i++] = f->centroid_hz;
	vec[i++] = f->flatness;
	vec[i++] = f->rolloff_hz;
	vec[i++] = f->crest;
	vec[i++] = f->zcr;
	vec[i++] = f->rms;
	return i; /* == ASE_FEATURE_DIM */
}

struct ase_verdict ase_classify_fallback(const struct ase_features *f)
{
	struct ase_verdict v = { ASE_AMBIENT, 0.6f };

	if (f->rms < 0.02f) {
		v.ev         = ASE_AMBIENT;
		v.confidence = 0.8f;
	} else if (f->crest > 4.0f && f->centroid_hz > 4000.0f && f->zcr > 0.4f) {
		/* Broadband impulsive high-frequency burst. */
		v.ev         = ASE_GLASS_BREAK;
		v.confidence = 0.85f;
	} else if (f->flatness < 0.2f && f->centroid_hz >= 2500.0f && f->centroid_hz <= 4000.0f) {
		/* Narrowband tonal beep in the alarm band. */
		v.ev         = ASE_ALARM;
		v.confidence = 0.9f;
	} else if (f->rms > 0.1f && f->centroid_hz >= 800.0f && f->centroid_hz < 2500.0f) {
		/* High-energy harmonic voice band. */
		v.ev         = ASE_SCREAM;
		v.confidence = 0.75f;
	} else {
		v.ev         = ASE_AMBIENT;
		v.confidence = 0.5f;
	}
	return v;
}

const char *ase_event_name(ase_event_t e)
{
	switch (e) {
	case ASE_AMBIENT:
		return "AMBIENT";
	case ASE_GLASS_BREAK:
		return "GLASS_BREAK";
	case ASE_ALARM:
		return "ALARM";
	case ASE_SCREAM:
		return "SCREAM";
	default:
		return "UNKNOWN";
	}
}
