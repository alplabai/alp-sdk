/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Yocto / Linux-user-space backend for <alp/inference.h>.
 *
 * Mirrors the role of src/zephyr/inference_zephyr.c: owns the
 * public-API glue (handle pool, cfg validation, backend dispatch)
 * and delegates the actual neural-network execution to a per-backend
 * helper compiled in only when the matching ALP_SDK_USE_<BACKEND>
 * CMake option is on.
 *
 * Backend dispatch (Yocto / V2N-M1 priority)
 *   ALP_INFERENCE_BACKEND_AUTO     -> picks the first available real
 *                                     backend in this order: DEEPX_DX,
 *                                     ETHOS_U, DRPAI, CPU.  DEEPX_DX
 *                                     comes first on V2N-M1 builds
 *                                     because that's the SoM's reason
 *                                     for shipping a companion NPU.
 *   ALP_INFERENCE_BACKEND_DEEPX_DXM1 -> DEEPX DX-M1 (proprietary).  Real
 *                                     dispatch lands in v0.4 once
 *                                     deepx-dxm1-host-sdk is on the
 *                                     sysroot; v0.3 wires routing only.
 *   ALP_INFERENCE_BACKEND_ETHOS_U  -> reserved for the i.MX 93 Yocto
 *                                     path (task #14).  Adapter slot
 *                                     stays empty until that lands.
 *   ALP_INFERENCE_BACKEND_DRPAI    -> reserved for the V2N Yocto path
 *                                     (task #14 sibling).
 *   ALP_INFERENCE_BACKEND_CPU      -> TFLM reference kernels on the
 *                                     A55s.  Wiring deferred to v0.4
 *                                     alongside the meta-alp tflite
 *                                     recipe.
 *
 * This file overrides the inference stubs that src/common/stub_backend.c
 * would otherwise provide (the ALP_VENDOR_OVERRIDES_INFERENCE macro
 * gates them out of the link).  It is compiled unconditionally for the
 * Yocto OS backend so the SDK keeps a single owner of the
 * alp_inference_* symbols on Linux user-space, whether or not any
 * NPU adapter is enabled in this build.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "alp/inference.h"

#include "alp_internal.h"

#ifndef ALP_SDK_MAX_INFERENCE_HANDLES
#define ALP_SDK_MAX_INFERENCE_HANDLES 2
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* ------------------------------------------------------------------ */
/* Internal handle (shared across this TU + the per-backend files).    */
/*                                                                     */
/* The per-backend files (currently inference_deepx.cpp) cast a       */
/* matching layout struct to read back be_state without including this */
/* header -- same pattern inference_zephyr.c uses with                 */
/* inference_drpai.c.                                                  */
/* ------------------------------------------------------------------ */

struct alp_inference {
    bool                    in_use;
    alp_inference_backend_t backend;
    void                   *be_state;
};

static struct alp_inference  g_inference_pool[ALP_SDK_MAX_INFERENCE_HANDLES];

static struct alp_inference *pool_acquire(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_inference_pool); ++i) {
        if (!g_inference_pool[i].in_use) {
            memset(&g_inference_pool[i], 0, sizeof(g_inference_pool[i]));
            g_inference_pool[i].in_use = true;
            return &g_inference_pool[i];
        }
    }
    return NULL;
}

static void pool_release(struct alp_inference *h)
{
    if (h != NULL) h->in_use = false;
}

/* ------------------------------------------------------------------ */
/* Per-backend hook declarations                                       */
/*                                                                     */
/* Each backend's source file (compiled in only when its CMake option */
/* is on) provides these symbols.  Empty when no backend is on -- the */
/* dispatcher returns NOSUPPORT through the default switch arm.       */
/* ------------------------------------------------------------------ */

#if defined(ALP_SDK_USE_DEEPX_DXM1)
alp_status_t alp_inference_deepx_open(struct alp_inference *h, const alp_inference_config_t *cfg);
size_t       alp_inference_deepx_num_inputs(struct alp_inference *h);
size_t       alp_inference_deepx_num_outputs(struct alp_inference *h);
alp_status_t alp_inference_deepx_get_input(struct alp_inference *h, size_t index,
                                           alp_inference_tensor_t *out);
alp_status_t alp_inference_deepx_get_output(struct alp_inference *h, size_t index,
                                            alp_inference_tensor_t *out);
alp_status_t alp_inference_deepx_invoke(struct alp_inference *h);
void         alp_inference_deepx_close(struct alp_inference *h);
#endif

/* ------------------------------------------------------------------ */
/* Auto-select policy                                                  */
/*                                                                     */
/* On Yocto the prevailing reason for choosing AUTO is "use the NPU   */
/* on this SoM."  DEEPX_DX wins first on V2N-M1.  Ethos-U / DRP-AI    */
/* slots land alongside task #14; CPU TFLM lands v0.4.                */
/* ------------------------------------------------------------------ */

static alp_inference_backend_t resolve_auto(void)
{
#if defined(ALP_SDK_USE_DEEPX_DXM1)
    return ALP_INFERENCE_BACKEND_DEEPX_DXM1;
#else
    return ALP_INFERENCE_BACKEND_AUTO; /* signals "nothing available" */
#endif
}

/* ------------------------------------------------------------------ */
/* Last-error stamping                                                 */
/*                                                                     */
/* The dispatcher calls alp_internal_set_last_error (declared in       */
/* src/common/alp_internal.h) so failures here are visible through the */
/* public alp_last_error() reader -- the same slot the peripheral      */
/* stubs in stub_backend.c write to.  Cross-TU correlation works in    */
/* non-vendor-override builds (the yocto path doesn't set              */
/* ALP_VENDOR_OVERRIDES_PERIPHERAL, so this just works).               */
/* ------------------------------------------------------------------ */

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

alp_inference_t *alp_inference_open(const alp_inference_config_t *cfg)
{
    if (cfg == NULL || cfg->model_data == NULL || cfg->model_size == 0) {
        alp_internal_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

    alp_inference_backend_t backend = cfg->backend;
    if (backend == ALP_INFERENCE_BACKEND_AUTO) {
        backend = resolve_auto();
        if (backend == ALP_INFERENCE_BACKEND_AUTO) {
            /* Nothing compiled in.  Honour the v0.1 contract: surface
             * is shipped so apps link cleanly; the runtime answer is
             * NOSUPPORT until a backend lands. */
            alp_internal_set_last_error(ALP_ERR_NOSUPPORT);
            return NULL;
        }
    }

    struct alp_inference *h = pool_acquire();
    if (h == NULL) {
        alp_internal_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    h->backend      = backend;

    alp_status_t rc = ALP_ERR_NOSUPPORT;
    switch (backend) {
#if defined(ALP_SDK_USE_DEEPX_DXM1)
    case ALP_INFERENCE_BACKEND_DEEPX_DXM1:
        rc = alp_inference_deepx_open(h, cfg);
        break;
#endif
    default:
        rc = ALP_ERR_NOSUPPORT;
        break;
    }

    if (rc != ALP_OK) {
        alp_internal_set_last_error(rc);
        pool_release(h);
        return NULL;
    }
    return h;
}

size_t alp_inference_num_inputs(alp_inference_t *inf)
{
    if (inf == NULL || !inf->in_use) return 0u;
    switch (inf->backend) {
#if defined(ALP_SDK_USE_DEEPX_DXM1)
    case ALP_INFERENCE_BACKEND_DEEPX_DXM1:
        return alp_inference_deepx_num_inputs(inf);
#endif
    default:
        return 0u;
    }
}

size_t alp_inference_num_outputs(alp_inference_t *inf)
{
    if (inf == NULL || !inf->in_use) return 0u;
    switch (inf->backend) {
#if defined(ALP_SDK_USE_DEEPX_DXM1)
    case ALP_INFERENCE_BACKEND_DEEPX_DXM1:
        return alp_inference_deepx_num_outputs(inf);
#endif
    default:
        return 0u;
    }
}

alp_status_t alp_inference_get_input(alp_inference_t *inf, size_t index,
                                     alp_inference_tensor_t *out)
{
    if (inf == NULL || !inf->in_use) return ALP_ERR_NOT_READY;
    if (out == NULL) return ALP_ERR_INVAL;
    *out = (alp_inference_tensor_t){0};
    switch (inf->backend) {
#if defined(ALP_SDK_USE_DEEPX_DXM1)
    case ALP_INFERENCE_BACKEND_DEEPX_DXM1:
        return alp_inference_deepx_get_input(inf, index, out);
#endif
    default:
        return ALP_ERR_NOSUPPORT;
    }
}

alp_status_t alp_inference_get_output(alp_inference_t *inf, size_t index,
                                      alp_inference_tensor_t *out)
{
    if (inf == NULL || !inf->in_use) return ALP_ERR_NOT_READY;
    if (out == NULL) return ALP_ERR_INVAL;
    *out = (alp_inference_tensor_t){0};
    switch (inf->backend) {
#if defined(ALP_SDK_USE_DEEPX_DXM1)
    case ALP_INFERENCE_BACKEND_DEEPX_DXM1:
        return alp_inference_deepx_get_output(inf, index, out);
#endif
    default:
        return ALP_ERR_NOSUPPORT;
    }
}

alp_status_t alp_inference_invoke(alp_inference_t *inf)
{
    if (inf == NULL || !inf->in_use) return ALP_ERR_NOT_READY;
    switch (inf->backend) {
#if defined(ALP_SDK_USE_DEEPX_DXM1)
    case ALP_INFERENCE_BACKEND_DEEPX_DXM1:
        return alp_inference_deepx_invoke(inf);
#endif
    default:
        return ALP_ERR_NOSUPPORT;
    }
}

void alp_inference_close(alp_inference_t *inf)
{
    if (inf == NULL || !inf->in_use) return;
    switch (inf->backend) {
#if defined(ALP_SDK_USE_DEEPX_DXM1)
    case ALP_INFERENCE_BACKEND_DEEPX_DXM1:
        alp_inference_deepx_close(inf);
        break;
#endif
    default:
        break;
    }
    pool_release(inf);
}
