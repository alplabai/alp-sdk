/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tmu.h
 * @brief ALP SDK math-accelerator surface (CORDIC / TMU offload with libm fallback).
 *
 * Portable single-precision math primitives that the active SoM's
 * hardware math unit can accelerate when present.  On the V2N family
 * (V2N + V2N-M1) the calls dispatch through the GD32G553 supervisor
 * MCU's CORDIC engine via the bridge protocol -- the GD32 returns the
 * computed result over SPI / I2C and the SDK hands it back to the
 * caller.  On SoMs without a comparable hardware accelerator, the
 * same call falls back to libm (`sinf`, `cosf`, ...) so customer code
 * stays portable.
 *
 * Compared to invoking libm directly, this surface gives the SDK a
 * single seam where future SoCs can register their own math
 * accelerators (Renesas RZ/V2N TFU, Alif Ensemble FPU helpers, etc.)
 * without the application changing.
 *
 * Backends:
 *   - V2N family       : GD32G5 TMU via the bridge (CMD_TMU_COMPUTE).
 *   - Other Zephyr SoMs: libm (`sinf`, `cosf`, `sqrtf`, ...).
 *   - Yocto / baremetal: libm.
 *
 * Accuracy:
 *   The libm fallback delivers the platform's single-precision math
 *   accuracy directly.  The CORDIC-backed path on V2N is accurate to
 *   a few LSBs of binary32 -- callers needing strict reproducibility
 *   across SoMs (e.g. for regression-test fingerprints) should be
 *   tolerant of unit-in-last-place differences.
 *
 * Domain:
 *   This surface mirrors C99's `sinf`/`cosf`/... semantics.  Inputs
 *   outside a function's mathematical domain (sqrt of a negative,
 *   log of zero, ...) yield @ref ALP_ERR_OUT_OF_RANGE on the V2N
 *   backend; the libm fallback returns NaN / +-inf in the output
 *   slot and @ref ALP_OK -- callers that need to discriminate
 *   should check for NaN with @c isnanf.
 *
 * Typical usage:
 * @code
 *     float s;
 *     if (alp_tmu_sin(0.5f, &s) == ALP_OK) {
 *         // s ~= 0.479425539
 *     }
 *     float h;
 *     alp_tmu_hypot(3.0f, 4.0f, &h);   // h == 5.0
 * @endcode
 *
 * @note This header has no `gd32` references in its public surface --
 *       the customer-facing API stays SoM-agnostic per the SDK's
 *       portability rule.  The V2N backend in
 *       `src/zephyr/peripheral_tmu.c` performs the GD32 dispatch.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      Wave-1 GD32 CORDIC TMU helpers; surface limited and may fold into <alp/dsp.h> for v1.0.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_TMU_H
#define ALP_TMU_H

#include <stdint.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Compute sin(@p in_a).
 *
 * @param[in]  in_a    Argument in radians.
 * @param[out] out     Result (sin(in_a)).
 * @return ALP_OK on success;
 *         ALP_ERR_INVAL if @p out is NULL;
 *         ALP_ERR_NOSUPPORT if the V2N backend's HAL body is not yet
 *           wired (libm fallback never returns NOSUPPORT);
 *         ALP_ERR_NOT_READY if the V2N supervisor is enabled but no
 *           bridge bus is configured;
 *         ALP_ERR_IO on a backend / bridge failure.
 */
alp_status_t alp_tmu_sin(float in_a, float *out);

/**
 * @brief Compute cos(@p in_a).
 *
 * @param[in]  in_a    Argument in radians.
 * @param[out] out     Result (cos(in_a)).
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT /
 *         ALP_ERR_NOT_READY / ALP_ERR_IO.
 */
alp_status_t alp_tmu_cos(float in_a, float *out);

/**
 * @brief Compute tan(@p in_a).
 *
 * @param[in]  in_a    Argument in radians.
 * @param[out] out     Result (tan(in_a)).
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT /
 *         ALP_ERR_NOT_READY / ALP_ERR_IO.
 */
alp_status_t alp_tmu_tan(float in_a, float *out);

/**
 * @brief Compute atan(@p in_a).
 *
 * @param[in]  in_a    Argument.
 * @param[out] out     Result (atan(in_a), in radians, in [-pi/2, pi/2]).
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT /
 *         ALP_ERR_NOT_READY / ALP_ERR_IO.
 */
alp_status_t alp_tmu_atan(float in_a, float *out);

/**
 * @brief Compute atan2(@p in_a, @p in_b).
 *
 * Returns the principal arctangent of @p in_a / @p in_b, using the
 * signs of both arguments to pick the correct quadrant.
 *
 * @param[in]  in_a    Numerator (y).
 * @param[in]  in_b    Denominator (x).
 * @param[out] out     Result in radians, in [-pi, pi].
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT /
 *         ALP_ERR_NOT_READY / ALP_ERR_IO.
 */
alp_status_t alp_tmu_atan2(float in_a, float in_b, float *out);

/**
 * @brief Compute sqrt(@p in_a).
 *
 * @param[in]  in_a    Non-negative argument.
 * @param[out] out     Result (sqrt(in_a)).
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT /
 *         ALP_ERR_NOT_READY / ALP_ERR_OUT_OF_RANGE (V2N backend, for
 *         negative inputs) / ALP_ERR_IO.
 */
alp_status_t alp_tmu_sqrt(float in_a, float *out);

/**
 * @brief Compute the natural logarithm log(@p in_a).
 *
 * @param[in]  in_a    Strictly positive argument.
 * @param[out] out     Result (ln(in_a)).
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT /
 *         ALP_ERR_NOT_READY / ALP_ERR_OUT_OF_RANGE (V2N backend, for
 *         @c in_a <= 0) / ALP_ERR_IO.
 */
alp_status_t alp_tmu_log(float in_a, float *out);

/**
 * @brief Compute exp(@p in_a).
 *
 * @param[in]  in_a    Argument.
 * @param[out] out     Result (e^in_a).
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT /
 *         ALP_ERR_NOT_READY / ALP_ERR_IO.
 */
alp_status_t alp_tmu_exp(float in_a, float *out);

/**
 * @brief Compute hyperbolic sine sinh(@p in_a).
 *
 * @param[in]  in_a    Argument.
 * @param[out] out     Result.
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT /
 *         ALP_ERR_NOT_READY / ALP_ERR_IO.
 */
alp_status_t alp_tmu_sinh(float in_a, float *out);

/**
 * @brief Compute hyperbolic cosine cosh(@p in_a).
 *
 * @param[in]  in_a    Argument.
 * @param[out] out     Result.
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT /
 *         ALP_ERR_NOT_READY / ALP_ERR_IO.
 */
alp_status_t alp_tmu_cosh(float in_a, float *out);

/**
 * @brief Compute hyperbolic tangent tanh(@p in_a).
 *
 * @param[in]  in_a    Argument.
 * @param[out] out     Result.
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT /
 *         ALP_ERR_NOT_READY / ALP_ERR_IO.
 */
alp_status_t alp_tmu_tanh(float in_a, float *out);

/**
 * @brief Compute the Euclidean vector magnitude sqrt(@p in_a^2 + @p in_b^2).
 *
 * Equivalent to C99's `hypotf(in_a, in_b)`; on the V2N backend the
 * call dispatches through the GD32G5's CORDIC "modulus" mode, which
 * gives a single-pass result without the intermediate overflow risks
 * of a naive @c sqrtf(@p in_a * @p in_a + @p in_b * @p in_b).
 *
 * @param[in]  in_a    First component.
 * @param[in]  in_b    Second component.
 * @param[out] out     Result (vector magnitude).
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT /
 *         ALP_ERR_NOT_READY / ALP_ERR_IO.
 */
alp_status_t alp_tmu_hypot(float in_a, float in_b, float *out);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_TMU_H */
