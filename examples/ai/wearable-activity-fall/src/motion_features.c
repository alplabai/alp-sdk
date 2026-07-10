/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * motion_features implementation -- see motion_features.h.
 *
 * This file implements the windowed IMU feature extraction pipeline used by the
 * wearable activity-recognition + fall-detection example.  It builds identically
 * for native_sim and Cortex-M55.  Read this file alongside motion_features.h.
 *
 * Pipeline overview (one 2.56 s window at 100 Hz = 256 samples):
 *
 *   raw IMU samples  (ax, ay, az g;  gx, gy, gz deg/s)
 *       |
 *       v
 *   mot_window_push()   -- append samples until the ring is full
 *       |
 *       v
 *   mot_feat_extract()  -- compute the MOT_FEATURE_DIM = 12 features:
 *       |
 *       +-- a_rms[3]      per-axis accel AC RMS          (vibration per axis)
 *       +-- g_rms[3]      per-axis gyro  AC RMS          (rotation rate per axis)
 *       +-- amag_rms      accel-magnitude AC RMS          (overall motion level)
 *       +-- gmag_rms      gyro-magnitude  AC RMS          (overall rotation level)
 *       +-- sma           signal-magnitude area           (total accel energy)
 *       +-- dom_freq_hz   dominant FFT bin of |a|         (step / stride cadence)
 *       +-- jerk_rms      RMS of d|a|/dt                  (impact sharpness)
 *       +-- tilt_deg      tilt of the mean gravity vector  (body orientation)
 *       |
 *       v
 *   mot_feat_pack()     -- flatten to float[12] for the AI model input tensor
 *       |
 *       v
 *   model inference  OR  mot_activity_fallback()  (when no model is loaded)
 *
 * SPECTRAL + STATISTICAL MATH -- library-backed, not hand-rolled.
 * ---------------------------------------------------------------
 * This example deliberately does NOT re-derive an FFT or the RMS/mean
 * statistics by hand.  Two library surfaces do the numeric work, and the
 * SAME source builds for native_sim and the Cortex-M55 alike:
 *
 *   1. The FFT goes through the portable <alp/dsp.h> chain API.  Its
 *      backend runs ARM CMSIS-DSP (arm_rfft_fast_f32, Helium-vectorised)
 *      on the M55, a hardware DSP block where the SoM has one, and a
 *      portable-C radix-2 fallback under native_sim -- selected by the
 *      backend registry, not an #ifdef here.  We call the alp wrapper
 *      (not arm_rfft_* directly) because alp-sdk SHIPS a portable FFT
 *      surface: keeping the example on it lets the identical source run
 *      on the V2N (A55 + DRP-AI) and NXP paths, which have no CMSIS-M.
 *
 *   2. The |a| / |gyro| magnitude-series statistics (mean, AC RMS) and the
 *      jerk RMS call ARM CMSIS-DSP arm_*_f32 kernels DIRECTLY -- alp-sdk
 *      has no portable scalar-stats surface to wrap, and these kernels are
 *      the idiomatic accelerated path on Cortex-M.  They are guarded by
 *      __has_include with a portable-C fallback so the native_sim gate
 *      still builds.  The per-axis accel/gyro AC RMS stays a portable loop
 *      (see the comment at its call site) because the IMU sample buffer is
 *      array-of-structs, not a layout CMSIS kernels can consume directly.
 */
#include "motion_features.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if defined(__has_include)
#if __has_include("arm_math.h")
#include "arm_math.h"
#define MOT_HAS_CMSIS_DSP 1
#endif
#endif
#ifndef MOT_HAS_CMSIS_DSP
#define MOT_HAS_CMSIS_DSP 0
#endif

#include <alp/dsp.h>

/* ===========================================================================
 * Window management
 *
 * The window is a fixed-length linear buffer of MOT_WINDOW_N IMU samples.
 * mot_window_push() appends samples one at a time; mot_window_full() signals
 * the caller to drain the window by calling mot_feat_extract() and then
 * mot_window_reset() before pushing the next batch.
 *
 * Overlapping windows (stride < MOT_WINDOW_N) are not built into this API
 * but can be achieved by the caller: copy the last (MOT_WINDOW_N - stride)
 * samples into a fresh mot_window_state before resetting.
 * =========================================================================== */

void mot_window_reset(struct mot_window_state *st)
{
	st->count = 0;
}

void mot_window_push(struct mot_window_state *st, struct mot_sample s)
{
	/* Silently drop samples that arrive after the buffer is full.  The
	 * caller is responsible for checking mot_window_full() and resetting
	 * before pushing the next window's samples. */
	if (st->count < MOT_WINDOW_N) {
		st->s[st->count++] = s;
	}
}

bool mot_window_full(const struct mot_window_state *st)
{
	return st->count >= MOT_WINDOW_N;
}

/* ===========================================================================
 * Feature extraction  --  mot_feat_extract()
 *
 * Computes all MOT_FEATURE_DIM = 12 features from a single filled window.
 * All features are dimensionally consistent: accel in g, gyro in deg/s,
 * frequency in Hz, angle in degrees.
 *
 * ---------------------------------------------------------------------------
 * AC (alternating-current) RMS
 * ---------------------------------------------------------------------------
 * "AC RMS" is the RMS of the mean-removed signal.  For a discrete signal x[i]
 * of length N:
 *
 *   mu   = (1/N) * sum_{i} x[i]          (DC mean)
 *   AC_RMS = sqrt( (1/N) * sum_{i} (x[i] - mu)^2 )
 *
 * Removing the DC mean isolates the dynamic (vibrational) component.
 * Gravity contributes ~1 g on the dominant accel axis; without DC removal
 * that constant term dominates and masks the small differences in vibration
 * intensity between walking (~0.2 g RMS) and running (~0.6 g RMS).
 *
 * ---------------------------------------------------------------------------
 * Signal-magnitude area (SMA)
 * ---------------------------------------------------------------------------
 * SMA = (1/N) * sum_{i} ( |ax[i]| + |ay[i]| + |az[i]| )
 *
 * SMA is the per-sample mean of the L1 norm of the accel vector.  It does
 * NOT subtract the mean, so gravity contributes ~1 g at rest.  This makes it
 * a simple but effective rest/active discriminator:
 *   resting      : SMA ≈ 1 g  (gravity only)
 *   walking      : SMA ≈ 1.2--1.5 g
 *   running      : SMA ≈ 1.8--2.5 g
 *   free-fall    : SMA → 0 g
 *
 * Dividing by N normalises across partial and full windows.  Left as a
 * portable loop: it combines three strided (array-of-structs) fields per
 * sample, which is not a shape a CMSIS elementwise kernel can consume.
 *
 * ---------------------------------------------------------------------------
 * Jerk RMS
 * ---------------------------------------------------------------------------
 * Jerk = d|a|/dt  (rate of change of acceleration magnitude).
 * Approximated as the first backward difference of the amag series scaled to
 * physical units by multiplying by sr_hz:
 *
 *   jerk[i] = ( amag[i] - amag[i-1] ) * sr_hz      (units: g/s)
 *
 *   jerk_rms = sqrt( (1/(N-1)) * sum_{i=1}^{N-1} jerk[i]^2 )
 *
 * amag[] is a contiguous time series, so this is a natural CMSIS-DSP fit:
 * arm_sub_f32(amag+1, amag, ..., N-1) computes every backward difference in
 * one vectorised call (pDst[k] = amag[k+1] - amag[k]), then arm_scale_f32
 * converts to g/s and arm_rms_f32 finishes the RMS in a single pass.
 * Scaling by sr_hz converts from "g per sample" to "g per second", making
 * the value sample-rate-independent so models trained at one sr_hz transfer
 * to another.  High jerk_rms indicates sharp, impulsive motion:
 *   walking      : moderate (~0.5 g/s)
 *   running      : elevated (~2 g/s, heel-strike spikes)
 *   fall impact  : very high (can exceed 10 g/s)
 *
 * ---------------------------------------------------------------------------
 * Tilt from vertical
 * ---------------------------------------------------------------------------
 * When motion is slow relative to gravity, the window-mean accel vector
 * approximates the gravity direction.  The tilt angle between that vector
 * and the device's Z-axis:
 *
 *   tilt = atan2( sqrt(mean_ax^2 + mean_ay^2), mean_az ) * 180/pi
 *
 * atan2 handles all quadrants and avoids division-by-zero when mean_az ≈ 0.
 * Example: device flat on a table → tilt ≈ 0°; worn on the side of the
 * wrist → tilt ≈ 90°.  The AI model uses tilt as a weak posture cue.  A
 * single atan2 call has no vector shape for CMSIS to accelerate, so it
 * stays a plain scalar expression.
 *
 * ---------------------------------------------------------------------------
 * Dominant cadence frequency via FFT
 * ---------------------------------------------------------------------------
 * Walking/running produces a nearly periodic oscillation in the accel
 * magnitude |a| at the step cadence (the "footfall frequency"):
 *   walking : ~1.5--2.5 Hz (doubles if two steps per oscillation)
 *   running : ~2.5--4.0 Hz
 *   cycling : ~1.0--1.5 Hz (pedal rate, not step rate)
 *
 * Frequency resolution:  delta_f = sr_hz / N = 100 / 256 ≈ 0.39 Hz/bin.
 * Nyquist limit:         f_max   = sr_hz / 2 = 50 Hz  (bin N/2-1 = 127).
 * Bin 0 is DC (suppressed by mean removal, explicitly skipped).
 * Negative-frequency bins (k >= N/2) mirror the positive bins for real input;
 * only bins 1..N/2-1 carry independent spectral information.  See the
 * "SPECTRUM" comment at the FFT call site below for how the transform itself
 * is obtained.
 * =========================================================================== */
void mot_feat_extract(const struct mot_window_state *st, float sr_hz, struct mot_features *out)
{
	const int n = (st->count < MOT_WINDOW_N) ? st->count : MOT_WINDOW_N;

	memset(out, 0, sizeof(*out));
	if (n <= 0) {
		return;
	}

	/*
	 * PASS A -- per-axis raw sums, |a| and |gyro| magnitude series, SMA.
	 *
	 * amag[i]/gmag[i] : Euclidean magnitude of sample i's accel/gyro vector.
	 *   These end up as CONTIGUOUS time series (unlike the raw AoS sample
	 *   buffer), which is exactly the shape the CMSIS-DSP mean/RMS kernels
	 *   below want -- that is why the per-sample combine of 3 strided
	 *   fields happens once here instead of being folded into each stat.
	 * mean_a[]/mean_g[] : running sums of the strided per-axis fields,
	 *   divided below.  Kept as a portable loop: CMSIS-DSP kernels require
	 *   stride-1 arrays, and the IMU sample buffer is array-of-structs
	 *   (ax,ay,az,gx,gy,gz interleaved per sample) -- deinterleaving 6
	 *   channels into 6 scratch buffers just to call arm_mean_f32 would
	 *   cost more (extra O(N) copies + 6 * MOT_WINDOW_N * 4B of static RAM)
	 *   than the 3-element running sum it would replace.
	 */
	float        mean_a[3] = { 0, 0, 0 }, mean_g[3] = { 0, 0, 0 };
	static float amag[MOT_WINDOW_N];
	static float gmag[MOT_WINDOW_N];
	float        sma = 0.0f;
	for (int i = 0; i < n; i++) {
		const struct mot_sample *s = &st->s[i];
		mean_a[0] += s->ax;
		mean_a[1] += s->ay;
		mean_a[2] += s->az;
		mean_g[0] += s->gx;
		mean_g[1] += s->gy;
		mean_g[2] += s->gz;
		amag[i] = sqrtf(s->ax * s->ax + s->ay * s->ay + s->az * s->az);
		gmag[i] = sqrtf(s->gx * s->gx + s->gy * s->gy + s->gz * s->gz);
		/* SMA numerator: L1 norm of the raw accel vector (gravity included). */
		sma += fabsf(s->ax) + fabsf(s->ay) + fabsf(s->az);
	}
	for (int k = 0; k < 3; k++) {
		mean_a[k] /= (float)n;
		mean_g[k] /= (float)n;
	}
	/* Normalise SMA to a per-sample mean (window-length independent). */
	out->sma = sma / (float)n;

	/*
	 * PASS B -- per-axis accel/gyro AC RMS.  Stays a portable loop for the
	 * same array-of-structs reason as mean_a[]/mean_g[] above: da[]/dg[]
	 * are 3-element per-sample vectors carved out of strided fields, not
	 * contiguous arrays a CMSIS elementwise kernel can walk.
	 */
	float sa[3] = { 0, 0, 0 }, sg[3] = { 0, 0, 0 };
	for (int i = 0; i < n; i++) {
		const struct mot_sample *s = &st->s[i];
		float da[3]                = { s->ax - mean_a[0], s->ay - mean_a[1], s->az - mean_a[2] };
		float dg[3]                = { s->gx - mean_g[0], s->gy - mean_g[1], s->gz - mean_g[2] };
		for (int k = 0; k < 3; k++) {
			sa[k] += da[k] * da[k];
			sg[k] += dg[k] * dg[k];
		}
	}
	for (int k = 0; k < 3; k++) {
		out->a_rms[k] = sqrtf(sa[k] / (float)n);
		out->g_rms[k] = sqrtf(sg[k] / (float)n);
	}

	/*
	 * |a| / |gyro| magnitude-series stats + jerk RMS -- CMSIS-DSP path.
	 * ------------------------------------------------------------------
	 * amag[]/gmag[] are contiguous, stride-1 float arrays (built in pass A
	 * above), so mean + AC RMS map directly onto arm_mean_f32 /
	 * arm_offset_f32 / arm_rms_f32, and the jerk backward-difference onto
	 * arm_sub_f32 + arm_scale_f32 + arm_rms_f32.  amag_ac[] doubles as the
	 * mean-centred series fed to the FFT below (zero-padded past n).
	 */
	static float amag_ac[MOT_WINDOW_N];
	static float gmag_ac[MOT_WINDOW_N];
	static float jerk[MOT_WINDOW_N];
	float        mean_amag, mean_gmag, peak;

#if MOT_HAS_CMSIS_DSP
	uint32_t peak_idx;
	arm_mean_f32(amag, (uint32_t)n, &mean_amag);
	arm_mean_f32(gmag, (uint32_t)n, &mean_gmag);
	arm_offset_f32(amag, -mean_amag, amag_ac, (uint32_t)n);
	arm_offset_f32(gmag, -mean_gmag, gmag_ac, (uint32_t)n);
	arm_rms_f32(amag_ac, (uint32_t)n, &out->amag_rms);
	arm_rms_f32(gmag_ac, (uint32_t)n, &out->gmag_rms);
	arm_absmax_f32(amag_ac, (uint32_t)n, &peak, &peak_idx);
	if (n > 1) {
		/* jerk[k] = amag[k+1] - amag[k], all N-1 differences in one call. */
		arm_sub_f32(&amag[1], &amag[0], jerk, (uint32_t)(n - 1));
		arm_scale_f32(jerk, sr_hz, jerk, (uint32_t)(n - 1));
		arm_rms_f32(jerk, (uint32_t)(n - 1), &out->jerk_rms);
	} else {
		out->jerk_rms = 0.0f;
	}
#else
	/* Portable-C fallback (native_sim, or any target without CMSIS-DSP). */
	mean_amag = 0.0f;
	mean_gmag = 0.0f;
	for (int i = 0; i < n; i++) {
		mean_amag += amag[i];
		mean_gmag += gmag[i];
	}
	mean_amag /= (float)n;
	mean_gmag /= (float)n;

	float s_amag = 0.0f, s_gmag = 0.0f;
	peak = 0.0f;
	for (int i = 0; i < n; i++) {
		amag_ac[i] = amag[i] - mean_amag;
		gmag_ac[i] = gmag[i] - mean_gmag;
		s_amag += amag_ac[i] * amag_ac[i];
		s_gmag += gmag_ac[i] * gmag_ac[i];
		float ampk = fabsf(amag_ac[i]);
		if (ampk > peak) {
			peak = ampk;
		}
	}
	out->amag_rms = sqrtf(s_amag / (float)n);
	out->gmag_rms = sqrtf(s_gmag / (float)n);

	float s_jerk = 0.0f;
	for (int i = 1; i < n; i++) {
		jerk[i - 1] = (amag[i] - amag[i - 1]) * sr_hz; /* per-second jerk */
		s_jerk += jerk[i - 1] * jerk[i - 1];
	}
	out->jerk_rms = (n > 1) ? sqrtf(s_jerk / (float)(n - 1)) : 0.0f;
#endif
	for (int i = n; i < MOT_WINDOW_N; i++) {
		amag_ac[i] = 0.0f; /* zero-pad the FFT tail on a short window */
	}

	/* ---- Tilt of the mean accel vector from vertical (Z) ------------------ */
	/*
	 * The horizontal component of the mean gravity vector is the L2 norm of
	 * (mean_ax, mean_ay).  atan2 gives the angle from the Z-axis in [0, pi].
	 * For Z-up mounting: tilt ≈ 0° when flat, ≈ 90° when upright.
	 */
	out->tilt_deg = atan2f(sqrtf(mean_a[0] * mean_a[0] + mean_a[1] * mean_a[1]), mean_a[2]) *
	                180.0f / (float)M_PI;

	/*
	 * SPECTRUM via the portable <alp/dsp.h> chain (NOT a hand-rolled FFT).
	 * ------------------------------------------------------------------
	 * A single ALP_DSP_STAGE_FFT (rectangular, no window) transforms the
	 * mean-centred |a| series to magnitude bins.  The backend runs CMSIS-DSP
	 * arm_rfft_fast_f32 on the M55 and a portable-C radix-2 FFT under
	 * native_sim -- the example source is identical either way.
	 *
	 * The chain consumes int16 samples (the accelerometer's native ADC
	 * format).  We scale the float amag_ac[] window to fill the int16 range
	 * before feeding it: the absolute scale is irrelevant here because
	 * dom_freq_hz is derived from an argmax over the output bins, which is
	 * scale-invariant.
	 */
	static int16_t samp_q15[MOT_WINDOW_N];
	float          scale = (peak > 1e-9f) ? (30000.0f / peak) : 0.0f;
	for (int i = 0; i < MOT_WINDOW_N; i++) {
		samp_q15[i] = (int16_t)lrintf(amag_ac[i] * scale);
	}

	static float    mag[MOT_WINDOW_N];
	alp_dsp_stage_t stages[] = {
		{ .kind  = ALP_DSP_STAGE_FFT,
		  .u.fft = { .n_points = MOT_WINDOW_N, .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE } },
	};
	alp_dsp_chain_t *chain = alp_dsp_chain_open(stages, 1u);
	size_t           got   = 0;
	memset(mag, 0, sizeof(mag));
	if (chain != NULL) {
		(void)alp_dsp_chain_apply_bins(chain, samp_q15, MOT_WINDOW_N, mag, MOT_WINDOW_N, &got);
		alp_dsp_chain_close(chain);
	}

	/*
	 * DOMINANT FREQUENCY BIN
	 * ----------------------
	 * The search starts at k = 1 to skip bin 0 (DC); even after mean
	 * removal a floating-point residual can linger there, and the relevant
	 * cadence content always starts at k >= 1.  Comparing magnitude
	 * directly (rather than magnitude-squared) is equivalent for an argmax
	 * -- both are monotonic in the true spectral amplitude -- so no squaring
	 * step is needed here.
	 */
	const int half    = MOT_WINDOW_N / 2;
	int       dom_bin = 1;
	float     dom_val = -1.0f;
	for (int k = 1; k < half; k++) {
		if (mag[k] > dom_val) {
			dom_val = mag[k];
			dom_bin = k;
		}
	}
	/* Convert bin index to physical frequency: f = k * sr_hz / N. */
	out->dom_freq_hz = (float)dom_bin * sr_hz / (float)MOT_WINDOW_N;
}

/* ===========================================================================
 * Feature packing  --  mot_feat_pack()
 *
 * Flattens the struct mot_features into a contiguous float array in a fixed,
 * documented order that MUST match the AI model's expected input layout:
 *
 *   indices [0..2]   a_rms[0..2]   per-axis accel AC RMS  (x, y, z)
 *   indices [3..5]   g_rms[0..2]   per-axis gyro  AC RMS  (x, y, z)
 *   index   [6]      amag_rms
 *   index   [7]      gmag_rms
 *   index   [8]      sma
 *   index   [9]      dom_freq_hz
 *   index   [10]     jerk_rms
 *   index   [11]     tilt_deg
 *
 * This explicit packing order is intentional: the model was trained with this
 * exact feature ordering, so even if the struct fields are reordered in the
 * future the model receives the correct feature in each slot.
 *
 * Returns MOT_FEATURE_DIM (= 12) on success, 0 if cap < MOT_FEATURE_DIM.
 * =========================================================================== */
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

/* ===========================================================================
 * Deterministic activity fallback classifier  --  mot_activity_fallback()
 *
 * Used when no AI model is loaded (battery save, first boot, unsupported HW).
 * Applies hand-tuned thresholds on two highly discriminative features:
 *
 *   amag_rms     -- dynamic motion level (gravity removed by AC RMS)
 *   dom_freq_hz  -- step cadence peak from the FFT
 *
 * Decision logic
 * --------------
 * 1.  amag_rms < 0.05 g  →  IDLE  (confidence 0.80)
 *     The AC component is nearly zero.  The person is sitting, standing still,
 *     sleeping, or the device is lying on a desk.  The 0.05 g threshold is
 *     above typical accelerometer noise floors (~0.01 g RMS) with margin.
 *
 * 2.  dom_freq_hz > 2.5 Hz  AND  amag_rms > 0.6 g  →  RUN  (confidence = amag_rms, capped 1.0)
 *     High cadence AND high vibration amplitude together identify running.
 *     Why both gates?
 *       - amag_rms alone: cycling over a rough road can reach 0.6 g without
 *         the leg-cadence periodicity signature.
 *       - dom_freq_hz alone: a slow metronome tap can hit 3 Hz with low energy.
 *     Together they tightly identify the heel-strike + push-off impact cycle
 *     of running.  Confidence is proportional to amag_rms because a clearer
 *     signal (higher magnitude) means less ambiguity.
 *
 * 3.  Otherwise  →  WALK  (confidence 0.70)
 *     Moving but not meeting the RUN criteria.  This bucket covers:
 *       - Normal walking  (cadence 1.5--2.5 Hz, amag_rms 0.1--0.5 g)
 *       - Brisk walk / slow jog boundary
 *       - STAIRS  (see explanation below)
 *
 * Why STAIRS always maps to WALK in the fallback
 * -----------------------------------------------
 * STAIRS is a distinct class because the trained model learns:
 *   - Subtle step-to-step asymmetry in the |a| waveform (longer push phase
 *     when lifting the body vs. stepping down on descent).
 *   - Cadence variation that differs from flat-surface walking.
 *   - Jerk pattern: stair descent has a harder heel-impact spike.
 *   - Altitude gain/loss -- which the accelerometer cannot resolve without
 *     a barometer.
 *
 * The pure-inertial fallback has none of those cues:
 *   - Stair cadence (≈ 1.2--2.0 Hz) overlaps walking cadence completely.
 *   - amag_rms on stairs overlaps the walking range.
 *
 * Silently mapping STAIRS → WALK is the correct safe default: the activity
 * is detected as "active, not running" which is accurate enough for step
 * counting and calorie estimation (stride length differs ~10%, not a
 * health-critical discrepancy).  The model comment in motion_features.h
 * documents this design choice for integrators.
 * =========================================================================== */
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

/* ===========================================================================
 * Activity name lookup  --  mot_activity_name()
 *
 * Returns a stable, NUL-terminated upper-case ASCII string for each activity
 * class.  Used for logging, telemetry records, and on-screen display labels.
 * Returns "UNKNOWN" defensively for any value outside the defined enum range.
 * =========================================================================== */
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
