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
 * from, so it is not wired into test-all.sh's generated-files loop).
 *
 * Method: windowed-sinc low-pass, Kaiser window, Q15 fixed-point, unity
 * DC gain.  Every ratio shares the SAME tap count and Kaiser beta
 * (ALP_DSP_DECIMATOR_TAPS = 135 taps, beta = 3.395) by holding the
 * normalised transition width constant across ratios instead of the
 * passband-edge fraction -- the achieved specs below land within a
 * tight, uniform band (~39-40 dB stopband, <0.2 dB ripple) instead of
 * degrading sharply at the largest ratio.  See <alp/dsp.h>'s
 * alp_dsp_decimator_init Doxygen for the full per-ratio numbers.
 */
/* clang-format off */

/* ratio=2  fs_in=32000 Hz  passband_edge=7467.7 Hz  output_nyquist=8000.0 Hz
   achieved: ripple=0.17 dB  stopband_attenuation=39.7 dB  group_delay=33.5 output samples */
static const int16_t _alp_dsp_decim_coeffs_r2[ALP_DSP_DECIMATOR_TAPS] = {
	22, -8, -27, 6, 33, -4, -39, 0,
	46, 5, -53, -12, 59, 21, -65, -31,
	71, 43, -75, -57, 79, 72, -80, -90,
	80, 110, -77, -131, 72, 154, -63, -178,
	50, 204, -34, -231, 12, 258, 15, -286,
	-48, 315, 89, -343, -138, 370, 196, -397,
	-267, 422, 353, -446, -458, 467, 591, -487,
	-766, 504, 1007, -519, -1370, 530, 1999, -538,
	-3423, 543, 10409, 15834, 10409, 543, -3423, -538,
	1999, 530, -1370, -519, 1007, 504, -766, -487,
	591, 467, -458, -446, 353, 422, -267, -397,
	196, 370, -138, -343, 89, 315, -48, -286,
	15, 258, 12, -231, -34, 204, 50, -178,
	-63, 154, 72, -131, -77, 110, 80, -90,
	-80, 72, 79, -57, -75, 43, 71, -31,
	-65, 21, 59, -12, -53, 5, 46, 0,
	-39, -4, 33, 6, -27, -8, 22,
};

/* ratio=3  fs_in=48000 Hz  passband_edge=7201.6 Hz  output_nyquist=8000.0 Hz
   achieved: ripple=0.18 dB  stopband_attenuation=39.9 dB  group_delay=22.3 output samples */
static const int16_t _alp_dsp_decim_coeffs_r3[ALP_DSP_DECIMATOR_TAPS] = {
	-15, 8, 27, 23, -5, -33, -33, 0,
	38, 45, 9, -42, -59, -21, 44, 74,
	37, -42, -90, -57, 36, 105, 81, -25,
	-119, -110, 7, 130, 142, 18, -138, -178,
	-52, 139, 217, 95, -133, -258, -149, 116,
	300, 216, -87, -342, -297, 41, 383, 397,
	27, -422, -522, -126, 457, 684, 273, -487,
	-911, -506, 512, 1270, 924, -530, -1998, -1928,
	541, 4755, 8740, 10371, 8740, 4755, 541, -1928,
	-1998, -530, 924, 1270, 512, -506, -911, -487,
	273, 684, 457, -126, -522, -422, 27, 397,
	383, 41, -297, -342, -87, 216, 300, 116,
	-149, -258, -133, 95, 217, 139, -52, -178,
	-138, 18, 142, 130, 7, -110, -119, -25,
	81, 105, 36, -57, -90, -42, 37, 74,
	44, -21, -59, -42, 9, 45, 38, 0,
	-33, -33, -5, 23, 27, 8, -15,
};

/* ratio=4  fs_in=32000 Hz  passband_edge=3467.7 Hz  output_nyquist=4000.0 Hz
   achieved: ripple=0.17 dB  stopband_attenuation=39.4 dB  group_delay=16.8 output samples */
static const int16_t _alp_dsp_decim_coeffs_r4[ALP_DSP_DECIMATOR_TAPS] = {
	-21, -24, -14, 6, 27, 36, 27, 0,
	-31, -49, -43, -12, 30, 62, 64, 31,
	-24, -73, -88, -57, 10, 80, 114, 90,
	14, -79, -140, -131, -50, 68, 162, 179,
	100, -43, -178, -231, -165, -1, 182, 287,
	247, 68, -170, -343, -350, -166, 132, 397,
	479, 308, -58, -446, -648, -521, -81, 488,
	893, 877, 350, -519, -1341, -1637, -1037, 539,
	2807, 5186, 6986, 7656, 6986, 5186, 2807, 539,
	-1037, -1637, -1341, -519, 350, 877, 893, 488,
	-81, -521, -648, -446, -58, 308, 479, 397,
	132, -166, -350, -343, -170, 68, 247, 287,
	182, -1, -165, -231, -178, -43, 100, 179,
	162, 68, -50, -131, -140, -79, 14, 90,
	114, 80, 10, -57, -88, -73, -24, 31,
	64, 62, 30, -12, -43, -49, -31, 0,
	27, 36, 27, 6, -14, -24, -21,
};

/* ratio=6  fs_in=48000 Hz  passband_edge=3201.6 Hz  output_nyquist=4000.0 Hz
   achieved: ripple=0.17 dB  stopband_attenuation=39.3 dB  group_delay=11.2 output samples */
static const int16_t _alp_dsp_decim_coeffs_r6[ALP_DSP_DECIMATOR_TAPS] = {
	4, -8, -20, -29, -33, -30, -18, 0,
	21, 40, 53, 55, 44, 21, -11, -44,
	-71, -85, -81, -57, -16, 33, 81, 115,
	127, 110, 66, 1, -72, -136, -176, -179,
	-141, -65, 34, 136, 219, 259, 244, 171,
	49, -99, -241, -344, -379, -330, -197, -1,
	222, 423, 554, 573, 460, 218, -118, -489,
	-815, -1013, -1010, -754, -231, 532, 1467, 2474,
	3434, 4226, 4748, 4930, 4748, 4226, 3434, 2474,
	1467, 532, -231, -754, -1010, -1013, -815, -489,
	-118, 218, 460, 573, 554, 423, 222, -1,
	-197, -330, -379, -344, -241, -99, 49, 171,
	244, 259, 219, 136, 34, -65, -141, -179,
	-176, -136, -72, 1, 66, 110, 127, 115,
	81, 33, -16, -57, -81, -85, -71, -44,
	-11, 21, 44, 55, 53, 40, 21, 0,
	-18, -30, -33, -29, -20, -8, 4,
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
	size_t         produced = 0u;
	for (size_t i = 0u; i < in_frames; i++) {
		for (uint8_t ch = 0u; ch < dec->n_channels; ch++) {
			uint32_t widx                 = dec->write_idx[ch];
			dec->history[ch][widx]        = in[i * dec->n_channels + ch];
			dec->history[ch][widx + taps] = in[i * dec->n_channels + ch];
			widx                          = (widx + 1u) % taps;
			dec->write_idx[ch]            = widx;
		}
		dec->phase++;
		if (dec->phase < dec->ratio) {
			continue;
		}
		dec->phase = 0u;
		for (uint8_t ch = 0u; ch < dec->n_channels; ch++) {
			const int16_t *window = &dec->history[ch][dec->write_idx[ch]];
			int64_t        acc    = 0;
			for (uint32_t j = 0u; j < taps; j++) {
				acc += (int64_t)dec->coeffs[j] * (int64_t)window[j];
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
			out[produced * dec->n_channels + ch] = (int16_t)y;
		}
		produced++;
	}
	*out_frames = produced;
	return ALP_OK;
}
