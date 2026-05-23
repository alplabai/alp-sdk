/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software TMU fallback (libm).  Wildcard backend at priority 0 --
 * picked only when no higher-priority hardware-aware backend is
 * linked into the build (the typical native_sim trimmed-image case).
 * Every op routes through the standard <math.h> float-precision
 * functions sinf / cosf / tanf / atanf / atan2f / sqrtf / logf /
 * expf / sinhf / coshf / tanhf / hypotf -- the same kernels the
 * zephyr_drv backend uses on non-V2N SoMs, but without any compile-
 * time gating around the V2N supervisor singleton.
 *
 * Matches the design spec Section 5 sw_fallback contract.
 *
 * @par Cost: ROM ~2-4 KB (libm linkage; depends on which math
 *      functions the rest of the firmware already pulls in -- if
 *      libm is already linked, marginal cost is ~24 fn-pointer
 *      bytes for the ops vtable).  RAM 0 B (stateless, no
 *      per-handle backend state because TMU has no handle).
 * @par Performance: One libm call per op.  Slower than a CORDIC
 *      hardware TMU but portable and bit-identical to a libm-on-
 *      host reference, which is the property the unit tests
 *      exercise (alp_tmu_sin(0.0f, &out) -> out == sinf(0.0f)).
 *      All ops are reentrant and lock-free.
 */

#include <math.h>
#include <stddef.h>

#include <alp/backend.h>
#include <alp/peripheral.h>
#include <alp/tmu.h>

#include "tmu_ops.h"

static alp_status_t sw_sin(float in_a, float *out)
{
    *out = sinf(in_a);
    return ALP_OK;
}

static alp_status_t sw_cos(float in_a, float *out)
{
    *out = cosf(in_a);
    return ALP_OK;
}

static alp_status_t sw_tan(float in_a, float *out)
{
    *out = tanf(in_a);
    return ALP_OK;
}

static alp_status_t sw_atan(float in_a, float *out)
{
    *out = atanf(in_a);
    return ALP_OK;
}

static alp_status_t sw_atan2(float in_a, float in_b, float *out)
{
    *out = atan2f(in_a, in_b);
    return ALP_OK;
}

static alp_status_t sw_sqrt(float in_a, float *out)
{
    *out = sqrtf(in_a);
    return ALP_OK;
}

static alp_status_t sw_log(float in_a, float *out)
{
    *out = logf(in_a);
    return ALP_OK;
}

static alp_status_t sw_exp(float in_a, float *out)
{
    *out = expf(in_a);
    return ALP_OK;
}

static alp_status_t sw_sinh(float in_a, float *out)
{
    *out = sinhf(in_a);
    return ALP_OK;
}

static alp_status_t sw_cosh(float in_a, float *out)
{
    *out = coshf(in_a);
    return ALP_OK;
}

static alp_status_t sw_tanh(float in_a, float *out)
{
    *out = tanhf(in_a);
    return ALP_OK;
}

static alp_status_t sw_hypot(float in_a, float in_b, float *out)
{
    *out = hypotf(in_a, in_b);
    return ALP_OK;
}

/* ---------- Registration ---------- */

static const alp_tmu_ops_t _ops = {
    .sin   = sw_sin,
    .cos   = sw_cos,
    .tan   = sw_tan,
    .atan  = sw_atan,
    .atan2 = sw_atan2,
    .sqrt  = sw_sqrt,
    .log   = sw_log,
    .exp   = sw_exp,
    .sinh  = sw_sinh,
    .cosh  = sw_cosh,
    .tanh  = sw_tanh,
    .hypot = sw_hypot,
};

ALP_BACKEND_REGISTER(tmu, sw_fallback, {
    .silicon_ref = "*",
    .vendor      = "sw",
    .base_caps   = 0u,
    .priority    = 0,
    .ops         = &_ops,
    .probe       = NULL,
});
