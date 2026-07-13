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
	 * slot mid-op. op_enter counts this op in; begin_close drains it.
	 * ALP_ERR_INVAL preserved here (not NOT_READY) to match this op's
	 * pre-#629 null/not-in-use contract. #629 */
	if (chain == NULL || !alp_handle_op_enter(&chain->lifecycle, &chain->active_ops)) {
		return ALP_ERR_INVAL;
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
		return ALP_ERR_INVAL;
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
		return ALP_ERR_INVAL;
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
		return ALP_ERR_INVAL;
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
	if (!alp_handle_begin_close(&chain->lifecycle, &chain->active_ops)) {
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
	/* CMSIS-DSP statistics kernels (Helium-vectorised on M-class cores). */
	uint32_t idx;
	arm_mean_f32(x, (uint32_t)n, &out->mean);
	arm_rms_f32(x, (uint32_t)n, &out->rms);
	arm_min_f32(x, (uint32_t)n, &out->min, &idx);
	arm_max_f32(x, (uint32_t)n, &out->max, &idx);
	arm_absmax_f32(x, (uint32_t)n, &out->abs_max, &out->abs_max_index);
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
