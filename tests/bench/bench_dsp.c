/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Microbenchmarks for <alp/dsp.h>.  Measures DSP-chain open
 * validation cost + apply rejection cost on the stub-backend path
 * (no CMSIS-DSP linked in for native_sim).  Real-stack benches
 * (FIR throughput, biquad section cost, FFT N-point latency at
 * various sizes) live in the per-(SoM, OS) baselines under
 * E1M-<MPN>-<os>.yaml once HiL goes online.
 */

#include <stddef.h>
#include <stdint.h>

#include "bench.h"

#include "alp/dsp.h"

/* A 4-tap FIR + Hann window + 64-point FFT chain -- exercises
 * every validator branch (FIR taps array, window kind enum, FFT
 * power-of-two check, chain ordering FFT-must-be-terminal). */
static const float k_fir_taps[4] = { 0.25f, 0.25f, 0.25f, 0.25f };

void               bench_dsp_main(void)
{
	/* chain_open with a NULL stages pointer -- earliest INVAL exit. */
	BENCH_RUN("alp_dsp_chain_open(NULL stages)", 1000000, { (void)alp_dsp_chain_open(NULL, 0u); });

	/* chain_open with a single FIR stage -- exercises the full
     * validation path (kind enum, n_taps range, coeff_format,
     * taps pointer copy).  On native_sim this either allocates
     * from the pool or returns NULL if NOSUPPORT; either way the
     * dispatcher cost is what we're measuring. */
	BENCH_RUN("alp_dsp_chain_open(FIR-4tap)", 100000, {
		const alp_dsp_stage_t stages[] = {
			{ .kind  = ALP_DSP_STAGE_FIR,
			  .u.fir = { .coeff_format = ALP_DSP_COEFF_FORMAT_F32,
			             .n_taps       = 4u,
			             .taps         = k_fir_taps } },
		};
		alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 1u);
		alp_dsp_chain_close(c);
	});

	/* chain_open with FFT-terminated chain -- exercises the
     * "WINDOW must immediately precede FFT" + "FFT must be
     * terminal" validators.  Most expensive open-time check. */
	BENCH_RUN("alp_dsp_chain_open(WINDOW+FFT-64)", 100000, {
		const alp_dsp_stage_t stages[] = {
			{ .kind = ALP_DSP_STAGE_WINDOW, .u.window = { .shape = ALP_DSP_WINDOW_HANN } },
			{ .kind  = ALP_DSP_STAGE_FFT,
			  .u.fft = { .n_points = 64u, .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE } },
		};
		alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 2u);
		alp_dsp_chain_close(c);
	});

	/* apply_samples on a NULL chain -- the NULL-handle guard. */
	BENCH_RUN("alp_dsp_chain_apply_samples(NULL)", 1000000, {
		int16_t out[16] = { 0 };
		size_t  got     = 0u;
		(void)alp_dsp_chain_apply_samples(NULL, NULL, 0u, out, 16u, &got);
	});

	/* apply_bins on a NULL chain -- the NULL-handle guard. */
	BENCH_RUN("alp_dsp_chain_apply_bins(NULL)", 1000000, {
		float  bins[16] = { 0 };
		size_t got      = 0u;
		(void)alp_dsp_chain_apply_bins(NULL, NULL, 0u, bins, 16u, &got);
	});

	/* close on NULL is a documented no-op; bench measures the
     * single-branch overhead. */
	BENCH_RUN("alp_dsp_chain_close(NULL)", 1000000, { alp_dsp_chain_close(NULL); });
}
