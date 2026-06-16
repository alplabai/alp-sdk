/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr backend for the <alp/tmu.h> surface.  Routes each
 * stateless math primitive to either the V2N GD32G5 TMU (CORDIC)
 * accelerator via the supervisor singleton, or to libm directly
 * when no hardware accelerator is on the active SoM.  The
 * customer-facing header is SoM-agnostic; this file is the only
 * place the GD32 name appears for TMU.
 *
 * Format selection on the bridge: the request payload carries the
 * format byte (Q31 vs IEEE-754 single).  This backend always uses
 * IEEE-754 single because the public surface is `float` and the
 * round-trip cost is one `memcpy`-style reinterpret each way.  The
 * GD32G5's TMU supports both natively; the choice is documented in
 * docs/gd32-bridge-protocol.md s3.x.
 *
 * Registry shape: silicon_ref="*" / vendor="zephyr" / priority 100.
 * Single wildcard backend covers every Zephyr-shaped SoM the SDK
 * supports; the libm-vs-bridge split is internal -- a build with
 * CONFIG_ALP_SDK_V2N_SUPERVISOR=y compiles the bridge path, every
 * other build compiles the libm path.
 */

#include <math.h>
#include <stddef.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/peripheral.h>
#include <alp/tmu.h>

#include "tmu_ops.h"

#if defined(CONFIG_ALP_SDK_V2N_SUPERVISOR)
#define ALP_TMU_HAS_BRIDGE_PATH 1
#include "alp/chips/gd32g553.h"
#include "v2n_supervisor.h"
#else
#define ALP_TMU_HAS_BRIDGE_PATH 0
#endif

#if ALP_TMU_HAS_BRIDGE_PATH
/* ------------------------------------------------------------------ */
/* float <-> wire-u32 reinterpret helpers.  Using `memcpy` (rather    */
/* than a union or a `*(uint32_t*)&f` punning cast) is the standard   */
/* C99 way to reinterpret bits without violating strict-aliasing.     */
/* Only compiled on bridge-path builds -- every caller sits inside    */
/* the same `#if ALP_TMU_HAS_BRIDGE_PATH` block, so leaving these at  */
/* file scope trips -Werror=unused-function on non-V2N builds         */
/* (CONFIG_ALP_SDK_V2N_SUPERVISOR=n, e.g. native_sim).                */
/* ------------------------------------------------------------------ */

static uint32_t f32_to_u32_bits(float f)
{
	uint32_t bits;
	memcpy(&bits, &f, sizeof bits);
	return bits;
}

static float u32_to_f32_bits(uint32_t bits)
{
	float f;
	memcpy(&f, &bits, sizeof f);
	return f;
}

/* Shared bridge path: acquire supervisor, send one CMD_TMU_COMPUTE,
 * release.  All twelve TMU ops share this body -- the per-op static
 * helpers below just pick the function id and fold their inputs
 * into u32 bits. */
static alp_status_t tmu_bridge_call(gd32g553_tmu_function_t function, float in_a, float in_b,
                                    float *out)
{
	gd32g553_t  *ctx = NULL;
	alp_status_t s   = alp_z_v2n_supervisor_acquire(&ctx);
	if (s != ALP_OK) return s;

	uint32_t result_bits = 0u;
	s = gd32g553_tmu_compute(ctx, function, GD32G553_TMU_FMT_F32, f32_to_u32_bits(in_a),
	                         f32_to_u32_bits(in_b), &result_bits);
	alp_z_v2n_supervisor_release();
	if (s != ALP_OK) return s;
	*out = u32_to_f32_bits(result_bits);
	return ALP_OK;
}
#endif

/* ------------------------------------------------------------------ */
/* Per-op static helpers.  The function pointers in the ops vtable    */
/* point at these; the dispatcher walks ops->sin / cos / ...          */
/* directly.  Each helper short-circuits NULL-out at the dispatcher   */
/* layer already, so the body assumes a non-NULL out pointer.         */
/* ------------------------------------------------------------------ */

static alp_status_t z_sin(float in_a, float *out)
{
#if ALP_TMU_HAS_BRIDGE_PATH
	return tmu_bridge_call(GD32G553_TMU_FN_SIN, in_a, 0.0f, out);
#else
	*out = sinf(in_a);
	return ALP_OK;
#endif
}

static alp_status_t z_cos(float in_a, float *out)
{
#if ALP_TMU_HAS_BRIDGE_PATH
	return tmu_bridge_call(GD32G553_TMU_FN_COS, in_a, 0.0f, out);
#else
	*out = cosf(in_a);
	return ALP_OK;
#endif
}

static alp_status_t z_tan(float in_a, float *out)
{
#if ALP_TMU_HAS_BRIDGE_PATH
	return tmu_bridge_call(GD32G553_TMU_FN_TAN, in_a, 0.0f, out);
#else
	*out = tanf(in_a);
	return ALP_OK;
#endif
}

static alp_status_t z_atan(float in_a, float *out)
{
#if ALP_TMU_HAS_BRIDGE_PATH
	return tmu_bridge_call(GD32G553_TMU_FN_ATAN, in_a, 0.0f, out);
#else
	*out = atanf(in_a);
	return ALP_OK;
#endif
}

static alp_status_t z_atan2(float in_a, float in_b, float *out)
{
#if ALP_TMU_HAS_BRIDGE_PATH
	return tmu_bridge_call(GD32G553_TMU_FN_ATAN2, in_a, in_b, out);
#else
	*out = atan2f(in_a, in_b);
	return ALP_OK;
#endif
}

static alp_status_t z_sqrt(float in_a, float *out)
{
#if ALP_TMU_HAS_BRIDGE_PATH
	return tmu_bridge_call(GD32G553_TMU_FN_SQRT, in_a, 0.0f, out);
#else
	*out = sqrtf(in_a);
	return ALP_OK;
#endif
}

static alp_status_t z_log(float in_a, float *out)
{
#if ALP_TMU_HAS_BRIDGE_PATH
	return tmu_bridge_call(GD32G553_TMU_FN_LOG, in_a, 0.0f, out);
#else
	*out = logf(in_a);
	return ALP_OK;
#endif
}

static alp_status_t z_exp(float in_a, float *out)
{
#if ALP_TMU_HAS_BRIDGE_PATH
	return tmu_bridge_call(GD32G553_TMU_FN_EXP, in_a, 0.0f, out);
#else
	*out = expf(in_a);
	return ALP_OK;
#endif
}

static alp_status_t z_sinh(float in_a, float *out)
{
#if ALP_TMU_HAS_BRIDGE_PATH
	return tmu_bridge_call(GD32G553_TMU_FN_SINH, in_a, 0.0f, out);
#else
	*out = sinhf(in_a);
	return ALP_OK;
#endif
}

static alp_status_t z_cosh(float in_a, float *out)
{
#if ALP_TMU_HAS_BRIDGE_PATH
	return tmu_bridge_call(GD32G553_TMU_FN_COSH, in_a, 0.0f, out);
#else
	*out = coshf(in_a);
	return ALP_OK;
#endif
}

static alp_status_t z_tanh(float in_a, float *out)
{
#if ALP_TMU_HAS_BRIDGE_PATH
	return tmu_bridge_call(GD32G553_TMU_FN_TANH, in_a, 0.0f, out);
#else
	*out = tanhf(in_a);
	return ALP_OK;
#endif
}

static alp_status_t z_hypot(float in_a, float in_b, float *out)
{
#if ALP_TMU_HAS_BRIDGE_PATH
	return tmu_bridge_call(GD32G553_TMU_FN_HYPOT, in_a, in_b, out);
#else
	*out = hypotf(in_a, in_b);
	return ALP_OK;
#endif
}

/* ---------- Registration ---------- */

static const alp_tmu_ops_t _ops = {
	.sin   = z_sin,
	.cos   = z_cos,
	.tan   = z_tan,
	.atan  = z_atan,
	.atan2 = z_atan2,
	.sqrt  = z_sqrt,
	.log   = z_log,
	.exp   = z_exp,
	.sinh  = z_sinh,
	.cosh  = z_cosh,
	.tanh  = z_tanh,
	.hypot = z_hypot,
};

ALP_BACKEND_REGISTER(tmu, zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
