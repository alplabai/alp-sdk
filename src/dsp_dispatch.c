/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * DSP class dispatcher.  Owns the public <alp/dsp.h> surface -- one
 * stateful handle type (alp_dsp_chain_t) carrying a composable
 * FIR / IIR / window / FFT pipeline -- on top of the backend registry.
 *
 * Dispatch shape mirrors the rtc / audio siblings: each open()
 * resolves the backend, allocates from a static pool, stores the
 * ops pointer on the handle's state struct, and lets the per-sample
 * apply ops dispatch through state.ops directly.  Capability
 * getters return per-handle cached snapshots the registry produced
 * at open() time.
 *
 * DSP is structurally unusual: the math kernels are libm +
 * CMSIS-DSP only -- OS-agnostic -- so the body ships as the
 * sw_fallback backend, and no separate zephyr_drv backend exists
 * today (V2N's HW-FFT bridge surface lands via wave-2's
 * alp_adc_filter_t / alp_adc_spectrum_t composition in <alp/adc.h>,
 * not through this class).  The dispatcher accepts a future HW
 * backend without change.
 *
 * Pool default: 2 chains (CONFIG_ALP_SDK_MAX_DSP_HANDLES).  Bumping
 * the cap costs ~9 KB per slot (FFT scratch + window samples +
 * per-stage state) so apps that need more should size the Kconfig
 * deliberately.
 *
 * last_error stamping reuses the existing TLS slot via extern
 * forward decls so the dispatcher does not pull in handles.h.  No
 * probe() is invoked -- base_caps are sufficient until a
 * SoC-specific backend with refined caps lands.
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
#include <alp/soc_caps.h>

#include "alp_slot_claim.h"
#include "backends/dsp/dsp_ops.h"
#include "dsp_range.h"

/*
 * alp_dsp_stats_f32 uses CMSIS-DSP arm_*_f32 statistics kernels when
 * the cmsis-dsp module is linked (ALP_HAS_CMSIS_DSP=1, wired from
 * CONFIG_CMSIS_DSP in the Zephyr build / the option() in plain CMake);
 * otherwise a single portable-C pass.  Same gate the sw_fallback DSP
 * backend uses, so the whole DSP surface picks one implementation.
 */
#if defined(ALP_HAS_CMSIS_DSP) && (ALP_HAS_CMSIS_DSP == 1)
#include <arm_math.h>
#define ALP_DSP_STATS_USE_CMSIS 1
#else
#define ALP_DSP_STATS_USE_CMSIS 0
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

ALP_BACKEND_DEFINE_CLASS(dsp);
ALP_BACKEND_ANCHOR(dsp);

#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_MAX_DSP_HANDLES
#define CONFIG_ALP_SDK_MAX_DSP_HANDLES 2
#endif

static struct alp_dsp_chain _pool[CONFIG_ALP_SDK_MAX_DSP_HANDLES];

static struct alp_dsp_chain *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_DSP_HANDLES; ++i) {
		/* Atomic claim: only the winner of the flag flip may touch the
		 * slot's other fields (in_use is the struct's last member, so
		 * zero everything before it -- incl. lifecycle/active_ops,
		 * parking a fresh slot at LC_UNOPENED). Issue #629. */
		if (alp_slot_try_claim(&_pool[i].in_use)) {
			memset(&_pool[i], 0, offsetof(struct alp_dsp_chain, in_use));
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_dsp_chain *h)
{
	alp_slot_release(&h->in_use);
}

alp_dsp_chain_t *alp_dsp_chain_open(const alp_dsp_stage_t *stages, size_t n_stages)
{
	alp_z_clear_last_error();
	if (stages == NULL || n_stages == 0u) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("dsp", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_dsp_ops_t *ops = (const alp_dsp_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_dsp_chain *h = _alloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	alp_capabilities_t caps = { .flags = be->base_caps };
	alp_status_t       rc   = ops->open(stages, n_stages, &h->state, &caps);
	if (rc != ALP_OK) {
		_free(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN); /* #629 */
	return h;
}

alp_status_t alp_dsp_chain_apply_samples(alp_dsp_chain_t *chain,
                                         const int16_t   *in_mv,
                                         size_t           in_n,
                                         int16_t         *out_mv,
                                         size_t           out_cap,
                                         size_t          *got)
{
	if (got == NULL) {
		return ALP_ERR_INVAL;
	}
	/* Gate on the lifecycle byte, not a plain in_use read: in_use is
	 * claimed/released atomically in _alloc/_free, so mixing it with a
	 * plain read here is a data race, and a racing close could free the
	 * slot mid-op. op_enter counts this op in; begin_close drains it. #629 */
	if (chain == NULL || !alp_handle_op_enter(&chain->lifecycle, &chain->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	if (chain->state.ops == NULL || chain->state.ops->apply_samples == NULL) {
		alp_handle_op_leave(&chain->active_ops);
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	alp_status_t rc =
	    chain->state.ops->apply_samples(&chain->state, in_mv, in_n, out_mv, out_cap, got);
	alp_handle_op_leave(&chain->active_ops);
	return rc;
}

alp_status_t alp_dsp_chain_apply_bins(alp_dsp_chain_t *chain,
                                      const int16_t   *in_mv,
                                      size_t           in_n,
                                      float           *out_bins,
                                      size_t           out_cap,
                                      size_t          *got)
{
	if (got == NULL) {
		return ALP_ERR_INVAL;
	}
	if (chain == NULL || !alp_handle_op_enter(&chain->lifecycle, &chain->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	if (chain->state.ops == NULL || chain->state.ops->apply_bins == NULL) {
		alp_handle_op_leave(&chain->active_ops);
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	alp_status_t rc =
	    chain->state.ops->apply_bins(&chain->state, in_mv, in_n, out_bins, out_cap, got);
	alp_handle_op_leave(&chain->active_ops);
	return rc;
}

alp_status_t alp_dsp_chain_apply_samples_f32(alp_dsp_chain_t *chain,
                                             const float     *in,
                                             size_t           in_n,
                                             float           *out,
                                             size_t           out_cap,
                                             size_t          *got)
{
	if (got == NULL) {
		return ALP_ERR_INVAL;
	}
	if (chain == NULL || !alp_handle_op_enter(&chain->lifecycle, &chain->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	if (chain->state.ops == NULL || chain->state.ops->apply_samples_f32 == NULL) {
		alp_handle_op_leave(&chain->active_ops);
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	alp_status_t rc =
	    chain->state.ops->apply_samples_f32(&chain->state, in, in_n, out, out_cap, got);
	alp_handle_op_leave(&chain->active_ops);
	return rc;
}

alp_status_t alp_dsp_chain_apply_bins_f32(alp_dsp_chain_t *chain,
                                          const float     *in,
                                          size_t           in_n,
                                          float           *out_bins,
                                          size_t           out_cap,
                                          size_t          *got)
{
	if (got == NULL) {
		return ALP_ERR_INVAL;
	}
	if (chain == NULL || !alp_handle_op_enter(&chain->lifecycle, &chain->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	if (chain->state.ops == NULL || chain->state.ops->apply_bins_f32 == NULL) {
		alp_handle_op_leave(&chain->active_ops);
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	alp_status_t rc =
	    chain->state.ops->apply_bins_f32(&chain->state, in, in_n, out_bins, out_cap, got);
	alp_handle_op_leave(&chain->active_ops);
	return rc;
}

void alp_dsp_chain_close(alp_dsp_chain_t *chain)
{
	if (chain == NULL) {
		return;
	}
	/* begin_close CAS OPEN->CLOSING then spins until every op that
	 * entered before the CAS has left -- so teardown never races an
	 * in-flight op. Idempotent: a second/never-opened close no-ops. #629 */
	if (!alp_handle_begin_close_blocking(&chain->lifecycle, &chain->active_ops)) {
		return;
	}
	if (chain->state.ops != NULL && chain->state.ops->close != NULL) {
		chain->state.ops->close(&chain->state);
	}
	alp_lifecycle_set(&chain->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free(chain);
}

const alp_capabilities_t *alp_dsp_chain_capabilities(const alp_dsp_chain_t *chain)
{
	return (chain != NULL) ? &chain->cached_caps : NULL;
}

alp_status_t alp_dsp_stats_f32(const float *x, size_t n, alp_dsp_stats_t *out)
{
	if (x == NULL || out == NULL || n == 0u) {
		return ALP_ERR_INVAL;
	}

#if ALP_DSP_STATS_USE_CMSIS
	/* CMSIS-DSP statistics kernels (Helium-vectorised on M-class cores).
	 * Their block size is uint32_t; a size_t wider than that would be
	 * SILENTLY TRUNCATED, so CMSIS would process only a prefix while we
	 * returned success (#734).  Reject the out-of-range length up front and
	 * narrow ONCE for reuse across the kernels. */
	uint32_t block;
	if (!alp_dsp_cmsis_block_len(n, &block)) {
		return ALP_ERR_OUT_OF_RANGE;
	}
	uint32_t idx;
	arm_mean_f32(x, block, &out->mean);
	arm_rms_f32(x, block, &out->rms);
	arm_min_f32(x, block, &out->min, &idx);
	arm_max_f32(x, block, &out->max, &idx);
	arm_absmax_f32(x, block, &out->abs_max, &out->abs_max_index);
#else
	/* Single portable-C pass (native_sim, or any target without CMSIS-DSP). */
	float    mean = 0.0f, sumsq = 0.0f, mn = x[0], mx = x[0], amax = 0.0f;
	uint32_t amax_i = 0u;
	for (size_t i = 0; i < n; i++) {
		float v = x[i];
		mean += v;
		sumsq += v * v;
		if (v < mn) {
			mn = v;
		}
		if (v > mx) {
			mx = v;
		}
		float a = fabsf(v);
		if (a > amax) {
			amax   = a;
			amax_i = (uint32_t)i;
		}
	}
	out->mean          = mean / (float)n;
	out->rms           = sqrtf(sumsq / (float)n);
	out->min           = mn;
	out->max           = mx;
	out->abs_max       = amax;
	out->abs_max_index = amax_i;
#endif
	/*
	 * Population variance E[x^2] - E[x]^2 (== rms^2 - mean^2), derived
	 * the same way on both paths so callers get ONE definition (CMSIS
	 * arm_var_f32 would give the sample variance / (n-1) instead).  A
	 * tiny negative from FP rounding on a near-constant signal is
	 * clamped to zero.
	 */
	out->variance = out->rms * out->rms - out->mean * out->mean;
	if (out->variance < 0.0f) {
		out->variance = 0.0f;
	}
	return ALP_OK;
}

alp_status_t alp_dsp_biquad_design(alp_dsp_biquad_kind_t kind,
                                   float                 f0_hz,
                                   float                 fs_hz,
                                   float                 q,
                                   float                 coeffs_out[5])
{
	/* Reject nonsensical designs up front: f0 must sit strictly inside
	 * (0, Nyquist), and Q / fs must be positive. */
	if (coeffs_out == NULL || fs_hz <= 0.0f || q <= 0.0f || f0_hz <= 0.0f ||
	    f0_hz >= 0.5f * fs_hz) {
		return ALP_ERR_INVAL;
	}

	/* RBJ audio-EQ cookbook, single 2nd-order section.  w0 is the
	 * normalised centre/cutoff, alpha sets the bandwidth from Q. */
	const float w0    = 2.0f * (float)M_PI * f0_hz / fs_hz;
	const float cosw  = cosf(w0);
	const float sinw  = sinf(w0);
	const float alpha = sinw / (2.0f * q);

	/* Denominator is shared by all four responses. */
	const float a0 = 1.0f + alpha;
	const float a1 = -2.0f * cosw;
	const float a2 = 1.0f - alpha;

	float b0, b1, b2;
	switch (kind) {
	case ALP_DSP_BIQUAD_LOWPASS:
		b0 = (1.0f - cosw) * 0.5f;
		b1 = 1.0f - cosw;
		b2 = b0;
		break;
	case ALP_DSP_BIQUAD_HIGHPASS:
		b0 = (1.0f + cosw) * 0.5f;
		b1 = -(1.0f + cosw);
		b2 = b0;
		break;
	case ALP_DSP_BIQUAD_BANDPASS: /* constant 0 dB peak gain */
		b0 = alpha;
		b1 = 0.0f;
		b2 = -alpha;
		break;
	case ALP_DSP_BIQUAD_NOTCH:
		b0 = 1.0f;
		b1 = -2.0f * cosw;
		b2 = 1.0f;
		break;
	default:
		return ALP_ERR_INVAL;
	}

	/* Normalise by a0 into the { b0, b1, b2, a1, a2 } order the IIR stage
	 * consumes (its difference equation subtracts a1,a2 directly). */
	coeffs_out[0] = b0 / a0;
	coeffs_out[1] = b1 / a0;
	coeffs_out[2] = b2 / a0;
	coeffs_out[3] = a1 / a0;
	coeffs_out[4] = a2 / a0;
	return ALP_OK;
}

/* ================================================================== */
/* Streaming integer-ratio decimator (#2134)                           */
/* ================================================================== */

/*
 * Anti-alias low-pass tap tables, one per supported ratio.  Generated by
 * `scripts/gen_dsp_decimator_coeffs.py` -- DO NOT hand-edit; re-run the
 * script and paste its output here if the design parameters change (it
 * is a leaf design tool with no other source-of-truth to regenerate
 * from, so it is not wired into test-all.sh's generated-files loop --
 * `python3 scripts/gen_dsp_decimator_coeffs.py --check` verifies these
 * tables still match the design).
 *
 * Method: windowed-sinc low-pass, Kaiser window (beta = 6.976), Q15
 * fixed-point.  Every ratio shares the SAME tap count and beta
 * (ALP_DSP_DECIMATOR_TAPS = 135 taps).  The PASSBAND edge is held at a
 * fixed offset below the output Nyquist (fs_in / (2*ratio)); the
 * STOPBAND edge sits at fs_out - passband_edge (fs_out = fs_in / ratio),
 * not at the output Nyquist itself -- the wider transition band this
 * buys is what lands stopband attenuation around 70 dB instead of ~40.
 * Content between the output Nyquist and fs_out - passband_edge is
 * therefore NOT stopband-attenuated; it folds into the transition band
 * above the stated passband.  Each table's centre tap is nudged so the
 * table sums to exactly 32768 (unity DC gain in Q15).  Figures below are
 * measured on these shipped Q15 taps, not the float design.  See
 * <alp/dsp.h>'s alp_dsp_decimator_init Doxygen for the full per-ratio
 * numbers.
 */
/* clang-format off */

/* ratio=2  fs_in=32000 Hz  passband_edge=7467.7 Hz  stopband_edge=8532.3 Hz
   achieved: ripple=0.0044 dB  stopband_attenuation=70.31 dB  group_delay=33.50 output samples */
static const int16_t _alp_dsp_decim_coeffs_r2[ALP_DSP_DECIMATOR_TAPS] = {
	-1, 0, 2, 0, -3, 0, 5, 0,
	-7, 0, 9, 0, -13, 0, 17, 0,
	-22, 0, 28, 0, -36, 0, 45, 0,
	-55, 0, 67, 0, -81, 0, 97, 0,
	-116, 0, 138, 0, -162, 0, 191, 0,
	-224, 0, 262, 0, -307, 0, 359, 0,
	-421, 0, 497, 0, -590, 0, 710, 0,
	-869, 0, 1093, 0, -1438, 0, 2049, 0,
	-3454, 0, 10423, 16382, 10423, 0, -3454, 0,
	2049, 0, -1438, 0, 1093, 0, -869, 0,
	710, 0, -590, 0, 497, 0, -421, 0,
	359, 0, -307, 0, 262, 0, -224, 0,
	191, 0, -162, 0, 138, 0, -116, 0,
	97, 0, -81, 0, 67, 0, -55, 0,
	45, 0, -36, 0, 28, 0, -22, 0,
	17, 0, -13, 0, 9, 0, -7, 0,
	5, 0, -3, 0, 2, 0, -1,
};

/* ratio=3  fs_in=48000 Hz  passband_edge=7201.6 Hz  stopband_edge=8798.4 Hz
   achieved: ripple=0.0052 dB  stopband_attenuation=65.46 dB  group_delay=22.33 output samples */
static const int16_t _alp_dsp_decim_coeffs_r3[ALP_DSP_DECIMATOR_TAPS] = {
	1, 0, -2, -2, 0, 3, 4, 0,
	-6, -7, 0, 10, 11, 0, -15, -17,
	0, 22, 25, 0, -31, -35, 0, 43,
	47, 0, -58, -64, 0, 77, 84, 0,
	-100, -109, 0, 129, 141, 0, -165, -179,
	0, 210, 227, 0, -266, -287, 0, 337,
	365, 0, -430, -469, 0, 560, 615, 0,
	-752, -840, 0, 1078, 1246, 0, -1774, -2232,
	0, 4504, 9027, 10916, 9027, 4504, 0, -2232,
	-1774, 0, 1246, 1078, 0, -840, -752, 0,
	615, 560, 0, -469, -430, 0, 365, 337,
	0, -287, -266, 0, 227, 210, 0, -179,
	-165, 0, 141, 129, 0, -109, -100, 0,
	84, 77, 0, -64, -58, 0, 47, 43,
	0, -35, -31, 0, 25, 22, 0, -17,
	-15, 0, 11, 10, 0, -7, -6, 0,
	4, 3, 0, -2, -2, 0, 1,
};

/* ratio=4  fs_in=32000 Hz  passband_edge=3467.7 Hz  stopband_edge=4532.3 Hz
   achieved: ripple=0.0046 dB  stopband_attenuation=69.35 dB  group_delay=16.75 output samples */
static const int16_t _alp_dsp_decim_coeffs_r4[ALP_DSP_DECIMATOR_TAPS] = {
	1, 1, 1, 0, -2, -4, -3, 0,
	5, 8, 7, 0, -9, -15, -12, 0,
	16, 25, 20, 0, -25, -40, -31, 0,
	39, 61, 47, 0, -57, -89, -69, 0,
	82, 126, 97, 0, -115, -176, -135, 0,
	158, 242, 185, 0, -217, -332, -254, 0,
	298, 457, 351, 0, -417, -646, -502, 0,
	614, 970, 773, 0, -1017, -1694, -1449, 0,
	2443, 5200, 7370, 8194, 7370, 5200, 2443, 0,
	-1449, -1694, -1017, 0, 773, 970, 614, 0,
	-502, -646, -417, 0, 351, 457, 298, 0,
	-254, -332, -217, 0, 185, 242, 158, 0,
	-135, -176, -115, 0, 97, 126, 82, 0,
	-69, -89, -57, 0, 47, 61, 39, 0,
	-31, -40, -25, 0, 20, 25, 16, 0,
	-12, -15, -9, 0, 7, 8, 5, 0,
	-3, -4, -2, 0, 1, 1, 1,
};

/* ratio=6  fs_in=48000 Hz  passband_edge=3201.6 Hz  stopband_edge=4798.4 Hz
   achieved: ripple=0.0062 dB  stopband_attenuation=69.87 dB  group_delay=11.17 output samples */
static const int16_t _alp_dsp_decim_coeffs_r6[ALP_DSP_DECIMATOR_TAPS] = {
	0, 0, 1, 2, 3, 3, 2, 0,
	-3, -7, -9, -10, -6, 0, 9, 17,
	22, 22, 14, 0, -18, -35, -45, -43,
	-27, 0, 33, 64, 81, 77, 49, 0,
	-58, -109, -138, -129, -81, 0, 95, 179,
	224, 210, 131, 0, -153, -287, -359, -337,
	-211, 0, 248, 468, 590, 560, 355, 0,
	-434, -840, -1093, -1078, -719, 0, 1024, 2232,
	3454, 4503, 5211, 5460, 5211, 4503, 3454, 2232,
	1024, 0, -719, -1078, -1093, -840, -434, 0,
	355, 560, 590, 468, 248, 0, -211, -337,
	-359, -287, -153, 0, 131, 210, 224, 179,
	95, 0, -81, -129, -138, -109, -58, 0,
	49, 77, 81, 64, 33, 0, -27, -43,
	-45, -35, -18, 0, 14, 22, 22, 17,
	9, 0, -6, -10, -9, -7, -3, 0,
	2, 3, 3, 2, 1, 0, 0,
};
/* clang-format on */

static const int16_t *_alp_dsp_decim_lookup(uint32_t ratio)
{
	switch (ratio) {
	case 2u:
		return _alp_dsp_decim_coeffs_r2;
	case 3u:
		return _alp_dsp_decim_coeffs_r3;
	case 4u:
		return _alp_dsp_decim_coeffs_r4;
	case 6u:
		return _alp_dsp_decim_coeffs_r6;
	default:
		return NULL;
	}
}

alp_status_t alp_dsp_decimator_init(alp_dsp_decimator_t *dec, uint32_t ratio, uint8_t n_channels)
{
	if (dec == NULL || n_channels == 0u || n_channels > ALP_DSP_DECIMATOR_MAX_CHANNELS) {
		return ALP_ERR_INVAL;
	}
	const int16_t *coeffs = _alp_dsp_decim_lookup(ratio);
	if (coeffs == NULL) {
		return ALP_ERR_OUT_OF_RANGE;
	}
	memset(dec, 0, sizeof(*dec));
	dec->coeffs     = coeffs;
	dec->ratio      = ratio;
	dec->n_channels = n_channels;
	return ALP_OK;
}

alp_status_t alp_dsp_decimator_reset(alp_dsp_decimator_t *dec)
{
	if (dec == NULL || dec->coeffs == NULL || dec->ratio == 0u) {
		return ALP_ERR_INVAL;
	}
	memset(dec->history, 0, sizeof(dec->history));
	memset(dec->write_idx, 0, sizeof(dec->write_idx));
	dec->phase = 0u;
	return ALP_OK;
}

alp_status_t alp_dsp_decimator_process(alp_dsp_decimator_t *dec,
                                       const int16_t       *in,
                                       size_t               in_frames,
                                       int16_t             *out,
                                       size_t               out_cap,
                                       size_t              *out_frames)
{
	if (dec == NULL || in == NULL || out == NULL || out_frames == NULL || dec->coeffs == NULL ||
	    dec->ratio == 0u) {
		return ALP_ERR_INVAL;
	}
	/* Exact output frame count for THIS call, computed up front from the
	 * running phase -- no loop needed, and no state is touched until the
	 * capacity check below passes (all-or-nothing: a too-small out_cap
	 * leaves dec unchanged rather than partially consuming input). */
	const size_t exact_out = (dec->phase + in_frames) / dec->ratio;
	if (out_cap < exact_out) {
		return ALP_ERR_OUT_OF_RANGE;
	}

	const uint32_t taps     = ALP_DSP_DECIMATOR_TAPS;
	const uint32_t half     = taps / 2u; /* 67 symmetric pairs + 1 centre tap. */
	const int16_t *coeffs   = dec->coeffs;
	const uint8_t  n_ch     = dec->n_channels;
	size_t         produced = 0u;
	for (size_t i = 0u; i < in_frames; i++) {
		for (uint8_t ch = 0u; ch < n_ch; ch++) {
			uint32_t widx                 = dec->write_idx[ch];
			dec->history[ch][widx]        = in[i * n_ch + ch];
			dec->history[ch][widx + taps] = in[i * n_ch + ch];
			widx++;
			if (widx == taps) { /* compare-and-wrap: widx only ever steps by
				                    * 1, so a full `% taps` (udiv+mls) is
				                    * wasted work here. */
				widx = 0u;
			}
			dec->write_idx[ch] = widx;
		}
		dec->phase++;
		if (dec->phase < dec->ratio) {
			continue;
		}
		dec->phase = 0u;
		for (uint8_t ch = 0u; ch < n_ch; ch++) {
			const int16_t *window = &dec->history[ch][dec->write_idx[ch]];
			/* The tap table is symmetric (coeffs[j] == coeffs[taps-1-j] --
			 * an odd-length linear-phase FIR), so pair up mirrored taps
			 * before multiplying: 67 products + the unpaired centre tap
			 * instead of 135, same exact accumulation (int arithmetic
			 * promotion means the window[j]+window[taps-1-j] sum cannot
			 * overflow before the multiply widens to int64). */
			int64_t acc = (int64_t)coeffs[half] * (int64_t)window[half];
			for (uint32_t j = 0u; j < half; j++) {
				acc += (int64_t)coeffs[j] * (int64_t)(window[j] + window[taps - 1u - j]);
			}
			/* Q15 rescale (arithmetic right shift -- sign-extending on
			 * every toolchain this SDK targets), then saturate: the
			 * Kaiser-windowed filter has unity DC gain but a transient
			 * combination of taps can still nudge the sum a few counts
			 * past full scale. */
			int64_t y = acc >> 15;
			if (y > INT16_MAX) {
				y = INT16_MAX;
			} else if (y < INT16_MIN) {
				y = INT16_MIN;
			}
			out[produced * n_ch + ch] = (int16_t)y;
		}
		produced++;
	}
	*out_frames = produced;
	return ALP_OK;
}
