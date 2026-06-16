/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software DSP fallback.  Wildcard backend at priority 0 -- picked
 * unconditionally on every SoC because no HW-DSP backend is
 * registered against the dsp class today (the V2N GD32G5 FFT/FAC
 * surfaces via wave-2's alp_adc_filter_t / alp_adc_spectrum_t in
 * <alp/adc.h>, not through this standalone class).  Future HW
 * backends at higher priority pre-empt this one transparently.
 *
 * Per-chain state (per-stage state + window samples + FFT scratch)
 * is struct dsp_be, owned by the backend and reached via
 * state->be_data; the dispatcher's handle (in dsp_ops.h) is a thin
 * wrapper.  CMSIS-DSP gating: ALP_HAS_CMSIS_DSP -> arm_fir /
 * arm_rfft_fast; otherwise portable-C convolution + radix-2 FFT.
 *
 * @par Cost: ROM varies with CMSIS-DSP linkage (8-30 KB).  RAM =
 *      sizeof(struct dsp_be) per backend slot = ~17 KB worst case
 *      (per-stage state + ALP_DSP_MAX_FFT_POINTS * 2 floats scratch
 *      + one window-sample buffer); two pool slots = ~34 KB static.
 * @par Performance: CMSIS-DSP soft-FP on M-class cores with FPU; on
 *      Cortex-M55 + Helium a future vendor backend will be ~5-10x
 *      faster.  On Cortex-M33 / native_sim this is the best portable
 *      path; for SoMs without a HW DSP block this stays production.
 *      Portable-C path (ALP_HAS_CMSIS_DSP undefined) is O(N*M) FIR /
 *      O(N) biquad / O(N log N) radix-2 -- correct but slow.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/dsp.h>
#include <alp/peripheral.h>

#include "dsp_ops.h"

#if defined(ALP_HAS_CMSIS_DSP) && (ALP_HAS_CMSIS_DSP == 1)
#include <arm_math.h>
#define ALP_DSP_USE_CMSIS 1
#else
#define ALP_DSP_USE_CMSIS 0
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ================================================================== */
/* Internal stage + backend representation                             */
/* ================================================================== */

typedef struct {
	alp_dsp_stage_kind_t kind;
	union {
		struct {
			uint16_t n_taps;
			/* Taps stored f32 regardless of caller-supplied format. */
			float taps[ALP_DSP_MAX_FIR_TAPS];
			/* Delay line: state[k] = x[n-1-k] for k=0..n_taps-1. */
			float state[ALP_DSP_MAX_FIR_TAPS];
		} fir;
		struct {
			uint16_t n_sections;
			/* coeffs[s] = { b0, b1, b2, a1, a2 } for section s. */
			float coeffs[ALP_DSP_MAX_IIR_SECTIONS][5];
			/* state[s] = { x[n-1], x[n-2], y[n-1], y[n-2] } (DF1). */
			float state[ALP_DSP_MAX_IIR_SECTIONS][4];
		} iir;
		struct {
			alp_dsp_window_kind_t shape;
		} window;
		struct {
			uint16_t             n_points;
			alp_dsp_fft_output_t output;
		} fft;
	} u;
} dsp_stage_t;

struct dsp_be {
	bool        in_use;
	bool        terminal_fft;
	uint8_t     n_stages;
	dsp_stage_t stages[ALP_DSP_MAX_STAGES];
	/* Window samples for the WINDOW stage (precomputed at open time;
     * length matches the immediately-following FFT's n_points). */
	float window_samples[ALP_DSP_MAX_FFT_POINTS];
	bool  window_ready;
	/* FFT working buffers (re / im, separate to keep the naive radix-2
     * loop straightforward).  Sized for the largest possible FFT. */
	float fft_re[ALP_DSP_MAX_FFT_POINTS];
	float fft_im[ALP_DSP_MAX_FFT_POINTS];
};

/* Static backend-data pool.  Same shape + size as the legacy
 * ALP_DSP_CHAIN_POOL_SIZE = 2; the dispatcher's pool is independent
 * but defaults to the same count so the worst-case footprint is
 * unchanged from the v0.3 build. */
#define DSP_BE_POOL_SIZE 2u
static struct dsp_be  g_be_pool[DSP_BE_POOL_SIZE];

static struct dsp_be *acquire_be_slot(void)
{
	for (size_t i = 0u; i < DSP_BE_POOL_SIZE; i++) {
		if (!g_be_pool[i].in_use) {
			return &g_be_pool[i];
		}
	}
	return NULL;
}

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

static bool is_power_of_two(uint32_t v)
{
	return (v != 0u) && ((v & (v - 1u)) == 0u);
}

static float q31_to_f32(int32_t q)
{
	/* Q31 full-scale = 2^31; map to +/-1.0. */
	return (float)q * (1.0f / 2147483648.0f);
}

/* Saturating cast from float (sample magnitude in mV-scale) to int16. */
static int16_t f32_to_i16_sat(float f)
{
	if (f > 32767.0f) return 32767;
	if (f < -32768.0f) return -32768;
	return (int16_t)lrintf(f);
}

static void compute_window_samples(alp_dsp_window_kind_t shape, uint16_t n_points, float *dst)
{
	if (n_points == 0u) return;
	if (shape == ALP_DSP_WINDOW_RECTANGULAR) {
		for (uint16_t i = 0u; i < n_points; i++) {
			dst[i] = 1.0f;
		}
		return;
	}
	const float denom = (n_points <= 1u) ? 1.0f : (float)(n_points - 1u);
	for (uint16_t i = 0u; i < n_points; i++) {
		const float theta = 2.0f * (float)M_PI * (float)i / denom;
		switch (shape) {
		case ALP_DSP_WINDOW_HANN:
			dst[i] = 0.5f * (1.0f - cosf(theta));
			break;
		case ALP_DSP_WINDOW_HAMMING:
			dst[i] = 0.54f - 0.46f * cosf(theta);
			break;
		case ALP_DSP_WINDOW_BLACKMAN:
			dst[i] = 0.42f - 0.5f * cosf(theta) + 0.08f * cosf(2.0f * theta);
			break;
		default:
			dst[i] = 1.0f; /* defensive fall-through to rectangular */
			break;
		}
	}
}

/* ================================================================== */
/* Math kernels (portable C fallback)                                  */
/* ================================================================== */

/* Run a FIR filter over @p in (length @p in_n).  Writes to @p out
 * (same length, in-place safe if out == in).  Updates @p stage state. */
static void fir_apply(dsp_stage_t *stage, const float *in, size_t in_n, float *out)
{
#if ALP_DSP_USE_CMSIS
	/* CMSIS-DSP arm_fir_f32 needs an arm_fir_instance_f32; we do
     * one-shot init here to avoid keeping CMSIS state across calls
     * when we already keep our own state in stage->fir.state. */
	arm_fir_instance_f32 inst;
	arm_fir_init_f32(&inst, stage->u.fir.n_taps, stage->u.fir.taps, stage->u.fir.state,
	                 (uint32_t)in_n);
	arm_fir_f32(&inst, (float *)in, out, (uint32_t)in_n);
#else
	const uint16_t M = stage->u.fir.n_taps;
	for (size_t n = 0u; n < in_n; n++) {
		float acc = 0.0f;
		acc += stage->u.fir.taps[0] * in[n];
		for (uint16_t k = 1u; k < M; k++) {
			const float x_prev = (n >= (size_t)k) ? in[n - k] : stage->u.fir.state[k - 1u];
			acc += stage->u.fir.taps[k] * x_prev;
		}
		out[n] = acc;
	}
	/* Save the last M-1 input samples as state for the next call. */
	const size_t save = (M > 1u) ? (size_t)(M - 1u) : 0u;
	for (size_t k = 0u; k < save; k++) {
		const size_t idx      = (in_n > k) ? (in_n - 1u - k) : 0u;
		stage->u.fir.state[k] = (in_n > k) ? in[idx] : 0.0f;
	}
#endif
}

/* Run an IIR biquad cascade over @p in (length @p in_n).  In-place
 * safe if out == in.  Updates per-section state. */
static void iir_apply(dsp_stage_t *stage, const float *in, size_t in_n, float *out)
{
	for (size_t n = 0u; n < in_n; n++) {
		float v = in[n];
		for (uint16_t s = 0u; s < stage->u.iir.n_sections; s++) {
			const float *c  = stage->u.iir.coeffs[s];
			float       *st = stage->u.iir.state[s];
			const float  y  = c[0] * v + c[1] * st[0] + c[2] * st[1] - c[3] * st[2] - c[4] * st[3];
			/* Shift state for next sample. */
			st[1] = st[0];
			st[0] = v;
			st[3] = st[2];
			st[2] = y;
			v     = y;
		}
		out[n] = v;
	}
}

/* Portable in-place radix-2 Cooley-Tukey FFT.  N MUST be a
 * power-of-two; chain validation already enforces this.
 *
 * Real input -> complex output: caller fills @p re from samples and
 * sets @p im to zero. */
#if !ALP_DSP_USE_CMSIS
static void fft_radix2_inplace(float *re, float *im, uint16_t n)
{
	/* Bit-reversal permutation. */
	uint16_t j = 0u;
	for (uint16_t i = 1u; i < n; i++) {
		uint16_t bit = n >> 1u;
		for (; j & bit; bit >>= 1u) {
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
	/* Butterfly stages. */
	for (uint16_t len = 2u; len <= n; len <<= 1u) {
		const float ang     = -2.0f * (float)M_PI / (float)len;
		const float wlen_re = cosf(ang);
		const float wlen_im = sinf(ang);
		for (uint16_t i = 0u; i < n; i += len) {
			float          w_re = 1.0f;
			float          w_im = 0.0f;
			const uint16_t half = len >> 1u;
			for (uint16_t k = 0u; k < half; k++) {
				const uint16_t a_idx = (uint16_t)(i + k);
				const uint16_t b_idx = (uint16_t)(a_idx + half);
				const float    t_re  = w_re * re[b_idx] - w_im * im[b_idx];
				const float    t_im  = w_re * im[b_idx] + w_im * re[b_idx];
				re[b_idx]            = re[a_idx] - t_re;
				im[b_idx]            = im[a_idx] - t_im;
				re[a_idx]            = re[a_idx] + t_re;
				im[a_idx]            = im[a_idx] + t_im;
				const float nw_re    = w_re * wlen_re - w_im * wlen_im;
				const float nw_im    = w_re * wlen_im + w_im * wlen_re;
				w_re                 = nw_re;
				w_im                 = nw_im;
			}
		}
	}
}
#endif

/* ================================================================== */
/* Stage validation helpers                                            */
/* ================================================================== */

static alp_status_t copy_fir_taps(const alp_dsp_fir_params_t *src, dsp_stage_t *dst)
{
	if (src->n_taps == 0u || src->n_taps > ALP_DSP_MAX_FIR_TAPS) {
		return ALP_ERR_OUT_OF_RANGE;
	}
	if (src->taps == NULL) {
		return ALP_ERR_INVAL;
	}
	dst->u.fir.n_taps = src->n_taps;
	if (src->coeff_format == ALP_DSP_COEFF_FORMAT_F32) {
		const float *p = (const float *)src->taps;
		for (uint16_t i = 0u; i < src->n_taps; i++) {
			dst->u.fir.taps[i] = p[i];
		}
	} else if (src->coeff_format == ALP_DSP_COEFF_FORMAT_Q31) {
		const int32_t *p = (const int32_t *)src->taps;
		for (uint16_t i = 0u; i < src->n_taps; i++) {
			dst->u.fir.taps[i] = q31_to_f32(p[i]);
		}
	} else {
		return ALP_ERR_INVAL;
	}
	/* Zero the state. */
	for (uint16_t i = 0u; i < ALP_DSP_MAX_FIR_TAPS; i++) {
		dst->u.fir.state[i] = 0.0f;
	}
	return ALP_OK;
}

static alp_status_t copy_iir_coeffs(const alp_dsp_iir_params_t *src, dsp_stage_t *dst)
{
	if (src->n_sections == 0u || src->n_sections > ALP_DSP_MAX_IIR_SECTIONS) {
		return ALP_ERR_OUT_OF_RANGE;
	}
	if (src->coeffs == NULL) {
		return ALP_ERR_INVAL;
	}
	dst->u.iir.n_sections = src->n_sections;
	if (src->coeff_format == ALP_DSP_COEFF_FORMAT_F32) {
		const float *p = (const float *)src->coeffs;
		for (uint16_t s = 0u; s < src->n_sections; s++) {
			for (uint8_t c = 0u; c < 5u; c++) {
				dst->u.iir.coeffs[s][c] = p[s * 5u + c];
			}
		}
	} else if (src->coeff_format == ALP_DSP_COEFF_FORMAT_Q31) {
		const int32_t *p = (const int32_t *)src->coeffs;
		for (uint16_t s = 0u; s < src->n_sections; s++) {
			for (uint8_t c = 0u; c < 5u; c++) {
				dst->u.iir.coeffs[s][c] = q31_to_f32(p[s * 5u + c]);
			}
		}
	} else {
		return ALP_ERR_INVAL;
	}
	for (uint16_t s = 0u; s < ALP_DSP_MAX_IIR_SECTIONS; s++) {
		for (uint8_t k = 0u; k < 4u; k++) {
			dst->u.iir.state[s][k] = 0.0f;
		}
	}
	return ALP_OK;
}

static alp_status_t validate_fft(const alp_dsp_fft_params_t *src, dsp_stage_t *dst)
{
	if (src->n_points < ALP_DSP_MIN_FFT_POINTS || src->n_points > ALP_DSP_MAX_FFT_POINTS) {
		return ALP_ERR_OUT_OF_RANGE;
	}
	if (!is_power_of_two((uint32_t)src->n_points)) {
		return ALP_ERR_OUT_OF_RANGE;
	}
	if (src->output_format != ALP_DSP_FFT_OUTPUT_COMPLEX &&
	    src->output_format != ALP_DSP_FFT_OUTPUT_MAGNITUDE) {
		return ALP_ERR_INVAL;
	}
	dst->u.fft.n_points = src->n_points;
	dst->u.fft.output   = src->output_format;
	return ALP_OK;
}

static alp_status_t validate_window(const alp_dsp_window_params_t *src, dsp_stage_t *dst)
{
	switch (src->shape) {
	case ALP_DSP_WINDOW_RECTANGULAR:
	case ALP_DSP_WINDOW_HANN:
	case ALP_DSP_WINDOW_HAMMING:
	case ALP_DSP_WINDOW_BLACKMAN:
		dst->u.window.shape = src->shape;
		return ALP_OK;
	default:
		return ALP_ERR_INVAL;
	}
}

/* ================================================================== */
/* Ops                                                                 */
/* ================================================================== */

static alp_status_t sw_open(const alp_dsp_stage_t *stages, size_t n_stages,
                            alp_dsp_backend_state_t *state, alp_capabilities_t *caps_out)
{
	if (n_stages > ALP_DSP_MAX_STAGES) {
		return ALP_ERR_INVAL;
	}

	/* Structural ordering check first: at most one FFT (terminal);
     * WINDOW only valid immediately preceding FFT. */
	int fft_idx = -1;
	for (size_t i = 0u; i < n_stages; i++) {
		if (stages[i].kind == ALP_DSP_STAGE_FFT) {
			if (fft_idx >= 0) {
				return ALP_ERR_INVAL;
			}
			fft_idx = (int)i;
		}
	}
	if (fft_idx >= 0 && fft_idx != (int)(n_stages - 1u)) {
		return ALP_ERR_INVAL;
	}
	for (size_t i = 0u; i < n_stages; i++) {
		if (stages[i].kind == ALP_DSP_STAGE_WINDOW) {
			if (fft_idx < 0 || (int)i != fft_idx - 1) {
				return ALP_ERR_INVAL;
			}
		}
	}

	struct dsp_be *be = acquire_be_slot();
	if (be == NULL) {
		return ALP_ERR_NOMEM;
	}

	/* Zero the slot fully so we don't carry over state from a prior
     * open() / close() cycle. */
	memset(be, 0, sizeof(*be));
	be->n_stages     = (uint8_t)n_stages;
	be->terminal_fft = (fft_idx >= 0);

	for (size_t i = 0u; i < n_stages; i++) {
		dsp_stage_t *dst = &be->stages[i];
		dst->kind        = stages[i].kind;
		alp_status_t s   = ALP_OK;
		switch (stages[i].kind) {
		case ALP_DSP_STAGE_FIR:
			s = copy_fir_taps(&stages[i].u.fir, dst);
			break;
		case ALP_DSP_STAGE_IIR:
			s = copy_iir_coeffs(&stages[i].u.iir, dst);
			break;
		case ALP_DSP_STAGE_WINDOW:
			s = validate_window(&stages[i].u.window, dst);
			break;
		case ALP_DSP_STAGE_FFT:
			s = validate_fft(&stages[i].u.fft, dst);
			break;
		default:
			s = ALP_ERR_INVAL;
			break;
		}
		if (s != ALP_OK) {
			be->in_use = false; /* hand the slot back */
			return s;
		}
	}

	/* Precompute window samples if WINDOW stage present (it precedes
     * FFT and uses FFT's n_points). */
	if (be->terminal_fft && n_stages >= 2u &&
	    be->stages[n_stages - 2u].kind == ALP_DSP_STAGE_WINDOW) {
		const uint16_t n = be->stages[n_stages - 1u].u.fft.n_points;
		compute_window_samples(be->stages[n_stages - 2u].u.window.shape, n, be->window_samples);
		be->window_ready = true;
	}

	be->in_use     = true;
	state->be_data = be;
	if (caps_out != NULL) {
		caps_out->flags = 0u;
	}
	return ALP_OK;
}

static alp_status_t sw_apply_samples(alp_dsp_backend_state_t *state, const int16_t *in_mv,
                                     size_t in_n, int16_t *out_mv, size_t out_cap, size_t *got)
{
	struct dsp_be *be = (struct dsp_be *)state->be_data;
	if (be == NULL || !be->in_use) {
		return ALP_ERR_INVAL;
	}
	if (be->terminal_fft) {
		return ALP_ERR_NOSUPPORT;
	}
	if ((in_mv == NULL && in_n > 0u) || (out_mv == NULL && out_cap > 0u)) {
		return ALP_ERR_INVAL;
	}
	const size_t n = (in_n < out_cap) ? in_n : out_cap;
	*got           = 0u;
	if (n == 0u) return ALP_OK;

	/* Walk through the buffer in chunks bounded by the FFT scratch
     * size, even though there's no FFT here -- reusing the same
     * scratch keeps the working set bounded. */
	const size_t chunk_max = ALP_DSP_MAX_FFT_POINTS;
	size_t       produced  = 0u;
	while (produced < n) {
		const size_t this_chunk = ((n - produced) < chunk_max) ? (n - produced) : chunk_max;
		for (size_t i = 0u; i < this_chunk; i++) {
			be->fft_re[i] = (float)in_mv[produced + i];
		}
		float *buf = be->fft_re;
		for (uint8_t st = 0u; st < be->n_stages; st++) {
			dsp_stage_t *s = &be->stages[st];
			switch (s->kind) {
			case ALP_DSP_STAGE_FIR:
				fir_apply(s, buf, this_chunk, buf);
				break;
			case ALP_DSP_STAGE_IIR:
				iir_apply(s, buf, this_chunk, buf);
				break;
			default:
				/* WINDOW / FFT cannot appear in a filter-terminated chain
                 * after the structural check; defensive fall-through. */
				break;
			}
		}
		for (size_t i = 0u; i < this_chunk; i++) {
			out_mv[produced + i] = f32_to_i16_sat(buf[i]);
		}
		produced += this_chunk;
	}
	*got = produced;
	return ALP_OK;
}

static alp_status_t sw_apply_bins(alp_dsp_backend_state_t *state, const int16_t *in_mv, size_t in_n,
                                  float *out_bins, size_t out_cap, size_t *got)
{
	struct dsp_be *be = (struct dsp_be *)state->be_data;
	if (be == NULL || !be->in_use) {
		return ALP_ERR_INVAL;
	}
	if (!be->terminal_fft) {
		return ALP_ERR_NOSUPPORT;
	}
	if (in_mv == NULL || out_bins == NULL) {
		return ALP_ERR_INVAL;
	}

	const dsp_stage_t *fft = &be->stages[be->n_stages - 1u];
	const uint16_t     n   = fft->u.fft.n_points;
	if (in_n < (size_t)n) {
		return ALP_ERR_OUT_OF_RANGE;
	}
	const size_t required =
	    (fft->u.fft.output == ALP_DSP_FFT_OUTPUT_COMPLEX) ? (size_t)(2u * n) : (size_t)n;
	if (out_cap < required) {
		return ALP_ERR_OUT_OF_RANGE;
	}

	/* Stage 0: load samples (mV -> float). */
	for (uint16_t i = 0u; i < n; i++) {
		be->fft_re[i] = (float)in_mv[i];
		be->fft_im[i] = 0.0f;
	}

	/* Run pre-FFT non-WINDOW stages over fft_re (in-place). */
	for (uint8_t st = 0u; st < (be->n_stages - 1u); st++) {
		dsp_stage_t *s = &be->stages[st];
		switch (s->kind) {
		case ALP_DSP_STAGE_FIR:
			fir_apply(s, be->fft_re, n, be->fft_re);
			break;
		case ALP_DSP_STAGE_IIR:
			iir_apply(s, be->fft_re, n, be->fft_re);
			break;
		case ALP_DSP_STAGE_WINDOW:
			if (!be->window_ready) {
				return ALP_ERR_NOT_READY;
			}
			for (uint16_t i = 0u; i < n; i++) {
				be->fft_re[i] *= be->window_samples[i];
			}
			break;
		default:
			return ALP_ERR_INVAL; /* unexpected */
		}
	}

#if ALP_DSP_USE_CMSIS
	{
		/* CMSIS-DSP real FFT: arm_rfft_fast_f32 produces N/2+1 complex
         * bins packed into N floats with DC and Nyquist real-valued
         * in slots 0 and 1.  Unpack into our full N-bin layout for
         * symmetric output. */
		arm_rfft_fast_instance_f32 inst;
		if (arm_rfft_fast_init_f32(&inst, n) != ARM_MATH_SUCCESS) {
			return ALP_ERR_IO;
		}
		float scratch[ALP_DSP_MAX_FFT_POINTS];
		arm_rfft_fast_f32(&inst, be->fft_re, scratch, 0u);
		/* Re-spread CMSIS's packed layout into our (re, im) arrays. */
		be->fft_re[0]      = scratch[0]; /* DC re   */
		be->fft_im[0]      = 0.0f;       /* DC im=0 */
		be->fft_re[n / 2u] = scratch[1]; /* Nyquist */
		be->fft_im[n / 2u] = 0.0f;
		for (uint16_t k = 1u; k < n / 2u; k++) {
			be->fft_re[k] = scratch[2u * k];
			be->fft_im[k] = scratch[2u * k + 1u];
			/* Mirror conjugate for k = n - bin. */
			be->fft_re[n - k] = be->fft_re[k];
			be->fft_im[n - k] = -be->fft_im[k];
		}
	}
#else
	fft_radix2_inplace(be->fft_re, be->fft_im, n);
#endif

	if (fft->u.fft.output == ALP_DSP_FFT_OUTPUT_COMPLEX) {
		for (uint16_t k = 0u; k < n; k++) {
			out_bins[2u * k]      = be->fft_re[k];
			out_bins[2u * k + 1u] = be->fft_im[k];
		}
		*got = (size_t)(2u * n);
	} else {
		for (uint16_t k = 0u; k < n; k++) {
			const float re = be->fft_re[k];
			const float im = be->fft_im[k];
			out_bins[k]    = sqrtf(re * re + im * im);
		}
		*got = (size_t)n;
	}
	return ALP_OK;
}

static void sw_close(alp_dsp_backend_state_t *state)
{
	struct dsp_be *be = (struct dsp_be *)state->be_data;
	if (be == NULL) return;
	be->in_use       = false;
	be->n_stages     = 0u;
	be->terminal_fft = false;
	be->window_ready = false;
	state->be_data   = NULL;
}

/* ---------- Registration ---------- */

static const alp_dsp_ops_t _ops = {
	.open          = sw_open,
	.apply_samples = sw_apply_samples,
	.apply_bins    = sw_apply_bins,
	.close         = sw_close,
};

ALP_BACKEND_REGISTER(dsp, sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_fallback",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
