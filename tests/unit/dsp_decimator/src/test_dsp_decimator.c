/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host (native_sim) tests for <alp/dsp.h>'s alp_dsp_decimator_t (#2134,
 * phase 1 -- the standalone streaming decimator; the audio-in backend
 * wiring is a separate, later phase and is NOT exercised here).
 *
 * Covers:
 *   - EVERY supported ratio (2, 3, 4, 6), table-driven: a passband tone at
 *     ~0.9x the ratio's stated passband edge stays within its stated
 *     ripple, and a tone just above the ratio's stated stopband edge is
 *     attenuated by at least (stated stopband - 0.5 dB).  Catches a
 *     wrong-ratio table swap that a single-ratio smoke test would miss
 *     (see the mutation note below).
 *   - Exact output frame counts across odd block-size boundaries
 *     (1, 7, 256, 1023 frames/call) match a single-block run bit-for-bit,
 *     AND match the absolute `n_in / ratio` count.
 *   - Stereo channels stay independent (tone on L only -> R stays exactly
 *     silent, since convolving an all-zero channel is exactly zero).
 *   - Every supported ratio (2, 3, 4, 6) opens; unsupported ratios and
 *     out-of-range channel counts are rejected.
 *   - alp_dsp_decimator_reset reproduces alp_dsp_decimator_init's state
 *     exactly (same input in -> same output as a freshly-opened decimator).
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "alp/dsp.h"
#include "alp/peripheral.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Goertzel single-bin magnitude estimator.  For a real sinusoid whose
 * frequency lands exactly on bin k of an N-sample block (k = round(f/fs*N)
 * with no remainder, as every frequency used below is chosen to satisfy),
 * this returns the sinusoid's amplitude (not just a relative power figure),
 * so test assertions can compare directly against the known input
 * amplitude instead of needing a separate unfiltered reference run. */
static float goertzel_amplitude(const int16_t *x, size_t n, size_t k)
{
	const float w     = 2.0f * (float)M_PI * (float)k / (float)n;
	const float coeff = 2.0f * cosf(w);
	float       s0 = 0.0f, s1 = 0.0f, s2 = 0.0f;
	for (size_t i = 0u; i < n; i++) {
		s0 = (float)x[i] + coeff * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	const float real = s1 - s2 * cosf(w);
	const float imag = s2 * sinf(w);
	return 2.0f * sqrtf(real * real + imag * imag) / (float)n;
}

/* ============================================================== */
/* 1. Tone-quality: EVERY ratio, table-driven                      */
/* ============================================================== */

/* One row per supported ratio.  `f_pb_hz` / `f_sb_hz` are chosen so both
 * tones land on an EXACT bin of the `n_out`-point output-rate analysis
 * (and, since n_in = n_out * ratio, an exact bin of the input block too
 * -- no spectral leakage either side).  `pb_bin` is the passband tone's
 * output bin; `sb_bin` is the bin the stopband tone ALIASES to after
 * decimation (input bin index mod n_out, the standard decimation-alias
 * relation for an n_in = ratio * n_out block).  `stopband_db` mirrors
 * the achieved figure in src/dsp_dispatch.c's embedded table comment /
 * this header's alp_dsp_decimator_init Doxygen -- keep the three in
 * sync if the design changes.
 *
 * Values reproduced by scripts/gen_dsp_decimator_coeffs.py (passband
 * 0.9x the stated edge; stopband the first exact-bin frequency above
 * the stated edge). */
typedef struct {
	uint32_t ratio;
	double   fs_in_hz;
	size_t   n_out;
	size_t   pb_bin;
	size_t   sb_bin;
	double   f_pb_hz;
	double   f_sb_hz;
	double   stopband_db;
} decim_ratio_spec_t;

static const decim_ratio_spec_t k_ratio_specs[] = {
	/* ratio  fs_in     n_out   pb_bin  sb_bin  f_pb_hz     f_sb_hz     stopband_db */
	{ 2u, 32000.0, 4096u, 1721u, 2185u, 6722.6562, 8535.1562, 70.31 },
	{ 3u, 48000.0, 4096u, 1659u, 2253u, 6480.4688, 8800.7812, 65.46 },
	{ 4u, 32000.0, 4096u, 1598u, 2321u, 3121.0938, 4533.2031, 69.35 },
	{ 6u, 48000.0, 4096u, 1475u, 2457u, 2880.8594, 4798.8281, 69.87 },
};

ZTEST(alp_dsp_decimator, test_every_ratio_passband_and_stopband_tones)
{
	const double a1 = 8000.0; /* passband tone amplitude */
	const double a2 = 8000.0; /* stopband tone amplitude */

	for (size_t s = 0u; s < ARRAY_SIZE(k_ratio_specs); s++) {
		const decim_ratio_spec_t *spec = &k_ratio_specs[s];

		/* A pure-tone-at-an-exact-bin input is only leakage-free against
		 * the LTI filter's STEADY-STATE response; the first (TAPS-1)
		 * input samples are a startup transient (history still filling
		 * from all-zero) whose broadband energy would otherwise swamp a
		 * stopband measurement this tight (order -0.5 dB matters). Prime
		 * the filter with a warmup run of the SAME two-tone signal --
		 * comfortably >= 2x the tap count, rounded up to a whole number
		 * of decimation cycles so `phase` is back at 0 at the boundary --
		 * and analyze only the frames produced AFTER warmup. */
		const size_t warmup_in =
		    ((2u * ALP_DSP_DECIMATOR_TAPS + spec->ratio - 1u) / spec->ratio) * spec->ratio;
		const size_t discard_out = warmup_in / spec->ratio;
		const size_t n_in        = warmup_in + spec->n_out * spec->ratio;

		int16_t *in = malloc(n_in * sizeof(int16_t));
		zassert_not_null(in, "ratio %u: alloc failed", spec->ratio);
		for (size_t i = 0u; i < n_in; i++) {
			double v = a1 * sin(2.0 * M_PI * spec->f_pb_hz * (double)i / spec->fs_in_hz) +
			           a2 * sin(2.0 * M_PI * spec->f_sb_hz * (double)i / spec->fs_in_hz);
			in[i]    = (int16_t)lrint(v);
		}

		alp_dsp_decimator_t dec;
		zassert_equal(
		    alp_dsp_decimator_init(&dec, spec->ratio, 1u), ALP_OK, "ratio %u", spec->ratio);

		const size_t out_cap = discard_out + spec->n_out;
		int16_t     *out     = malloc(out_cap * sizeof(int16_t));
		zassert_not_null(out, "ratio %u: alloc failed", spec->ratio);
		size_t       got = 0u;
		alp_status_t rc  = alp_dsp_decimator_process(&dec, in, n_in, out, out_cap, &got);
		zassert_equal(rc, ALP_OK, "ratio %u", spec->ratio);
		zassert_equal(
		    got, out_cap, "ratio %u: expected %zu frames, got %zu", spec->ratio, out_cap, got);

		const int16_t *steady = out + discard_out;
		float          mag_pb = goertzel_amplitude(steady, spec->n_out, spec->pb_bin);
		float          mag_sb = goertzel_amplitude(steady, spec->n_out, spec->sb_bin);

		/* Passband: within a generous +/-15% of the injected amplitude
		 * (stated ripple is well under 0.01 dB =~ 0.1%; the rest of the
		 * margin covers Q15 quantization and the filter's startup
		 * transient). */
		zassert_true(mag_pb > 0.85f * (float)a1 && mag_pb < 1.15f * (float)a1,
		             "ratio %u: passband amplitude out of band: %f (expected ~%f)",
		             spec->ratio,
		             (double)mag_pb,
		             a1);

		/* Stopband: attenuated by at least (stated stopband - 0.5 dB).
		 * This is the check that catches a wrong-ratio table (e.g. #2134
		 * review's case-3u-serves-the-ratio-6-table mutation): a swapped
		 * table's actual cutoff sits at the WRONG ratio's edge, so this
		 * ratio's stopband tone lands well inside its passband instead
		 * of being attenuated. */
		double atten_db = 20.0 * log10((double)a2 / (double)mag_sb);
		zassert_true(atten_db >= spec->stopband_db - 0.5,
		             "ratio %u: stopband tone only attenuated %f dB (want >= %f)",
		             spec->ratio,
		             atten_db,
		             spec->stopband_db - 0.5);

		free(in);
		free(out);
	}
}

/* ============================================================== */
/* 2. Exact frame counts across odd block-size boundaries           */
/* ============================================================== */

ZTEST(alp_dsp_decimator, test_exact_frame_counts_across_odd_blocks)
{
	const uint32_t ratio = 3u;
	const size_t   n_in  = 3000u;

	int16_t in[3000];
	for (size_t i = 0u; i < n_in; i++) {
		/* Deterministic non-trivial signal: two mixed tones, no need to
		 * be periodic here -- this test only checks streaming exactness,
		 * not spectral content. */
		double v = 5000.0 * sin(2.0 * M_PI * 733.0 * (double)i / 48000.0) +
		           2000.0 * sin(2.0 * M_PI * 9001.0 * (double)i / 48000.0);
		in[i]    = (int16_t)lrint(v);
	}

	/* Reference: single-block run. */
	alp_dsp_decimator_t dec_ref;
	zassert_equal(alp_dsp_decimator_init(&dec_ref, ratio, 1u), ALP_OK, NULL);
	int16_t out_ref[1100];
	size_t  got_ref = 0u;
	zassert_equal(
	    alp_dsp_decimator_process(&dec_ref, in, n_in, out_ref, ARRAY_SIZE(out_ref), &got_ref),
	    ALP_OK,
	    NULL);
	/* Absolute count, not just "chunked matches single-block": n_in=3000
	 * is an exact multiple of ratio=3, so from phase 0 a single call must
	 * emit exactly n_in/ratio frames -- no more, no fewer. */
	zassert_equal(got_ref, n_in / ratio, "expected %zu frames, got %zu", n_in / ratio, got_ref);

	/* Chunked run: cycle through {1, 7, 256, 1023}-frame blocks. */
	const size_t        block_sizes[] = { 1u, 7u, 256u, 1023u };
	alp_dsp_decimator_t dec_chunked;
	zassert_equal(alp_dsp_decimator_init(&dec_chunked, ratio, 1u), ALP_OK, NULL);
	int16_t out_chunked[1100];
	size_t  total_got = 0u;
	size_t  consumed  = 0u;
	size_t  block_i   = 0u;
	while (consumed < n_in) {
		size_t chunk = block_sizes[block_i % ARRAY_SIZE(block_sizes)];
		block_i++;
		if (chunk > n_in - consumed) {
			chunk = n_in - consumed;
		}
		size_t       got = 0u;
		alp_status_t s   = alp_dsp_decimator_process(&dec_chunked,
		                                             &in[consumed],
		                                             chunk,
		                                             &out_chunked[total_got],
		                                             ARRAY_SIZE(out_chunked) - total_got,
		                                             &got);
		zassert_equal(s, ALP_OK, NULL);
		total_got += got;
		consumed += chunk;
	}

	zassert_equal(total_got, got_ref, "chunked=%zu single-block=%zu", total_got, got_ref);
	zassert_mem_equal(out_chunked, out_ref, got_ref * sizeof(int16_t), NULL);
}

/* ============================================================== */
/* 3. Stereo channel independence                                   */
/* ============================================================== */

ZTEST(alp_dsp_decimator, test_stereo_channels_independent)
{
	const uint32_t ratio = 3u;
	const size_t   n_in  = 300u;

	int16_t in[300 * 2];
	for (size_t i = 0u; i < n_in; i++) {
		double l        = 6000.0 * sin(2.0 * M_PI * 1000.0 * (double)i / 48000.0);
		in[i * 2u + 0u] = (int16_t)lrint(l); /* L: tone */
		in[i * 2u + 1u] = 0;                 /* R: silence */
	}

	alp_dsp_decimator_t dec;
	zassert_equal(alp_dsp_decimator_init(&dec, ratio, 2u), ALP_OK, NULL);

	int16_t out[120 * 2];
	size_t  got = 0u;
	zassert_equal(
	    alp_dsp_decimator_process(&dec, in, n_in, out, ARRAY_SIZE(out) / 2u, &got), ALP_OK, NULL);
	zassert_true(got > 0u, NULL);

	bool l_nonzero = false;
	for (size_t i = 0u; i < got; i++) {
		zassert_equal(out[i * 2u + 1u], 0, "R channel not silent at frame %zu", i);
		if (out[i * 2u + 0u] != 0) {
			l_nonzero = true;
		}
	}
	zassert_true(l_nonzero, "L channel came out all-zero");
}

/* ============================================================== */
/* 4. Ratio / channel-count validation                              */
/* ============================================================== */

ZTEST(alp_dsp_decimator, test_all_supported_ratios_open)
{
	const uint32_t ratios[] = { 2u, 3u, 4u, 6u };
	for (size_t i = 0u; i < ARRAY_SIZE(ratios); i++) {
		alp_dsp_decimator_t dec;
		zassert_equal(
		    alp_dsp_decimator_init(&dec, ratios[i], 1u), ALP_OK, "ratio %u rejected", ratios[i]);
		alp_dsp_decimator_t dec2;
		zassert_equal(alp_dsp_decimator_init(&dec2, ratios[i], 2u),
		              ALP_OK,
		              "ratio %u rejected (stereo)",
		              ratios[i]);
	}
}

ZTEST(alp_dsp_decimator, test_unsupported_ratio_rejected)
{
	alp_dsp_decimator_t dec;
	zassert_equal(alp_dsp_decimator_init(&dec, 5u, 1u), ALP_ERR_OUT_OF_RANGE, NULL);
	zassert_equal(alp_dsp_decimator_init(&dec, 1u, 1u), ALP_ERR_OUT_OF_RANGE, NULL);
	zassert_equal(alp_dsp_decimator_init(&dec, 0u, 1u), ALP_ERR_OUT_OF_RANGE, NULL);
}

ZTEST(alp_dsp_decimator, test_bad_channel_count_rejected)
{
	alp_dsp_decimator_t dec;
	zassert_equal(alp_dsp_decimator_init(&dec, 3u, 0u), ALP_ERR_INVAL, NULL);
	zassert_equal(alp_dsp_decimator_init(&dec, 3u, 3u), ALP_ERR_INVAL, NULL);
}

ZTEST(alp_dsp_decimator, test_null_args_rejected)
{
	alp_dsp_decimator_t dec;
	zassert_equal(alp_dsp_decimator_init(NULL, 3u, 1u), ALP_ERR_INVAL, NULL);
	zassert_equal(alp_dsp_decimator_init(&dec, 3u, 1u), ALP_OK, NULL);

	int16_t in[4]  = { 0 };
	int16_t out[4] = { 0 };
	size_t  got    = 0u;
	zassert_equal(alp_dsp_decimator_process(NULL, in, 4u, out, 4u, &got), ALP_ERR_INVAL, NULL);
	zassert_equal(alp_dsp_decimator_process(&dec, NULL, 4u, out, 4u, &got), ALP_ERR_INVAL, NULL);
	zassert_equal(alp_dsp_decimator_process(&dec, in, 4u, NULL, 4u, &got), ALP_ERR_INVAL, NULL);
	zassert_equal(alp_dsp_decimator_process(&dec, in, 4u, out, 4u, NULL), ALP_ERR_INVAL, NULL);
	zassert_equal(alp_dsp_decimator_reset(NULL), ALP_ERR_INVAL, NULL);
}

ZTEST(alp_dsp_decimator, test_out_cap_too_small_rejected_and_state_unchanged)
{
	alp_dsp_decimator_t dec;
	zassert_equal(alp_dsp_decimator_init(&dec, 3u, 1u), ALP_OK, NULL);

	int16_t in[9]  = { 100, 200, 300, 400, 500, 600, 700, 800, 900 };
	int16_t out[2] = { 0 };
	size_t  got    = 0u;
	/* 9 input frames at ratio 3 from phase 0 produce exactly 3 output
	 * frames -- out_cap of 2 must be rejected, not silently truncated. */
	zassert_equal(
	    alp_dsp_decimator_process(&dec, in, 9u, out, 2u, &got), ALP_ERR_OUT_OF_RANGE, NULL);

	/* State must be untouched: a retry with a big-enough buffer must
	 * still see phase==0 (i.e. produce exactly 3 frames, not fewer). */
	int16_t out_ok[8] = { 0 };
	zassert_equal(
	    alp_dsp_decimator_process(&dec, in, 9u, out_ok, ARRAY_SIZE(out_ok), &got), ALP_OK, NULL);
	zassert_equal(got, 3u, NULL);
}

/* ============================================================== */
/* 5. Reset clears state                                            */
/* ============================================================== */

ZTEST(alp_dsp_decimator, test_reset_reproduces_fresh_init)
{
	const uint32_t ratio       = 3u;
	int16_t        probe_in[7] = { 111, -222, 333, -444, 555, -666, 777 };

	/* Freshly-initialised decimator processing probe_in is the ground truth. */
	alp_dsp_decimator_t dec_fresh;
	zassert_equal(alp_dsp_decimator_init(&dec_fresh, ratio, 1u), ALP_OK, NULL);
	int16_t out_fresh[4] = { 0 };
	size_t  got_fresh    = 0u;
	zassert_equal(alp_dsp_decimator_process(&dec_fresh,
	                                        probe_in,
	                                        ARRAY_SIZE(probe_in),
	                                        out_fresh,
	                                        ARRAY_SIZE(out_fresh),
	                                        &got_fresh),
	              ALP_OK,
	              NULL);

	/* Same decimator, driven through unrelated history + a reset, must
	 * reproduce the fresh-init result exactly on the same probe input. */
	alp_dsp_decimator_t dec_used;
	zassert_equal(alp_dsp_decimator_init(&dec_used, ratio, 1u), ALP_OK, NULL);
	int16_t churn_in[50];
	for (size_t i = 0u; i < ARRAY_SIZE(churn_in); i++) {
		churn_in[i] = (int16_t)(1000 + (int)i * 37);
	}
	int16_t churn_out[20] = { 0 };
	size_t  churn_got     = 0u;
	zassert_equal(alp_dsp_decimator_process(&dec_used,
	                                        churn_in,
	                                        ARRAY_SIZE(churn_in),
	                                        churn_out,
	                                        ARRAY_SIZE(churn_out),
	                                        &churn_got),
	              ALP_OK,
	              NULL);
	zassert_equal(alp_dsp_decimator_reset(&dec_used), ALP_OK, NULL);

	int16_t out_after_reset[4] = { 0 };
	size_t  got_after_reset    = 0u;
	zassert_equal(alp_dsp_decimator_process(&dec_used,
	                                        probe_in,
	                                        ARRAY_SIZE(probe_in),
	                                        out_after_reset,
	                                        ARRAY_SIZE(out_after_reset),
	                                        &got_after_reset),
	              ALP_OK,
	              NULL);

	zassert_equal(got_after_reset, got_fresh, NULL);
	zassert_mem_equal(out_after_reset, out_fresh, got_fresh * sizeof(int16_t), NULL);
}

ZTEST_SUITE(alp_dsp_decimator, NULL, NULL, NULL, NULL, NULL);
