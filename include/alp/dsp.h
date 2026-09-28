/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dsp.h
 * @brief Alp SDK digital-signal-processing chain abstraction.
 *
 * Composable filter / window / FFT chains run against in-RAM sample
 * buffers (standalone API) or, in a later release, inline with an
 * `alp_adc_stream_*` source on SoMs whose backend can offload to a
 * hardware DSP block (the V2N supervisor's GD32G5 FFT / FAC units).
 *
 * This header declares the **standalone** surface: build a chain
 * with @ref alp_dsp_chain_open against a list of @ref alp_dsp_stage_t
 * descriptors, then feed sample buffers through with
 * @ref alp_dsp_chain_apply_samples (filter-terminated chain) or
 * @ref alp_dsp_chain_apply_bins (FFT-terminated chain).  The
 * bridge-wired counterpart (@c alp_adc_filter_t / @c alp_adc_spectrum_t)
 * sits in `<alp/adc.h>` and lands in v0.5.x.
 *
 * Backends:
 *   - **CMSIS-DSP** when `ALP_HAS_CMSIS_DSP=1` (preferred — `arm_fir_*`,
 *     `arm_biquad_cascade_df1_*`, `arm_rfft_fast_f32`).
 *   - **Portable C fallback** otherwise (naive convolution + radix-2
 *     Cooley-Tukey).  Documented as O(N*M) / O(N^2) — fine for unit
 *     tests and small (~< 256-point) chains; not suitable for the hot
 *     path.  Applications that want spectral or filtered ADC data at
 *     line rate should target a SoM with a HW backend (V2N family
 *     today; AEN once the wave-2 bridge ships).
 *
 * @ref alp_dsp_decimator_t (streaming integer-ratio decimator) is
 * separate from the chain above: plain portable C on every target, no
 * backend selection.
 *
 * Chain validation rules (enforced at @ref alp_dsp_chain_open):
 *   - 1..@ref ALP_DSP_MAX_STAGES stages.
 *   - At most ONE @ref ALP_DSP_STAGE_FFT, and if present it MUST be
 *     the terminal stage.
 *   - A @ref ALP_DSP_STAGE_WINDOW (if present) MUST immediately
 *     precede @ref ALP_DSP_STAGE_FFT.  Window without a terminating
 *     FFT is rejected -- it has no defined audible meaning in the
 *     filtered-samples path.
 *   - Per-stage param ranges (n_taps / n_sections / n_points) are
 *     bounded -- see the @c ALP_DSP_MAX_* macros.
 *
 * Typical usage (filter-terminated):
 * @code
 *     static const float fir_taps[8] = { ... };
 *     alp_dsp_stage_t stages[] = {
 *         { .kind = ALP_DSP_STAGE_FIR,
 *           .u.fir = { .coeff_format = ALP_DSP_COEFF_FORMAT_F32,
 *                      .n_taps = 8, .taps = fir_taps } },
 *     };
 *     alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 1u);
 *     size_t got = 0;
 *     int16_t out[256];
 *     alp_dsp_chain_apply_samples(c, in, 256u, out, 256u, &got);
 *     alp_dsp_chain_close(c);
 * @endcode
 *
 * Typical usage (FFT-terminated):
 * @code
 *     alp_dsp_stage_t stages[] = {
 *         { .kind = ALP_DSP_STAGE_WINDOW,
 *           .u.window = { .shape = ALP_DSP_WINDOW_HANN } },
 *         { .kind = ALP_DSP_STAGE_FFT,
 *           .u.fft = { .n_points = 128u,
 *                      .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE } },
 *     };
 *     alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 2u);
 *     float mag[128];
 *     size_t got = 0;
 *     alp_dsp_chain_apply_bins(c, in, 128u, mag, 128u, &got);
 *     alp_dsp_chain_close(c);
 * @endcode
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.5 new -- standalone DSP-chain API.  Composes with adc.h filter/spectrum types; both sides may co-evolve.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_DSP_H
#define ALP_DSP_H

#include <stddef.h>
#include <stdint.h>

#include "alp/cap_instance.h"
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Limits                                                              */
/* ================================================================== */

/** Maximum stages per chain.  v0.5 wire format documents the same
 *  bound for the upcoming bridge-side `CMD_ADC_STREAM_CONFIGURE_DSP`. */
#define ALP_DSP_MAX_STAGES 4u

/** Maximum FIR taps per FIR stage.  Q31 storage = 256 bytes max. */
#define ALP_DSP_MAX_FIR_TAPS 64u

/** Maximum biquad sections per IIR stage (cascaded direct-form-1). */
#define ALP_DSP_MAX_IIR_SECTIONS 8u

/** Minimum FFT size in points (power-of-two). */
#define ALP_DSP_MIN_FFT_POINTS 32u

/** Maximum FFT size in points (power-of-two). */
#define ALP_DSP_MAX_FFT_POINTS 1024u

/* ================================================================== */
/* Enums                                                               */
/* ================================================================== */

/** DSP stage kinds. */
typedef enum {
	ALP_DSP_STAGE_FIR    = 0, /**< Finite impulse response filter.   */
	ALP_DSP_STAGE_IIR    = 1, /**< Cascaded biquad IIR filter.        */
	ALP_DSP_STAGE_WINDOW = 2, /**< Window function (must precede FFT). */
	ALP_DSP_STAGE_FFT    = 3, /**< Forward FFT (terminal stage only). */
} alp_dsp_stage_kind_t;

/** Window shape for @ref ALP_DSP_STAGE_WINDOW. */
typedef enum {
	ALP_DSP_WINDOW_RECTANGULAR = 0, /**< No window (unity gain).      */
	ALP_DSP_WINDOW_HANN        = 1, /**< Hann window.                  */
	ALP_DSP_WINDOW_HAMMING     = 2, /**< Hamming window.               */
	ALP_DSP_WINDOW_BLACKMAN    = 3, /**< Blackman window.              */
} alp_dsp_window_kind_t;

/** Output format for @ref ALP_DSP_STAGE_FFT. */
typedef enum {
	/** Interleaved (re, im) f32 pairs.  Output element count =
     *  2 * @c n_points.  Bins are not normalised. */
	ALP_DSP_FFT_OUTPUT_COMPLEX = 0,
	/** Magnitude `sqrt(re*re + im*im)` per bin, full two-sided spectrum.
     *  Output element count = @c n_points. */
	ALP_DSP_FFT_OUTPUT_MAGNITUDE = 1,
	/** Magnitude of the positive-frequency half only: bins 0..N/2, i.e.
     *  `n_points/2 + 1` values (DC through Nyquist).  This is the natural
     *  output of a real FFT -- the two-sided MAGNITUDE spectrum is just
     *  this mirrored -- so prefer it for real-input spectra to halve the
     *  output buffer and skip the redundant negative-frequency bins. */
	ALP_DSP_FFT_OUTPUT_MAGNITUDE_ONESIDED = 2,
} alp_dsp_fft_output_t;

/** Coefficient format for FIR/IIR stages. */
typedef enum {
	/** IEEE-754 single-precision.  Caller pointer is `const float *`. */
	ALP_DSP_COEFF_FORMAT_F32 = 0,
	/** Q31 fixed-point (signed 32-bit, full-scale = +/-1.0).  Caller
     *  pointer is `const int32_t *`. */
	ALP_DSP_COEFF_FORMAT_Q31 = 1,
} alp_dsp_coeff_format_t;

/* ================================================================== */
/* Per-stage parameter structs                                         */
/* ================================================================== */

/** FIR filter parameters (one per @ref ALP_DSP_STAGE_FIR). */
typedef struct {
	alp_dsp_coeff_format_t coeff_format; /**< F32 or Q31.            */
	uint16_t               n_taps;       /**< 1..@ref ALP_DSP_MAX_FIR_TAPS. */
	const void            *taps;         /**< Pointer to coefficient
                                              array; @c n_taps entries in
                                              the chosen format.  Copied
                                              into the chain on open --
                                              caller may free after
                                              @ref alp_dsp_chain_open. */
} alp_dsp_fir_params_t;

/** Cascaded biquad IIR parameters (one per @ref ALP_DSP_STAGE_IIR).
 *  Each biquad section computes
 *  @c y[n]=b0*x[n]+b1*x[n-1]+b2*x[n-2]-a1*y[n-1]-a2*y[n-2].  Coefficient
 *  order on the wire and in memory: @c b0,b1,b2,a1,a2 per section. */
typedef struct {
	alp_dsp_coeff_format_t coeff_format; /**< F32 or Q31.            */
	uint16_t               n_sections;   /**< 1..@ref ALP_DSP_MAX_IIR_SECTIONS. */
	const void            *coeffs;       /**< 5 * @c n_sections entries
                                              in the chosen format.  Copied
                                              into the chain on open. */
} alp_dsp_iir_params_t;

/** Window parameters (one per @ref ALP_DSP_STAGE_WINDOW).
 *  The window length is bound to the following FFT stage's @c n_points;
 *  the window coefficients are computed inside the chain at open time. */
typedef struct {
	alp_dsp_window_kind_t shape; /**< One of @ref alp_dsp_window_kind_t. */
} alp_dsp_window_params_t;

/** FFT parameters (one per @ref ALP_DSP_STAGE_FFT). */
typedef struct {
	uint16_t             n_points;      /**< Power-of-two in
                                              [@ref ALP_DSP_MIN_FFT_POINTS,
                                               @ref ALP_DSP_MAX_FFT_POINTS]. */
	alp_dsp_fft_output_t output_format; /**< COMPLEX or MAGNITUDE. */
} alp_dsp_fft_params_t;

/** One stage description.  Union member selected by @c kind. */
typedef struct {
	alp_dsp_stage_kind_t kind;
	union {
		alp_dsp_fir_params_t    fir;
		alp_dsp_iir_params_t    iir;
		alp_dsp_window_params_t window;
		alp_dsp_fft_params_t    fft;
	} u;
} alp_dsp_stage_t;

/* ================================================================== */
/* Chain handle + API                                                  */
/* ================================================================== */

/** Opaque chain handle.  Allocate via @ref alp_dsp_chain_open and
 *  release via @ref alp_dsp_chain_close. */
typedef struct alp_dsp_chain alp_dsp_chain_t;

/**
 * @brief Validate and open a DSP chain.
 *
 * The implementation copies the stage params (including coefficient
 * arrays) into the chain so the caller's source memory can be freed
 * immediately on return.
 *
 * @param[in] stages    Array of @p n_stages descriptors.  Must be
 *                      non-NULL when @p n_stages > 0.
 * @param[in] n_stages  Stage count, 1..@ref ALP_DSP_MAX_STAGES.
 *
 * @return Open handle on success, or NULL with @ref alp_last_error set
 *         to one of:
 *         - @ref ALP_ERR_INVAL on NULL pointer, zero stage count,
 *           bad enum value, or chain ordering violation (FFT not
 *           terminal, WINDOW not preceding FFT, etc).
 *         - @ref ALP_ERR_OUT_OF_RANGE on per-stage bound violation
 *           (@c n_taps / @c n_sections / @c n_points outside the
 *           @c ALP_DSP_MAX_* / @ref ALP_DSP_MIN_FFT_POINTS limits,
 *           or @c n_points not a power-of-two).
 *         - @ref ALP_ERR_NOMEM when the static chain pool is
 *           exhausted (compile-time pool size).
 */
alp_dsp_chain_t *alp_dsp_chain_open(const alp_dsp_stage_t *stages, size_t n_stages);

/**
 * @brief Apply a filter-terminated chain to an in-RAM sample buffer.
 *
 * Valid only when the chain does NOT contain an FFT stage.  Each
 * non-FFT stage runs sequentially against the running sample buffer;
 * the final stage's output is written to @p out_mv.
 *
 * @param[in]  chain    Handle from @ref alp_dsp_chain_open.
 * @param[in]  in_mv    Input samples (mV, int16_t).
 * @param[in]  in_n     Input sample count.
 * @param[out] out_mv   Output buffer.  May overlap with @p in_mv only
 *                      if @c out_mv == in_mv (in-place).
 * @param[in]  out_cap  Capacity of @p out_mv (samples).
 * @param[out] got      Receives the number of output samples written
 *                      (`<= min(in_n, out_cap)`).
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT (chain ends with
 *         FFT -- use @ref alp_dsp_chain_apply_bins instead) /
 *         ALP_ERR_NOT_READY (NULL or closed @p chain).
 */
alp_status_t alp_dsp_chain_apply_samples(alp_dsp_chain_t *chain,
                                         const int16_t   *in_mv,
                                         size_t           in_n,
                                         int16_t         *out_mv,
                                         size_t           out_cap,
                                         size_t          *got);

/**
 * @brief Float-native twin of @ref alp_dsp_chain_apply_samples.
 *
 * Identical to @ref alp_dsp_chain_apply_samples but consumes and
 * produces `float` samples directly -- no int16 quantisation on either
 * edge.  Prefer this for float DSP pipelines; the int16 variant is the
 * ADC-domain (millivolt) flavour that composes with `<alp/adc.h>`.
 *
 * @param[in]  chain    Handle from @ref alp_dsp_chain_open.
 * @param[in]  in       Input samples (float).
 * @param[in]  in_n     Input sample count.
 * @param[out] out      Output buffer (float; in-place if @c out == in).
 * @param[in]  out_cap  Capacity of @p out (samples).
 * @param[out] got      Number of output samples written
 *                      (`<= min(in_n, out_cap)`).
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT (chain ends with
 *         FFT -- use @ref alp_dsp_chain_apply_bins_f32 instead) /
 *         ALP_ERR_NOT_READY (NULL or closed @p chain).
 */
alp_status_t alp_dsp_chain_apply_samples_f32(alp_dsp_chain_t *chain,
                                             const float     *in,
                                             size_t           in_n,
                                             float           *out,
                                             size_t           out_cap,
                                             size_t          *got);

/**
 * @brief Apply an FFT-terminated chain to an in-RAM sample buffer.
 *
 * Valid only when the chain ends with an FFT stage.  Pre-FFT stages
 * (FIR / IIR / WINDOW) run sequentially against the running sample
 * buffer; the FFT consumes exactly @c n_points samples from the
 * window output and emits bins per the configured @c output_format.
 *
 * Output element count:
 *   - @ref ALP_DSP_FFT_OUTPUT_COMPLEX -- 2 * @c n_points
 *     (interleaved re/im pairs).
 *   - @ref ALP_DSP_FFT_OUTPUT_MAGNITUDE -- @c n_points
 *     (full two-sided per-bin magnitudes).
 *   - @ref ALP_DSP_FFT_OUTPUT_MAGNITUDE_ONESIDED -- @c n_points/2 + 1
 *     (positive-frequency half; the natural real-FFT output).
 *
 * @par Streaming semantics: pre-FFT FIR / IIR stages carry their filter
 *      state ACROSS calls (a sliding filter), while the FFT itself
 *      consumes exactly @c n_points samples from @c in each call.
 *      Feeding successive (optionally overlapping) windows therefore
 *      yields an STFT-style stream; @ref alp_dsp_chain_close resets all
 *      filter state.
 *
 * @param[in]  chain     Handle from @ref alp_dsp_chain_open.
 * @param[in]  in_mv     Input samples (mV, int16_t).  Must provide
 *                       at least @c n_points samples (extra are
 *                       silently truncated; @c n_points is the FFT
 *                       size from the terminal stage).
 * @param[in]  in_n      Input sample count.
 * @param[out] out_bins  Output buffer (float).
 * @param[in]  out_cap   Capacity of @p out_bins (elements, not bytes).
 *                       Must be >= the output element count above.
 * @param[out] got       Receives the number of output elements
 *                       written.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT (chain does not
 *         end with FFT) / ALP_ERR_OUT_OF_RANGE (in_n < n_points or
 *         out_cap < required) / ALP_ERR_NOT_READY (NULL or closed
 *         @p chain).
 */
alp_status_t alp_dsp_chain_apply_bins(alp_dsp_chain_t *chain,
                                      const int16_t   *in_mv,
                                      size_t           in_n,
                                      float           *out_bins,
                                      size_t           out_cap,
                                      size_t          *got);

/**
 * @brief Float-native twin of @ref alp_dsp_chain_apply_bins.
 *
 * Identical to @ref alp_dsp_chain_apply_bins (same output formats,
 * counts, and streaming semantics) but consumes `float` input samples
 * directly instead of int16 millivolts.  Prefer this for float pipelines.
 *
 * @param[in]  chain     Handle from @ref alp_dsp_chain_open.
 * @param[in]  in        Input samples (float); >= @c n_points provided.
 * @param[in]  in_n      Input sample count.
 * @param[out] out_bins  Output buffer (float).
 * @param[in]  out_cap   Capacity of @p out_bins (elements).
 * @param[out] got       Number of output elements written.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT / ALP_ERR_OUT_OF_RANGE /
 *         ALP_ERR_NOT_READY (NULL or closed @p chain).
 */
alp_status_t alp_dsp_chain_apply_bins_f32(alp_dsp_chain_t *chain,
                                          const float     *in,
                                          size_t           in_n,
                                          float           *out_bins,
                                          size_t           out_cap,
                                          size_t          *got);

/**
 * @brief Release a DSP chain back to the pool.
 *
 * NULL is a no-op.  After this call @p chain is invalid -- subsequent
 * apply / close calls on the same handle are undefined.
 *
 * @param[in] chain  Handle from @ref alp_dsp_chain_open, or NULL.
 */
void alp_dsp_chain_close(alp_dsp_chain_t *chain);

/**
 * @brief Cached per-handle capability snapshot.
 *
 * Returns a pointer to the capability bitmask the backend registry
 * recorded for @p chain at @ref alp_dsp_chain_open time.  The flags
 * are SoC-specific (HW-FFT-present, Q31 fast-path, ...); use
 * @ref alp_capabilities_has to test individual bits.
 *
 * @param[in] chain  Handle from @ref alp_dsp_chain_open, or NULL.
 *
 * @return Pointer to the cached capability struct, or NULL if
 *         @p chain is NULL.  The pointer is valid until
 *         @ref alp_dsp_chain_close.
 */
const alp_capabilities_t *alp_dsp_chain_capabilities(const alp_dsp_chain_t *chain);

/* ================================================================== */
/* Summary statistics                                                  */
/* ================================================================== */

/**
 * @brief One-pass summary statistics over a real float buffer.
 *
 * Populated by @ref alp_dsp_stats_f32.  All fields describe the buffer
 * passed to that call verbatim -- if the caller wants AC statistics
 * (DC/mean removed), it must mean-centre the buffer first and pass the
 * centred data.
 */
typedef struct {
	float    mean;          /**< Arithmetic mean, `(1/n) * sum(x[i])`.        */
	float    rms;           /**< Root-mean-square, `sqrt((1/n) * sum(x[i]^2))`. */
	float    variance;      /**< Population variance, `E[x^2] - E[x]^2` (>= 0). */
	float    min;           /**< Minimum sample value.                         */
	float    max;           /**< Maximum sample value.                         */
	float    abs_max;       /**< Maximum magnitude, `max(|x[i]|)` (peak).      */
	uint32_t abs_max_index; /**< Index of the @c abs_max sample.               */
} alp_dsp_stats_t;

/**
 * @brief Compute summary statistics over a real float buffer in one call.
 *
 * Fills @p out with the mean, RMS, population variance, min/max, and the
 * peak magnitude (+ its index) of @p x.  This is the portable
 * scalar-stats counterpart to the FFT chain: the backend runs CMSIS-DSP
 * `arm_mean_f32` / `arm_rms_f32` / `arm_min_f32` / `arm_max_f32` /
 * `arm_absmax_f32` on Cortex-M (when the cmsis-dsp module is linked) and
 * a single portable-C pass otherwise, so application code never calls
 * `arm_*` directly and the same source builds on every target.
 *
 * @param[in]  x    Sample buffer; must be non-NULL and hold @p n floats.
 * @param[in]  n    Sample count; must be > 0.
 * @param[out] out  Destination stats struct; must be non-NULL.
 *
 * @return @ref ALP_OK on success, or @ref ALP_ERR_INVAL when @p x or
 *         @p out is NULL or @p n is 0 (in which case @p out is
 *         left untouched).
 *
 * @note @ref alp_dsp_stats_t::variance is the POPULATION variance
 *       (divides by @p n), not the sample variance (@p n - 1) that
 *       CMSIS-DSP's `arm_var_f32` returns -- the value is the same on
 *       both the CMSIS and portable paths.
 */
alp_status_t alp_dsp_stats_f32(const float *x, size_t n, alp_dsp_stats_t *out);

/* ================================================================== */
/* Biquad filter design                                                */
/* ================================================================== */

/**
 * @brief Response type for @ref alp_dsp_biquad_design.
 *
 * These are the four RBJ audio-EQ-cookbook second-order responses.
 * @note SCOPE BOUNDARY (deliberate, for maintenance): this surface
 *       covers ONLY the closed-form cookbook biquads below.  It is NOT
 *       a general filter-design toolbox -- higher-order Butterworth
 *       cascades, Chebyshev / elliptic / Bessel families, and
 *       arbitrary pole placement (Parks-McClellan etc.) are out of
 *       scope on purpose and belong in application code or an external
 *       design tool feeding F32 coefficients to @ref ALP_DSP_STAGE_IIR.
 */
typedef enum {
	ALP_DSP_BIQUAD_LOWPASS  = 0, /**< 2nd-order low-pass.             */
	ALP_DSP_BIQUAD_HIGHPASS = 1, /**< 2nd-order high-pass.            */
	ALP_DSP_BIQUAD_BANDPASS = 2, /**< Band-pass, constant 0 dB peak.  */
	ALP_DSP_BIQUAD_NOTCH    = 3, /**< Band-reject (notch).            */
} alp_dsp_biquad_kind_t;

/**
 * @brief Design one second-order biquad section (RBJ cookbook).
 *
 * Computes the five normalised coefficients for a single
 * @ref ALP_DSP_STAGE_IIR section from a centre/cutoff frequency and Q,
 * so callers do not hand-derive filter math.  A 2nd-order Butterworth
 * low-pass, for example, is @ref ALP_DSP_BIQUAD_LOWPASS with
 * @p q = 0.70710678 (`1/sqrt(2)`).
 *
 * @param[in]  kind       Response type (@ref alp_dsp_biquad_kind_t).
 * @param[in]  f0_hz      Cutoff (LP/HP) or centre (BP/notch) frequency;
 *                        must satisfy `0 < f0_hz < fs_hz/2`.
 * @param[in]  fs_hz      Sample rate in Hz; must be > 0.
 * @param[in]  q          Quality factor; must be > 0 (Butterworth LP/HP
 *                        uses `1/sqrt(2)`; higher Q = narrower/peakier).
 * @param[out] coeffs_out Receives `{ b0, b1, b2, a1, a2 }` normalised
 *                        (a0 = 1), ready to pass as an IIR stage's
 *                        @c coeffs with @c n_sections = 1 and
 *                        @ref ALP_DSP_COEFF_FORMAT_F32.
 *
 * @return @ref ALP_OK, or @ref ALP_ERR_INVAL on a NULL pointer, a bad
 *         @p kind, or a frequency/Q outside the valid ranges above.
 */
alp_status_t alp_dsp_biquad_design(alp_dsp_biquad_kind_t kind,
                                   float                 f0_hz,
                                   float                 fs_hz,
                                   float                 q,
                                   float                 coeffs_out[5]);

/* ================================================================== */
/* Streaming integer-ratio decimator                                   */
/* ================================================================== */

/** Maximum interleaved channels @ref alp_dsp_decimator_t supports. */
#define ALP_DSP_DECIMATOR_MAX_CHANNELS 2u

/** Anti-alias FIR length (taps), fixed across every supported ratio --
 *  see @ref alp_dsp_decimator_init for why one length is shared. */
#define ALP_DSP_DECIMATOR_TAPS 135u

/**
 * @brief Streaming keep-1-in-N decimator: anti-alias FIR + integer
 *        downsample, with per-channel filter state carried across calls.
 *
 * @par Allocation: caller-owned, no heap
 *      Unlike @ref alp_dsp_chain_t (library-owned, drawn from a static
 *      backend pool), this struct is a plain value type -- declare it on
 *      the stack, `static`, or inside another struct, and pass its
 *      address to @ref alp_dsp_decimator_init.  The library never
 *      allocates and keeps no hidden registry of outstanding decimators.
 *
 * @par Size: 1104 bytes on M55
 *      Larger than Zephyr's default `CONFIG_MAIN_STACK_SIZE` (1024 B).
 *      Prefer `static` storage or embedding this struct in backend/app
 *      state over declaring it on a thread's own stack.
 *
 * @par Opaque outside init/process/reset
 *      Layout is exposed only so callers can allocate storage without a
 *      pool; do not access fields directly.
 */
typedef struct {
	/** Per-channel circular history of raw int16 samples, double-length
	 *  (`2 * ALP_DSP_DECIMATOR_TAPS`) so the convolution window is
	 *  always a contiguous slice -- no per-tap modulo. */
	int16_t        history[ALP_DSP_DECIMATOR_MAX_CHANNELS][2u * ALP_DSP_DECIMATOR_TAPS];
	uint32_t       write_idx[ALP_DSP_DECIMATOR_MAX_CHANNELS]; /**< Next write slot,
	                                                        0..TAPS-1.        */
	const int16_t *coeffs;     /**< Selected ratio's Q15 tap table (internal
	                               static storage; never freed).            */
	uint32_t       ratio;      /**< Configured decimation ratio.              */
	uint32_t       phase;      /**< Input frames consumed since the last
	                               emitted output frame, 0..ratio-1.        */
	uint8_t        n_channels; /**< Configured channel count.                */
} alp_dsp_decimator_t;

/**
 * @brief Configure a decimator for a supported integer ratio + channel count.
 *
 * Zeroes all filter history and the phase counter, so a fresh @p dec
 * starts as if no samples had ever been seen (equivalent to
 * @ref alp_dsp_decimator_reset immediately after).
 *
 * @par Supported ratios
 *      2, 3, 4, 6 -- the set that serves 48000 Hz -> 16000/8000 Hz
 *      (ratios 3, 6) and 32000 Hz -> 16000/8000 Hz (ratios 2, 4).  Any
 *      other ratio (including 1, and non-integer resampling) is
 *      rejected; this surface covers integer-ratio decimation only.
 *
 * @par Filter design
 *      Identical method for every ratio -- generated by
 *      `scripts/gen_dsp_decimator_coeffs.py`, Kaiser-windowed-sinc
 *      (beta 6.976), @ref ALP_DSP_DECIMATOR_TAPS = 135 taps, Q15
 *      fixed-point, each table's centre tap nudged so it sums to
 *      exactly 32768 (unity DC gain).  The passband edge sits at a
 *      fixed offset below the output Nyquist (`fs_in / (2*ratio)`); the
 *      stopband edge sits at `fs_out - passband_edge` (`fs_out = fs_in /
 *      ratio`) rather than at the output Nyquist itself, which is what
 *      buys ~70 dB of stopband attenuation instead of ~40 at this tap
 *      count -- content between the output Nyquist and `fs_out -
 *      passband_edge` is NOT stopband-attenuated; it folds into the
 *      transition band above the stated passband (ratio 3: 8.0-8.8 kHz
 *      folds into the 7201.6-8798.4 Hz transition).  The achieved
 *      passband edge, passband ripple, stopband edge, stopband
 *      attenuation, and group delay in OUTPUT samples -- measured on the
 *      shipped Q15 taps, not the float design -- are:
 *        - ratio 2 (32000->16000 Hz): passband edge 7467.7 Hz, ripple
 *          0.0044 dB, stopband edge 8532.3 Hz, stopband attenuation
 *          70.31 dB, group delay ~33.50 output samples.
 *        - ratio 3 (48000->16000 Hz): passband edge 7201.6 Hz, ripple
 *          0.0052 dB, stopband edge 8798.4 Hz, stopband attenuation
 *          65.46 dB, group delay ~22.33 output samples.
 *        - ratio 4 (32000->8000 Hz): passband edge 3467.7 Hz, ripple
 *          0.0046 dB, stopband edge 4532.3 Hz, stopband attenuation
 *          69.35 dB, group delay ~16.75 output samples.
 *        - ratio 6 (48000->8000 Hz): passband edge 3201.6 Hz, ripple
 *          0.0062 dB, stopband edge 4798.4 Hz, stopband attenuation
 *          69.87 dB, group delay ~11.17 output samples.
 *      Group delay in INPUT samples is `(ALP_DSP_DECIMATOR_TAPS - 1) / 2`
 *      = 67 for every ratio (odd symmetric FIR -> exact integer linear
 *      phase); the output-sample figures above are 67 / ratio.
 *
 * @param[in,out] dec        Caller-allocated decimator to configure.  Must
 *                            be non-NULL.
 * @param[in]     ratio      Decimation ratio; must be one of 2, 3, 4, 6.
 * @param[in]     n_channels Interleaved channel count; must be in
 *                            1..@ref ALP_DSP_DECIMATOR_MAX_CHANNELS.
 *
 * @return @ref ALP_OK, or @ref ALP_ERR_INVAL on @p dec == NULL or
 *         @p n_channels out of range, or @ref ALP_ERR_OUT_OF_RANGE on an
 *         unsupported @p ratio.
 */
alp_status_t alp_dsp_decimator_init(alp_dsp_decimator_t *dec, uint32_t ratio, uint8_t n_channels);

/**
 * @brief Filter + downsample one block of interleaved int16 samples.
 *
 * Streaming: filter history and the keep-1-in-`ratio` phase persist
 * across calls, so feeding a stream in arbitrarily-sized blocks (1, 7,
 * 256, ... frames per call) produces EXACTLY the same output samples,
 * in the same order, as feeding it in one call -- no dropped or
 * duplicated output frame at a block boundary.
 *
 * @param[in]  dec       Decimator from @ref alp_dsp_decimator_init.
 * @param[in]  in        Interleaved input samples, `in_frames *
 *                        n_channels` elements (@p n_channels from the
 *                        @ref alp_dsp_decimator_init call).
 * @param[in]  in_frames Input frame count (samples per channel).
 * @param[out] out       Interleaved output buffer, caller-allocated.
 * @param[in]  out_cap   Capacity of @p out in FRAMES.  This call emits
 *                        AT MOST `(in_frames + ratio - 1) / ratio`
 *                        frames (a safe sizing bound that does not
 *                        require knowing the internal phase); if
 *                        @p out_cap is smaller than the exact frame
 *                        count this call would produce, it returns
 *                        @ref ALP_ERR_OUT_OF_RANGE and leaves @p dec's
 *                        state UNCHANGED (no partial consumption).
 * @param[out] out_frames Receives the number of output frames written.
 *
 * @return @ref ALP_OK, @ref ALP_ERR_INVAL (NULL @p dec/@p in/@p out/
 *         @p out_frames, or @p dec not initialised), or
 *         @ref ALP_ERR_OUT_OF_RANGE (@p out_cap too small -- see above).
 */
alp_status_t alp_dsp_decimator_process(alp_dsp_decimator_t *dec,
                                       const int16_t       *in,
                                       size_t               in_frames,
                                       int16_t             *out,
                                       size_t               out_cap,
                                       size_t              *out_frames);

/**
 * @brief Clear filter history and the decimation phase, keeping the
 *        configured ratio and channel count.
 *
 * Equivalent to re-running @ref alp_dsp_decimator_init with the same
 * @p ratio / @p n_channels, without needing to remember them.
 *
 * @param[in,out] dec  Decimator from @ref alp_dsp_decimator_init.  Must
 *                      be non-NULL.
 *
 * @return @ref ALP_OK, or @ref ALP_ERR_INVAL if @p dec is NULL or is
 *         zero-initialised and never passed to @ref alp_dsp_decimator_init.
 */
alp_status_t alp_dsp_decimator_reset(alp_dsp_decimator_t *dec);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_DSP_H */
