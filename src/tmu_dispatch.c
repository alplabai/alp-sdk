/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * TMU class dispatcher.  Owns the public <alp/tmu.h> surface
 * (twelve stateless float math primitives -- sin / cos / tan /
 * atan / atan2 / sqrt / log / exp / sinh / cosh / tanh / hypot)
 * on top of the backend registry mechanism shipped in Slice 0
 * (PR #17).
 *
 * Unlike the rest of the registry family (USB / mproc / MQTT /
 * RPC / WDT / ...), the TMU surface has no `alp_tmu_open` and no
 * handle type -- every primitive is stateless.  Consequently:
 *
 *   - there is no handle pool;
 *   - there is no per-call backend-state allocation;
 *   - the dispatcher caches the selected backend's ops vtable
 *     on first call and routes every subsequent alp_tmu_* through
 *     the cached pointer.
 *
 * Two backends register at build time -- the portable Zephyr /
 * V2N-bridge backend at priority 100 and the libm SW fallback at
 * priority 0 (wildcard).  alp_backend_select() does the actual
 * pick on first use; the cache here is a pure microoptimisation so
 * the per-call hot path is two loads + one branch + an indirect
 * call.
 *
 * Thread safety: the per-call work does not need a mutex (the
 * inner GD32 bridge acquires the V2N supervisor itself; the libm
 * path is reentrant), so the cache here is lock-free.  The cached
 * pointer is written exactly once; in the worst case two threads
 * race the first call and both observe the same value
 * `alp_backend_select` computes deterministically.
 */

#include <stdbool.h>
#include <stddef.h>

#include <alp/backend.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>
#include <alp/tmu.h>

#include "backends/tmu/tmu_ops.h"

ALP_BACKEND_DEFINE_CLASS(tmu);

static const alp_tmu_ops_t *_cached_ops = NULL;

static const alp_tmu_ops_t *_get_ops(void)
{
    if (_cached_ops != NULL) {
        return _cached_ops;
    }
    const alp_backend_t *be = alp_backend_select("tmu", ALP_SOC_REF_STR);
    if (be == NULL) {
        return NULL;
    }
    _cached_ops = (const alp_tmu_ops_t *)be->ops;
    return _cached_ops;
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

alp_status_t alp_tmu_sin(float in_a, float *out)
{
    if (out == NULL) return ALP_ERR_INVAL;
    const alp_tmu_ops_t *ops = _get_ops();
    if (ops == NULL || ops->sin == NULL) return ALP_ERR_NOT_IMPLEMENTED;
    return ops->sin(in_a, out);
}

alp_status_t alp_tmu_cos(float in_a, float *out)
{
    if (out == NULL) return ALP_ERR_INVAL;
    const alp_tmu_ops_t *ops = _get_ops();
    if (ops == NULL || ops->cos == NULL) return ALP_ERR_NOT_IMPLEMENTED;
    return ops->cos(in_a, out);
}

alp_status_t alp_tmu_tan(float in_a, float *out)
{
    if (out == NULL) return ALP_ERR_INVAL;
    const alp_tmu_ops_t *ops = _get_ops();
    if (ops == NULL || ops->tan == NULL) return ALP_ERR_NOT_IMPLEMENTED;
    return ops->tan(in_a, out);
}

alp_status_t alp_tmu_atan(float in_a, float *out)
{
    if (out == NULL) return ALP_ERR_INVAL;
    const alp_tmu_ops_t *ops = _get_ops();
    if (ops == NULL || ops->atan == NULL) return ALP_ERR_NOT_IMPLEMENTED;
    return ops->atan(in_a, out);
}

alp_status_t alp_tmu_atan2(float in_a, float in_b, float *out)
{
    if (out == NULL) return ALP_ERR_INVAL;
    const alp_tmu_ops_t *ops = _get_ops();
    if (ops == NULL || ops->atan2 == NULL) return ALP_ERR_NOT_IMPLEMENTED;
    return ops->atan2(in_a, in_b, out);
}

alp_status_t alp_tmu_sqrt(float in_a, float *out)
{
    if (out == NULL) return ALP_ERR_INVAL;
    const alp_tmu_ops_t *ops = _get_ops();
    if (ops == NULL || ops->sqrt == NULL) return ALP_ERR_NOT_IMPLEMENTED;
    return ops->sqrt(in_a, out);
}

alp_status_t alp_tmu_log(float in_a, float *out)
{
    if (out == NULL) return ALP_ERR_INVAL;
    const alp_tmu_ops_t *ops = _get_ops();
    if (ops == NULL || ops->log == NULL) return ALP_ERR_NOT_IMPLEMENTED;
    return ops->log(in_a, out);
}

alp_status_t alp_tmu_exp(float in_a, float *out)
{
    if (out == NULL) return ALP_ERR_INVAL;
    const alp_tmu_ops_t *ops = _get_ops();
    if (ops == NULL || ops->exp == NULL) return ALP_ERR_NOT_IMPLEMENTED;
    return ops->exp(in_a, out);
}

alp_status_t alp_tmu_sinh(float in_a, float *out)
{
    if (out == NULL) return ALP_ERR_INVAL;
    const alp_tmu_ops_t *ops = _get_ops();
    if (ops == NULL || ops->sinh == NULL) return ALP_ERR_NOT_IMPLEMENTED;
    return ops->sinh(in_a, out);
}

alp_status_t alp_tmu_cosh(float in_a, float *out)
{
    if (out == NULL) return ALP_ERR_INVAL;
    const alp_tmu_ops_t *ops = _get_ops();
    if (ops == NULL || ops->cosh == NULL) return ALP_ERR_NOT_IMPLEMENTED;
    return ops->cosh(in_a, out);
}

alp_status_t alp_tmu_tanh(float in_a, float *out)
{
    if (out == NULL) return ALP_ERR_INVAL;
    const alp_tmu_ops_t *ops = _get_ops();
    if (ops == NULL || ops->tanh == NULL) return ALP_ERR_NOT_IMPLEMENTED;
    return ops->tanh(in_a, out);
}

alp_status_t alp_tmu_hypot(float in_a, float in_b, float *out)
{
    if (out == NULL) return ALP_ERR_INVAL;
    const alp_tmu_ops_t *ops = _get_ops();
    if (ops == NULL || ops->hypot == NULL) return ALP_ERR_NOT_IMPLEMENTED;
    return ops->hypot(in_a, in_b, out);
}
