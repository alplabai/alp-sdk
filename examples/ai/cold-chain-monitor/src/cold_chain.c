/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cold_chain implementation -- see cold_chain.h.  The two non-obvious formulas
 * (Mean Kinetic Temperature and the Magnus dewpoint) are derived inline.
 */
#include "cold_chain.h"

#include <math.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Window management.
 *
 * The window is a plain fixed-size array (not a ring buffer): samples are
 * written once and not overwritten.  A new monitoring epoch always starts
 * with cc_window_reset(), which resets only the count, not the array -- the
 * next cc_window_push() calls will overwrite stale entries naturally.
 * This design keeps the implementation interrupt-safe (no head/tail pair to
 * update atomically) and cache-friendly (linear scan for feature extraction).
 * ------------------------------------------------------------------------- */

void cc_window_reset(struct cc_window_state *st)
{
	/* Zero the counter; the sample array is written lazily as samples arrive. */
	st->count = 0;
}

void cc_window_push(struct cc_window_state *st, struct cc_sample s)
{
	/* Silently ignore pushes after the window is full.  The caller is
	 * expected to call cc_window_reset() to begin a new epoch. */
	if (st->count < CC_WINDOW_N) {
		st->s[st->count++] = s;
	}
}

bool cc_window_full(const struct cc_window_state *st)
{
	/* The window is full when count reaches the compile-time maximum.
	 * The Task-2 classifier gates on this before running inference. */
	return st->count >= CC_WINDOW_N;
}

/* -------------------------------------------------------------------------
 * Mean Kinetic Temperature (ICH Q1A / USP <1079>).
 *
 * MKT is the constant temperature that would inflict the same Arrhenius-rate
 * cumulative degradation as the real, fluctuating profile:
 *
 *     MKT = (ΔH/R) / ( -ln( (1/n) Σ exp( -(ΔH/R) / T_i ) ) )
 *
 * with T_i in KELVIN.  Because exp(-c/T) is convex, Jensen's inequality gives
 * MKT >= the arithmetic mean -- a short hot excursion raises MKT more than a
 * plain average would, which is exactly why cold-chain auditing uses MKT.
 * The accumulation runs in double precision because the exponentials are tiny.
 * ------------------------------------------------------------------------- */
static float mkt_celsius(const struct cc_window_state *st, int n)
{
	/* Accumulate Σ exp(-ΔH/R / T_i) in double to avoid catastrophic
	 * cancellation; individual terms can be as small as ~3e-14 at 4 °C. */
	double sum_exp = 0.0;
	for (int i = 0; i < n; i++) {
		/* Convert each Celsius reading to Kelvin before taking the exponential.
		 * Using double here preserves ~15 significant digits vs. ~7 for float. */
		double t_kelvin = (double)st->s[i].temp_c + 273.15;
		sum_exp += exp(-(double)CC_DH_OVER_R / t_kelvin);
	}
	/* Divide by n to get the mean of the Boltzmann-weighted exponentials. */
	double mean_exp = sum_exp / (double)n;
	/* Guard the log: mean_exp is strictly positive for any real temperature. */
	/* Invert the logarithm: MKT_K = (ΔH/R) / (-ln(mean_exp)). */
	double mkt_kelvin = (double)CC_DH_OVER_R / (-log(mean_exp));
	/* Convert back to Celsius for the application layer. */
	return (float)(mkt_kelvin - 273.15);
}

/* -------------------------------------------------------------------------
 * Magnus dewpoint approximation (a = 17.62, b = 243.12 °C):
 *
 *     γ  = ln(RH/100) + a·T / (b + T)
 *     Td = b·γ / (a - γ)
 *
 * Valid for RH in (0, 100]; we clamp RH to >= 1% to keep the log finite.
 * Error < 0.35 °C over 0..60 °C / 1..100% RH, which is adequate for
 * condensation-risk detection.
 * ------------------------------------------------------------------------- */
static float dewpoint_celsius(float temp_c, float rh_pct)
{
	/* Magnus coefficients (Alduchov & Eskridge 1996 optimisation). */
	const float a = 17.62f, b = 243.12f;
	/* Clamp RH to a safe range: log(0) = -∞, log(>100%) is physically wrong. */
	float rh = (rh_pct < 1.0f) ? 1.0f : ((rh_pct > 100.0f) ? 100.0f : rh_pct);
	/* γ combines the vapour-pressure and temperature terms. */
	float gamma = logf(rh / 100.0f) + (a * temp_c) / (b + temp_c);
	/* Solve for the temperature at which RH would equal 100%. */
	return (b * gamma) / (a - gamma);
}

/* -------------------------------------------------------------------------
 * Main feature extraction pass.
 * ------------------------------------------------------------------------- */

void cc_feat_extract(const struct cc_window_state *st,
                     const struct cc_config       *cfg,
                     struct cc_features           *out)
{
	/* Cap at CC_WINDOW_N; cc_window_push() never exceeds it but the cast
	 * from uint16_t makes the compiler happy with signed loop indices. */
	const int n = (st->count < CC_WINDOW_N) ? st->count : CC_WINDOW_N;

	/* Zero all output fields; early return leaves them at zero for empty windows. */
	memset(out, 0, sizeof(*out));
	if (n <= 0) {
		return;
	}

	/* ------------------------------------------------------------------
	 * First pass: means, min/max temperature, and the out-of-band sample
	 * count.  A single pass keeps cache pressure low on the M55.
	 *
	 * We intentionally combine all four aggregates (mean_t, mean_rh,
	 * min_t, max_t) into one loop iteration to maximise data locality:
	 * each cc_sample is 12 bytes and fits in a single cache line fetch.
	 * ------------------------------------------------------------------ */
	float sum_t = 0.0f, sum_rh = 0.0f;
	/* Seed min/max with the first sample to avoid a sentinel like FLT_MAX. */
	float min_t = st->s[0].temp_c, max_t = st->s[0].temp_c;
	int   out_of_band = 0;
	for (int i = 0; i < n; i++) {
		float t = st->s[i].temp_c;
		sum_t += t;
		sum_rh += st->s[i].rh_pct;
		/* Track the coldest and warmest readings seen in the window. */
		if (t < min_t) {
			min_t = t;
		}
		if (t > max_t) {
			max_t = t;
		}
		/* A reading counts as an excursion when it leaves the product's band. */
		if (t < cfg->t_lo || t > cfg->t_hi) {
			out_of_band++;
		}
	}
	/* Store the four scalar aggregates. */
	out->mean_temp_c = sum_t / (float)n;
	out->mean_rh_pct = sum_rh / (float)n;
	out->min_temp_c  = min_t;
	out->max_temp_c  = max_t;
	/* Convert out-of-band sample count to minutes via the per-sample duration. */
	out->excursion_min = (float)out_of_band * CC_SAMPLE_MIN;

	/* ------------------------------------------------------------------
	 * Warming/cooling trend: last-quarter mean minus first-quarter mean,
	 * divided by the window duration in minutes (positive = warming).
	 *
	 * Using quarter-window means rather than raw endpoints suppresses the
	 * noise of a single outlier reading biasing the trend estimate.  This
	 * is a simple but effective linear trend approximation that avoids the
	 * overhead of a full least-squares regression on the M55.
	 * ------------------------------------------------------------------ */
	int q = n / 4;
	/* Guard: if the window is very short (< 4 samples), use a single sample. */
	if (q < 1) {
		q = 1;
	}
	/* Accumulate the first and last quarters separately. */
	float first = 0.0f, last = 0.0f;
	for (int i = 0; i < q; i++) {
		first += st->s[i].temp_c;
		/* Mirror-index from the end to get the last quarter. */
		last += st->s[n - 1 - i].temp_c;
	}
	/* Slope = ΔT / Δt in °C/min; normalise both differences by the same q. */
	out->temp_slope_c_per_min = ((last - first) / (float)q) / ((float)n * CC_SAMPLE_MIN);

	/* Dewpoint uses the window-mean T/RH so it lines up with the classifier's
	 * mean-temperature comparison. */
	out->dewpoint_c = dewpoint_celsius(out->mean_temp_c, out->mean_rh_pct);

	/* Mean kinetic temperature over the whole window (expensive: n exp() calls;
	 * runs last so the earlier cheap fields are ready for early-exit callers). */
	out->mkt_c = mkt_celsius(st, n);
}

/* -------------------------------------------------------------------------
 * Feature packing -- lay the metrics out in a fixed order for the AI model.
 *
 * The order here must match the column order used when the classifier was
 * trained (see tools/training/feature_extractor.py).  If a feature is added
 * or removed, CC_FEATURE_DIM must be updated AND the model must be retrained.
 *
 * The function intentionally takes a raw float pointer + capacity so that the
 * same vector can be passed directly to the .alpmodel inference call without
 * an intermediate copy.
 * ------------------------------------------------------------------------- */

size_t cc_feat_pack(const struct cc_features *f, float *vec, size_t cap)
{
	/* Refuse to write a partial vector: the model expects exactly CC_FEATURE_DIM
	 * floats and must not receive a shorter, ambiguously-padded slice. */
	if (cap < (size_t)CC_FEATURE_DIM) {
		return 0;
	}
	/* Pack in the same order the training pipeline used to build the model.
	 * Any reordering here must be mirrored in the Python feature_extractor. */
	size_t i = 0;
	/* Temperature statistics (3 floats). */
	vec[i++] = f->mean_temp_c;
	vec[i++] = f->min_temp_c;
	vec[i++] = f->max_temp_c;
	/* Humidity mean (1 float). */
	vec[i++] = f->mean_rh_pct;
	/* Trend and derived metrics (4 floats). */
	vec[i++] = f->temp_slope_c_per_min;
	vec[i++] = f->dewpoint_c;
	vec[i++] = f->mkt_c;
	vec[i++] = f->excursion_min;
	return i; /* == CC_FEATURE_DIM */
}
