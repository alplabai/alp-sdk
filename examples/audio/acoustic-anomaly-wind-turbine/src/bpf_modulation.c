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

void bpf_env_reset(struct bpf_env_state *st)
{
	st->head  = 0;
	st->count = 0;
}

void bpf_env_push(struct bpf_env_state *st, float frame_energy)
{
	st->env[st->head] = frame_energy;
	st->head          = (uint16_t)((st->head + 1) % BPF_ENV_N);
	if (st->count < BPF_ENV_N) {
		st->count++;
	}
}

/* Generalised Goertzel: |X|^2 at an arbitrary frequency target_hz. */
static float goertzel_power(const float *x, int n, float target_hz, float fs)
{
	if (target_hz <= 0.0f || fs <= 0.0f) {
		return 0.0f;
	}
	float w     = 2.0f * (float)M_PI * target_hz / fs;
	float coeff = 2.0f * cosf(w);
	float s1 = 0.0f, s2 = 0.0f;
	for (int i = 0; i < n; i++) {
		float s0 = x[i] + coeff * s1 - s2;
		s2       = s1;
		s1       = s0;
	}
	return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

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

	/* Linearise the ring (oldest..newest) into a temp, mean-removed. */
	static float buf[BPF_ENV_N];
	float        mean  = 0.0f;
	int          start = (st->count < BPF_ENV_N) ? 0 : st->head;
	for (int i = 0; i < n; i++) {
		buf[i] = st->env[(start + i) % BPF_ENV_N];
		mean += buf[i];
	}
	mean /= (float)n;

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

	/* Total envelope (AC) energy for normalisation. */
	float total = 0.0f;
	for (int i = 0; i < n; i++) {
		total += buf[i] * buf[i];
	}
	/* |X(k)|^2 sums to n*Sigma|x|^2 (Parseval); divide by (n*total) so each
	 * blade-order energy is a bounded fraction of the envelope AC energy. */
	float norm = (total > 1e-12f) ? (1.0f / ((float)n * total)) : 0.0f;

	for (int k = 1; k <= BPF_N_HARMONICS; k++) {
		float p                        = goertzel_power(buf, n, (float)k * bpf_hz, frame_rate_hz);
		out->blade_order_energy[k - 1] = p * norm;
	}
	out->modulation_depth = (emax + emin > 1e-9f) ? ((emax - emin) / (emax + emin)) : 0.0f;
}

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
