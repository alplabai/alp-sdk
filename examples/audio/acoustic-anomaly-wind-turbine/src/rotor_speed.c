/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rotor_speed implementation -- see rotor_speed.h.
 */
#include "rotor_speed.h"

#include <math.h>

/* ---------------------------------------------------------------------------
 * rotor_bpf_hz -- blade-pass frequency from shaft speed.
 *
 * The blade-pass frequency (BPF) is the rotor-order invariant used throughout
 * the anomaly pipeline.  Each time a blade sweeps past a fixed reference
 * (e.g. tower shadow or structural resonance point) the aerodynamic load
 * produces an amplitude modulation at:
 *
 *     BPF = n_blades * rpm / 60     [Hz]
 *
 * All n_blades blades pass per revolution; dividing rpm by 60 converts to
 * revolutions per second.  A typical 3-blade turbine at 15 rpm gives
 * BPF = 3 * 15 / 60 = 0.75 Hz -- well within the band-energy envelope
 * window captured by bpf_modulation.
 * ---------------------------------------------------------------------------
 */
float rotor_bpf_hz(float rpm, uint8_t n_blades)
{
	return (float)n_blades * rpm / 60.0f;
}

/* ---------------------------------------------------------------------------
 * rotor_rpm_valid -- plausibility gate for wind-turbine rotor speed.
 *
 * Utility-scale horizontal-axis turbines operate in the narrow band 3..30 rpm
 * (cut-in to rated speed).  Values outside this range indicate either a
 * stopped rotor, sensor loss, or a fault condition; all invalidate BPF-based
 * anomaly detection and should be rejected before calling rotor_bpf_hz().
 * ---------------------------------------------------------------------------
 */
bool rotor_rpm_valid(float rpm)
{
	return rpm >= 3.0f && rpm <= 30.0f;
}

/* ---------------------------------------------------------------------------
 * rotor_tacho_rpm -- shaft RPM from a tacho pulse interval.
 *
 * Many turbine nacelles fit a Hall-effect or optical tachometer that produces
 * pulses_per_rev pulses per shaft revolution.  Firmware timestamps rising
 * edges and hands the inter-pulse interval (in microseconds) here.
 *
 * Derivation:
 *   rev_period_us = pulse_interval_us * pulses_per_rev    [us/rev]
 *   RPM = 60e6 / rev_period_us                           [rev/min]
 *
 * Double precision is used for the intermediate product to avoid overflow on
 * the 32-bit interval * 16-bit ppr multiplication (max ~2.8e9, fits in double
 * but not in uint32_t).  The result is downcast to float for the pipeline.
 * ---------------------------------------------------------------------------
 */
float rotor_tacho_rpm(uint32_t pulse_interval_us, uint16_t pulses_per_rev)
{
	if (pulse_interval_us == 0u || pulses_per_rev == 0u) {
		return 0.0f;
	}
	/* rev_period_us = interval * ppr; rpm = 60e6 / rev_period_us. */
	double rev_period_us = (double)pulse_interval_us * (double)pulses_per_rev;
	return (float)(60000000.0 / rev_period_us);
}

/* ---------------------------------------------------------------------------
 * rotor_tacholess_rpm -- mic-only RPM from blade-pass autocorrelation.
 *
 * When no tachometer is available, the blade-pass frequency can be recovered
 * from the band-energy envelope (env[]).  Blade faults and tower shadow impose
 * periodic amplitude modulation at the BPF; autocorrelation of the envelope
 * peaks at lags corresponding to multiples of the BPF period.
 *
 * Algorithm:
 *
 *   1. Mean-remove the envelope to suppress DC (constant background energy).
 *
 *   2. Convert the plausible RPM range (3..30 rpm) to a lag search window:
 *        max_bpf = n_blades * 30 / 60   [Hz]
 *        min_bpf = n_blades *  3 / 60   [Hz]
 *        lag_min = round(frame_rate_hz / max_bpf)    [frames]
 *        lag_max = round(frame_rate_hz / min_bpf)    [frames]
 *      lag_max is capped at n/2 to keep both halves of each lagged product
 *      within the buffer; the effective RPM floor is therefore:
 *        RPM_floor = 60 * frame_rate_hz / (n_blades * (n/2))
 *      At n=256, frame_rate_hz=62.5, n_blades=3 this gives ~10 rpm -- below
 *      that the estimate degrades and the tacho path should be preferred.
 *
 *   3. Compute the normalised (unbiased) autocorrelation for each candidate lag:
 *        R(lag) = sum_{i=0}^{n-lag-1} [ env[i] * env[i+lag] ] / (n - lag)
 *      Dividing by (n - lag) rather than n removes the small-lag bias that
 *      would otherwise push the argmax toward lag_min regardless of the signal:
 *      for smaller lags the sum has more terms, inflating R(lag) unless the
 *      per-term average is taken.
 *
 *   4. The lag with the highest R(lag) gives the dominant BPF period; convert:
 *        bpf = frame_rate_hz / best_lag
 *        rpm = 60 * bpf / n_blades
 * ---------------------------------------------------------------------------
 */
float rotor_tacholess_rpm(const float *env, size_t n, float frame_rate_hz, uint8_t n_blades)
{
	if (n < 32 || n_blades == 0u || frame_rate_hz <= 0.0f) {
		return 0.0f;
	}

	/* Step 1: mean-remove the envelope. */
	double mean = 0.0;
	for (size_t i = 0; i < n; i++) {
		mean += (double)env[i];
	}
	mean /= (double)n;

	/* Step 2: convert RPM bounds to lag bounds.
	 * Lag bounds derived from the plausible rotor range (3..30 rpm):
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

	/* Step 3: search for the peak of the normalised autocorrelation.
	 * Dividing by (n - lag) is the unbiased estimate: it removes the
	 * term-count advantage of smaller lags and yields a true per-term average
	 * so the argmax reflects the periodic structure, not the window length. */
	double best     = -1e300;
	size_t best_lag = lag_min;
	for (size_t lag = lag_min; lag <= lag_max; lag++) {
		double s = 0.0;
		for (size_t i = 0; i + lag < n; i++) {
			s += ((double)env[i] - mean) * ((double)env[i + lag] - mean);
		}
		/* Unbiased estimate: divide by the term count so the (n-lag) envelope
		 * does not bias the argmax toward small lags. */
		s /= (double)(n - lag);
		if (s > best) {
			best     = s;
			best_lag = lag;
		}
	}
	/* Step 4: convert best lag -> BPF -> RPM. */
	float bpf = frame_rate_hz / (float)best_lag;
	return 60.0f * bpf / (float)n_blades;
}
