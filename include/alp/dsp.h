/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dsp.h
 * @brief ALP SDK digital-signal-processing chain abstraction.
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
 * Chain validation rules (enforced at @ref alp_dsp_chain_open):
 *   - 1..@ref ALP_DSP_MAX_STAGES stages.
 *   - At most ONE @ref ALP_DSP_STAGE_FFT, and if present it MUST be
 *     the terminal stage.
 *   - A @ref ALP_DSP_STAGE_WINDOW (if present) MUST immediately
 *     precede @ref ALP_DSP_STAGE_FFT.  Window without a terminating
 *     FFT is rejected -- it has no defined audible meaning in the
 *     filtered-samples path.
 *   - Per-stage param ranges (n_taps / n_sections / n_points) are
 *     bounded -- see the @ref ALP_DSP_MAX_* macros.
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

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Limits                                                              */
/* ================================================================== */

/** Maximum stages per chain.  v0.5 wire format documents the same
 *  bound for the upcoming bridge-side `CMD_ADC_STREAM_CONFIGURE_DSP`. */
#define ALP_DSP_MAX_STAGES        4u

/** Maximum FIR taps per FIR stage.  Q31 storage = 256 bytes max. */
#define ALP_DSP_MAX_FIR_TAPS      64u

/** Maximum biquad sections per IIR stage (cascaded direct-form-1). */
#define ALP_DSP_MAX_IIR_SECTIONS  8u

/** Minimum FFT size in points (power-of-two). */
#define ALP_DSP_MIN_FFT_POINTS    32u

/** Maximum FFT size in points (power-of-two). */
#define ALP_DSP_MAX_FFT_POINTS    1024u

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
    ALP_DSP_FFT_OUTPUT_COMPLEX   = 0,
    /** Magnitude `sqrt(re*re + im*im)` per bin.  Output element count
     *  = @c n_points.  Wire-friendly: half the bandwidth of COMPLEX. */
    ALP_DSP_FFT_OUTPUT_MAGNITUDE = 1,
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
 *  Each biquad section is `y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2]
 *                                - a1*y[n-1] - a2*y[n-2]`.  Coefficient
 *  order on the wire and in memory: `b0, b1, b2, a1, a2` per section. */
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
    uint16_t              n_points;      /**< Power-of-two in
                                              [@ref ALP_DSP_MIN_FFT_POINTS,
                                               @ref ALP_DSP_MAX_FFT_POINTS]. */
    alp_dsp_fft_output_t  output_format; /**< COMPLEX or MAGNITUDE. */
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
 *           @ref ALP_DSP_MAX_* / @ref ALP_DSP_MIN_FFT_POINTS limits,
 *           or @c n_points not a power-of-two).
 *         - @ref ALP_ERR_NOMEM when the static chain pool is
 *           exhausted (compile-time pool size).
 */
alp_dsp_chain_t *alp_dsp_chain_open(const alp_dsp_stage_t *stages,
                                    size_t n_stages);

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
 *         FFT -- use @ref alp_dsp_chain_apply_bins instead).
 */
alp_status_t alp_dsp_chain_apply_samples(alp_dsp_chain_t *chain,
                                         const int16_t *in_mv,
                                         size_t in_n,
                                         int16_t *out_mv,
                                         size_t out_cap,
                                         size_t *got);

/**
 * @brief Apply an FFT-terminated chain to an in-RAM sample buffer.
 *
 * Valid only when the chain ends with an FFT stage.  Pre-FFT stages
 * (FIR / IIR / WINDOW) run sequentially against the running sample
 * buffer; the FFT consumes exactly @c n_points samples from the
 * window output and emits bins per the configured @c output_format.
 *
 * Output element count:
 *   - @ref ALP_DSP_FFT_OUTPUT_COMPLEX:    2 * @c n_points
 *     (interleaved re/im pairs).
 *   - @ref ALP_DSP_FFT_OUTPUT_MAGNITUDE:  @c n_points
 *     (per-bin magnitudes).
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
 *         out_cap < required).
 */
alp_status_t alp_dsp_chain_apply_bins(alp_dsp_chain_t *chain,
                                      const int16_t *in_mv,
                                      size_t in_n,
                                      float *out_bins,
                                      size_t out_cap,
                                      size_t *got);

/**
 * @brief Release a DSP chain back to the pool.
 *
 * NULL is a no-op.  After this call @p chain is invalid -- subsequent
 * apply / close calls on the same handle are undefined.
 *
 * @param[in] chain  Handle from @ref alp_dsp_chain_open, or NULL.
 */
void alp_dsp_chain_close(alp_dsp_chain_t *chain);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* ALP_DSP_H */
