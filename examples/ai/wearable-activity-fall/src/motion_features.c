/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * motion_features implementation -- see motion_features.h.
 *
 * This file implements the windowed IMU feature extraction pipeline used by the
 * wearable activity-recognition + fall-detection example.  It is intentionally
 * self-contained (stdint/math only) so it builds identically for native_sim and
 * Cortex-M55.  Read this file alongside motion_features.h.
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
 */
#include "motion_features.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
 * Radix-2 Cooley-Tukey DIT FFT  (in-place, iterative)
 *
 * Mathematical background
 * -----------------------
 * The Discrete Fourier Transform of a length-N complex sequence x[n] is:
 *
 *   X[k] = sum_{n=0}^{N-1}  x[n] * W_N^{nk},   k = 0, 1, ..., N-1
 *
 * where  W_N = e^{-j*2*pi/N}  is the primitive N-th root of unity ("twiddle
 * base").  Naive evaluation costs O(N^2) complex multiplications.
 *
 * Cooley-Tukey factorisation (radix-2)
 * -------------------------------------
 * When N = 2^M, the DFT can be split recursively into two DFTs of length N/2
 * over the even-indexed and odd-indexed sub-sequences:
 *
 *   X[k]       = E[k] + W_N^k  * O[k]          (k = 0 .. N/2-1)
 *   X[k + N/2] = E[k] - W_N^k  * O[k]
 *
 * where E = DFT of x[0], x[2], x[4], ...  and  O = DFT of x[1], x[3], ...
 *
 * Applying this recursively gives O(N log2 N) multiplications.
 *
 * Iterative ("bottom-up") implementation
 * ---------------------------------------
 * Rather than actual recursion (which allocates log2 N stack frames), the
 * iterative form:
 *   1. Pre-permutes the input into bit-reversed order  (Phase 1).
 *   2. Merges sub-DFTs of length 2, 4, 8, ..., N with "butterfly" operations
 *      (Phase 2, log2 N passes).
 *
 * --------------------------------------------------------------------------
 * Phase 1: bit-reversal permutation
 * --------------------------------------------------------------------------
 * At the base of the recursion, sample x[n] ends up at position rev(n),
 * where rev(n) is the binary representation of n with its bits reversed.
 *
 * Example for N = 8 (3-bit indices):
 *   n = 0 (000) → rev = 000 = 0  (stays)
 *   n = 1 (001) → rev = 100 = 4  (swap 1 ↔ 4)
 *   n = 2 (010) → rev = 010 = 2  (stays)
 *   n = 3 (011) → rev = 110 = 6  (swap 3 ↔ 6)
 *   n = 4 (100) → rev = 001 = 1  (already swapped)
 *   ...
 *
 * The in-place swap loop uses the standard j-tracking trick:
 *   - j tracks the bit-reversed position of the current index i.
 *   - For each increment of i (in binary), the bit-reversed j is advanced by
 *     flipping bits from the most-significant bit downward using XOR, stopping
 *     when there is no carry.  This is equivalent to incrementing a counter
 *     whose bits run in reverse order.
 *   - Only swap when i < j so each pair is exchanged exactly once.
 *
 * --------------------------------------------------------------------------
 * Phase 2: butterfly stages  (len = 2, 4, 8, ..., N)
 * --------------------------------------------------------------------------
 * At stage s (sub-DFT length = len = 2^s) the data array is partitioned
 * into N/len blocks of length len.  Inside each block, a "butterfly" merges
 * the upper half (indices 0..len/2-1, call it A) and the lower half
 * (indices len/2..len-1, call it B) using the DIT combine formula:
 *
 *   U[k]          =  A[k]  +  W_len^k  *  B[k]
 *   U[k + len/2]  =  A[k]  -  W_len^k  *  B[k]
 *
 * where W_len^k = e^{-j*2*pi*k/len} is the twiddle factor for bin k.
 *
 * --------------------------------------------------------------------------
 * Twiddle factor recurrence  (avoids per-butterfly trigonometric calls)
 * --------------------------------------------------------------------------
 * Instead of calling cosf/sinf for every k, the code seeds W_0 = 1 + 0j
 * and advances iteratively:
 *
 *   W_{k+1} = W_k  *  W_1
 *
 * where  W_1 = e^{-j*2*pi/len} = (wlr, wli)  is computed once per stage
 * from ang = -2*pi/len.
 *
 * Complex multiply:
 *   nwr = wr*wlr - wi*wli        (real part)
 *   wi  = wr*wli + wi*wlr        (imag part, using old wr)
 *   wr  = nwr
 *
 * This costs 4 multiplications + 2 additions per butterfly step (cheaper
 * than one sinf/cosf) and stays on the unit circle to avoid magnitude drift.
 *
 * N must be a power of two.  Caller guarantees MOT_WINDOW_N = 256 = 2^8.
 * =========================================================================== */
static void fft_radix2(float *re, float *im, int n)
{
	/* ---- Phase 1: bit-reversal permutation -------------------------------- */
	for (int i = 1, j = 0; i < n; i++) {
		/* Advance j to the bit-reversed position of i.
		 * Peel carry bits from the MSB downward using XOR until no carry. */
		int bit = n >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		/* Swap the complex sample at position i with position j.
		 * The guard (i < j) ensures each pair is swapped at most once. */
		if (i < j) {
			float tr = re[i];
			re[i]    = re[j];
			re[j]    = tr;
			float ti = im[i];
			im[i]    = im[j];
			im[j]    = ti;
		}
	}

	/* ---- Phase 2: butterfly merge stages  (len doubles each pass) --------- */
	for (int len = 2; len <= n; len <<= 1) {
		/* Base twiddle for this stage: W_1 = e^{-j*2*pi/len}. */
		float ang = -2.0f * (float)M_PI / (float)len;
		float wlr = cosf(ang); /* real part of stage twiddle base */
		float wli = sinf(ang); /* imag part of stage twiddle base */

		/* Process every length-len sub-block starting at offset i. */
		for (int i = 0; i < n; i += len) {
			/* Start with W_0 = 1 + 0j; advance via recurrence each step. */
			float wr = 1.0f, wi = 0.0f;

			for (int k = 0; k < len / 2; k++) {
				int a = i + k;           /* index in the "A" (upper) half */
				int b = i + k + len / 2; /* index in the "B" (lower) half */

				/* Multiply B[k] by the current twiddle factor W_k. */
				float tr = wr * re[b] - wi * im[b];
				float ti = wr * im[b] + wi * re[b];

				/* DIT butterfly: U = A + W*B,  L = A - W*B. */
				re[b] = re[a] - tr;
				im[b] = im[a] - ti;
				re[a] += tr;
				im[a] += ti;

				/* Advance twiddle: W_{k+1} = W_k * W_1 (complex multiply). */
				float nwr = wr * wlr - wi * wli;
				wi        = wr * wli + wi * wlr;
				wr        = nwr;
			}
		}
	}
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
 * Dividing by N normalises across partial and full windows.
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
 * wrist → tilt ≈ 90°.  The AI model uses tilt as a weak posture cue.
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
 * Procedure:
 *   1. Build the |a| time series (amag[]), already computed in pass A.
 *   2. Subtract the mean (mean_amag) to suppress the DC bin in the FFT.
 *   3. Zero-pad or truncate to MOT_WINDOW_N = 256 samples.
 *   4. Run the radix-2 FFT on the real-valued series (imag = 0).
 *   5. Search bins k = 1 .. N/2-1 for the peak of re[k]^2 + im[k]^2.
 *   6. Convert winning bin k to Hz:  f = k * sr_hz / N.
 *
 * Frequency resolution:  delta_f = sr_hz / N = 100 / 256 ≈ 0.39 Hz/bin.
 * Nyquist limit:         f_max   = sr_hz / 2 = 50 Hz  (bin N/2-1 = 127).
 * Bin 0 is DC (suppressed by mean removal, explicitly skipped).
 * Negative-frequency bins (k >= N/2) mirror the positive bins for real input;
 * only bins 1..N/2-1 carry independent spectral information.
 *
 * Using magnitude-squared (re[k]^2 + im[k]^2) for the peak search avoids
 * N/2 sqrtf calls; argmax is the same as for magnitude.
 * =========================================================================== */
void mot_feat_extract(const struct mot_window_state *st, float sr_hz, struct mot_features *out)
{
	const int n = (st->count < MOT_WINDOW_N) ? st->count : MOT_WINDOW_N;

	memset(out, 0, sizeof(*out));
	if (n <= 0) {
		return;
	}

	/* ---- Pass A: per-axis means, |a| series, SMA -------------------------- */
	/*
	 * mean_a[k]  : sum of axis k accel samples (divided below → DC mean).
	 *              Approximates the gravity projection onto axis k when motion
	 *              is not dominated by rapid dynamics.
	 * amag[i]    : Euclidean magnitude of sample i = sqrt(ax^2+ay^2+az^2) [g].
	 * mean_amag  : mean of amag[], subtracted before the FFT (DC removal).
	 * sma        : accumulator for the SMA numerator (sum of L1 norms).
	 */
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
		/* SMA numerator: L1 norm of the raw accel vector (gravity included). */
		sma += fabsf(s->ax) + fabsf(s->ay) + fabsf(s->az);
	}
	for (int k = 0; k < 3; k++) {
		mean_a[k] /= (float)n;
		mean_g[k] /= (float)n;
	}
	mean_amag /= (float)n;
	/* Normalise SMA to a per-sample mean (window-length independent). */
	out->sma = sma / (float)n;

	/* ---- Pass B: axis AC RMS for accel + gyro, |a| AC RMS, |gyro| mean --- */
	/*
	 * da[k] = s->a[k] - mean_a[k]  : AC component of accel axis k.
	 * dg[k] = s->g[k] - mean_g[k]  : AC component of gyro  axis k.
	 * sa[k]  : sum of da[k]^2 → after division, variance of axis k accel.
	 * sg[k]  : sum of dg[k]^2 → variance of axis k gyro.
	 * dm     : AC component of the |a| magnitude series (amag[i] - mean_amag).
	 * s_amag : sum of dm^2 → variance of the magnitude signal (for amag_rms).
	 * mean_gmag : mean of |gyro| magnitude; the |gyro| AC RMS is a two-pass
	 *             computation (need mean before computing deviations) so it is
	 *             done in pass B (mean) + pass C (RMS).
	 */
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
		/* |gyro| = L2 norm of the angular-rate vector.  Accumulate the mean
		 * so pass C can subtract it for the AC (dynamic rotation) component. */
		float gm = sqrtf(s->gx * s->gx + s->gy * s->gy + s->gz * s->gz);
		mean_gmag += gm;
	}
	mean_gmag /= (float)n;

	/* ---- Pass C: |gyro| AC RMS  (two-pass: mean known from pass B) -------- */
	for (int i = 0; i < n; i++) {
		const struct mot_sample *s = &st->s[i];
		/* Subtract the DC mean to isolate the dynamic rotation component. */
		float gm = sqrtf(s->gx * s->gx + s->gy * s->gy + s->gz * s->gz) - mean_gmag;
		s_gmag += gm * gm;
	}

	/* Finalise AC RMS from accumulated sum-of-squares: sqrt( S / N ). */
	for (int k = 0; k < 3; k++) {
		out->a_rms[k] = sqrtf(sa[k] / (float)n);
		out->g_rms[k] = sqrtf(sg[k] / (float)n);
	}
	out->amag_rms = sqrtf(s_amag / (float)n);
	out->gmag_rms = sqrtf(s_gmag / (float)n);

	/* ---- Jerk RMS --------------------------------------------------------- */
	/*
	 * First backward difference of the |a| series, scaled to g/s.
	 * Summed over N-1 pairs; the first sample has no predecessor.
	 */
	float s_jerk = 0.0f;
	for (int i = 1; i < n; i++) {
		float d = (amag[i] - amag[i - 1]) * sr_hz; /* per-second jerk */
		s_jerk += d * d;
	}
	out->jerk_rms = (n > 1) ? sqrtf(s_jerk / (float)(n - 1)) : 0.0f;

	/* ---- Tilt of the mean accel vector from vertical (Z) ------------------ */
	/*
	 * The horizontal component of the mean gravity vector is the L2 norm of
	 * (mean_ax, mean_ay).  atan2 gives the angle from the Z-axis in [0, pi].
	 * For Z-up mounting: tilt ≈ 0° when flat, ≈ 90° when upright.
	 */
	out->tilt_deg = atan2f(sqrtf(mean_a[0] * mean_a[0] + mean_a[1] * mean_a[1]), mean_a[2]) *
	                180.0f / (float)M_PI;

	/* ---- Dominant frequency of the |a| envelope via FFT (DC removed) ----- */
	/*
	 * Load the AC accel-magnitude series into the FFT input buffers.
	 * Samples beyond the live count (i >= n) are zero-padded; with a full
	 * window (n == MOT_WINDOW_N = 256) there is no padding.
	 * Imaginary part is zero because the input signal is real-valued.
	 */
	static float re[MOT_WINDOW_N];
	static float im[MOT_WINDOW_N];
	for (int i = 0; i < MOT_WINDOW_N; i++) {
		re[i] = (i < n) ? (amag[i] - mean_amag) : 0.0f;
		im[i] = 0.0f;
	}
	fft_radix2(re, im, MOT_WINDOW_N);

	/*
	 * Search positive-frequency bins 1 .. N/2-1 for the peak spectral power.
	 * Start dom_val at -1 so the first bin (k = 1) always wins the first
	 * comparison even if its power is zero, giving a deterministic result on
	 * an all-zeros input.
	 */
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
