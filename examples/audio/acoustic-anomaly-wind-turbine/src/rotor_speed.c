/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rotor_speed implementation -- see rotor_speed.h.
 */
#include "rotor_speed.h"

#include <math.h>

float rotor_bpf_hz(float rpm, uint8_t n_blades)
{
	return (float)n_blades * rpm / 60.0f;
}

bool rotor_rpm_valid(float rpm)
{
	return rpm >= 3.0f && rpm <= 30.0f;
}

float rotor_tacho_rpm(uint32_t pulse_interval_us, uint16_t pulses_per_rev)
{
	if (pulse_interval_us == 0u || pulses_per_rev == 0u) {
		return 0.0f;
	}
	/* rev_period_us = interval * ppr; rpm = 60e6 / rev_period_us. */
	double rev_period_us = (double)pulse_interval_us * (double)pulses_per_rev;
	return (float)(60000000.0 / rev_period_us);
}

float rotor_tacholess_rpm(const float *env, size_t n, float frame_rate_hz, uint8_t n_blades)
{
	if (n < 32 || n_blades == 0u || frame_rate_hz <= 0.0f) {
		return 0.0f;
	}

	/* Mean-remove. */
	double mean = 0.0;
	for (size_t i = 0; i < n; i++) {
		mean += (double)env[i];
	}
	mean /= (double)n;

	/* Lag bounds derived from the plausible rotor range (3..30 rpm):
	 * max_bpf = n_blades * 30 / 60 -> lag_min = frame_rate / max_bpf.
	 * min_bpf = n_blades *  3 / 60 -> lag_phys = frame_rate / min_bpf.
	 * Cap lag_max at n/2 to stay within the window. */
	float  max_bpf  = (float)n_blades * 30.0f / 60.0f;
	float  min_bpf  = (float)n_blades * 3.0f / 60.0f;
	size_t lag_min  = (size_t)(frame_rate_hz / max_bpf);
	size_t lag_phys = (size_t)(frame_rate_hz / min_bpf);
	size_t lag_max  = lag_phys < n / 2u ? lag_phys : n / 2u;

	if (lag_min < 1u) {
		lag_min = 1u;
	}
	if (lag_max <= lag_min) {
		return 0.0f;
	}

	double best     = -1e300;
	size_t best_lag = lag_min;
	for (size_t lag = lag_min; lag <= lag_max; lag++) {
		double s = 0.0;
		for (size_t i = 0; i + lag < n; i++) {
			s += ((double)env[i] - mean) * ((double)env[i + lag] - mean);
		}
		if (s > best) {
			best     = s;
			best_lag = lag;
		}
	}
	float bpf = frame_rate_hz / (float)best_lag;
	return 60.0f * bpf / (float)n_blades;
}
