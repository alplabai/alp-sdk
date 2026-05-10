/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/inference.h>.
 *
 * Replaces the v0.1 NOSUPPORT stub.  This file owns the public-API
 * glue (handle pool, cfg validation, backend selector dispatch) and
 * delegates the actual neural-network execution to a per-backend
 * helper compiled in only when the matching CONFIG_ flag is on.
 *
 * Backend dispatch
 *   ALP_INFERENCE_BACKEND_AUTO    -> picks the first available real
 *                                    backend in this order: ETHOS_U,
 *                                    DRPAI, DEEPX_DX, CPU.  Falls back
 *                                    to ALP_ERR_NOSUPPORT when none is
 *                                    enabled at build time.
 *   ALP_INFERENCE_BACKEND_CPU     -> TFLM reference kernels.  Lands
 *                                    behind CONFIG_ALP_SDK_INFERENCE_TFLM
 *                                    in v0.2.x once tflite-micro is
 *                                    pinned in west.yml.
 *   ALP_INFERENCE_BACKEND_ETHOS_U -> TFLM + Arm Ethos-U op resolver.
 *                                    Same gate as CPU.  Real on AEN
 *                                    HW; native_sim falls back to
 *                                    NOSUPPORT.
 *   ALP_INFERENCE_BACKEND_DRPAI   -> v0.3 (Renesas DRP-AI translator).
 *   ALP_INFERENCE_BACKEND_DEEPX_DX-> v0.4 (DEEPX SDK adapter).
 *
 * The wrapper itself is always compiled so apps can link against
 * <alp/inference.h> on every target; the real backends slot in
 * behind their CONFIG_ flags without changing the public surface.
 */

#include <errno.h>
#include <string.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "alp/inference.h"
#include "handles.h"

/* ------------------------------------------------------------------ */
/* Pool size                                                           */
/* ------------------------------------------------------------------ */

#ifndef CONFIG_ALP_SDK_MAX_INFERENCE_HANDLES
#define CONFIG_ALP_SDK_MAX_INFERENCE_HANDLES 2
#endif

/* ------------------------------------------------------------------ */
/* Internal handle structure                                           */
/* ------------------------------------------------------------------ */

struct alp_inference {
    bool                    in_use;
    alp_inference_backend_t backend;
    /* Backend-specific state lives in a void *; the per-backend
     * source file (inference_tflm.cpp, inference_drpai.c, ...)
     * casts to its own type.  Lifetime tied to the handle. */
    void *be_state;
};

static struct alp_inference  g_inference_pool[CONFIG_ALP_SDK_MAX_INFERENCE_HANDLES];

static struct alp_inference *inference_pool_acquire(void)
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

static void inference_pool_release(struct alp_inference *h)
{
    if (h != NULL) h->in_use = false;
}

/* ------------------------------------------------------------------ */
/* Per-backend hook declarations                                       */
/*                                                                     */
/* Each backend's source file (compiled only when its CONFIG_ flag    */
/* is on) provides these symbols.  The dispatcher below picks one.    */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_ALP_SDK_INFERENCE_TFLM)
alp_status_t alp_inference_tflm_open(struct alp_inference *h, const alp_inference_config_t *cfg);
size_t       alp_inference_tflm_num_inputs(struct alp_inference *h);
size_t       alp_inference_tflm_num_outputs(struct alp_inference *h);
alp_status_t alp_inference_tflm_get_input(struct alp_inference *h, size_t index,
                                          alp_inference_tensor_t *out);
alp_status_t alp_inference_tflm_get_output(struct alp_inference *h, size_t index,
                                           alp_inference_tensor_t *out);
alp_status_t alp_inference_tflm_invoke(struct alp_inference *h);
void         alp_inference_tflm_close(struct alp_inference *h);
#endif

/* ------------------------------------------------------------------ */
/* Backend availability flags                                          */
/* ------------------------------------------------------------------ */

#define ALP_BE_HAS_TFLM defined(CONFIG_ALP_SDK_INFERENCE_TFLM)
#define ALP_BE_HAS_ETHOS_U defined(CONFIG_ALP_SDK_INFERENCE_ETHOS_U)
#define ALP_BE_HAS_DRPAI 0    /* v0.3 */
#define ALP_BE_HAS_DEEPX_DX 0 /* v0.4 */

#if ALP_BE_HAS_TFLM || ALP_BE_HAS_ETHOS_U
#define ALP_INFERENCE_HAS_REAL_BACKEND 1
#else
#define ALP_INFERENCE_HAS_REAL_BACKEND 0
#endif

/* ------------------------------------------------------------------ */
/* Auto-select policy                                                  */
/* ------------------------------------------------------------------ */

static alp_inference_backend_t resolve_auto(void)
{
#if ALP_BE_HAS_ETHOS_U
    return ALP_INFERENCE_BACKEND_ETHOS_U;
#elif ALP_BE_HAS_TFLM
    return ALP_INFERENCE_BACKEND_CPU;
#else
    return ALP_INFERENCE_BACKEND_AUTO; /* signals "nothing available" */
#endif
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

alp_inference_t *alp_inference_open(const alp_inference_config_t *cfg)
{
    alp_z_clear_last_error();

    if (cfg == NULL || cfg->model_data == NULL || cfg->model_size == 0) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

    alp_inference_backend_t backend = cfg->backend;
    if (backend == ALP_INFERENCE_BACKEND_AUTO) {
        backend = resolve_auto();
        if (backend == ALP_INFERENCE_BACKEND_AUTO) {
            /* No backend compiled in.  Honour the v0.1 contract -- the
             * SDK shipped the surface so apps can link cleanly; the
             * runtime answer is NOSUPPORT until a backend lands. */
            alp_z_set_last_error(ALP_ERR_NOSUPPORT);
            return NULL;
        }
    }

    struct alp_inference *h = inference_pool_acquire();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    h->backend      = backend;

    alp_status_t rc = ALP_ERR_NOSUPPORT;
    switch (backend) {
#if defined(CONFIG_ALP_SDK_INFERENCE_TFLM)
    case ALP_INFERENCE_BACKEND_CPU:
    case ALP_INFERENCE_BACKEND_ETHOS_U:
        rc = alp_inference_tflm_open(h, cfg);
        break;
#endif
    default:
        rc = ALP_ERR_NOSUPPORT;
        break;
    }

    if (rc != ALP_OK) {
        alp_z_set_last_error(rc);
        inference_pool_release(h);
        return NULL;
    }
    return h;
}

size_t alp_inference_num_inputs(alp_inference_t *inf)
{
    if (inf == NULL || !inf->in_use) return 0u;
    switch (inf->backend) {
#if defined(CONFIG_ALP_SDK_INFERENCE_TFLM)
    case ALP_INFERENCE_BACKEND_CPU:
    case ALP_INFERENCE_BACKEND_ETHOS_U:
        return alp_inference_tflm_num_inputs(inf);
#endif
    default:
        return 0u;
    }
}

size_t alp_inference_num_outputs(alp_inference_t *inf)
{
    if (inf == NULL || !inf->in_use) return 0u;
    switch (inf->backend) {
#if defined(CONFIG_ALP_SDK_INFERENCE_TFLM)
    case ALP_INFERENCE_BACKEND_CPU:
    case ALP_INFERENCE_BACKEND_ETHOS_U:
        return alp_inference_tflm_num_outputs(inf);
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
#if defined(CONFIG_ALP_SDK_INFERENCE_TFLM)
    case ALP_INFERENCE_BACKEND_CPU:
    case ALP_INFERENCE_BACKEND_ETHOS_U:
        return alp_inference_tflm_get_input(inf, index, out);
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
#if defined(CONFIG_ALP_SDK_INFERENCE_TFLM)
    case ALP_INFERENCE_BACKEND_CPU:
    case ALP_INFERENCE_BACKEND_ETHOS_U:
        return alp_inference_tflm_get_output(inf, index, out);
#endif
    default:
        return ALP_ERR_NOSUPPORT;
    }
}

alp_status_t alp_inference_invoke(alp_inference_t *inf)
{
    if (inf == NULL || !inf->in_use) return ALP_ERR_NOT_READY;
    switch (inf->backend) {
#if defined(CONFIG_ALP_SDK_INFERENCE_TFLM)
    case ALP_INFERENCE_BACKEND_CPU:
    case ALP_INFERENCE_BACKEND_ETHOS_U:
        return alp_inference_tflm_invoke(inf);
#endif
    default:
        return ALP_ERR_NOSUPPORT;
    }
}

void alp_inference_close(alp_inference_t *inf)
{
    if (inf == NULL || !inf->in_use) return;
    switch (inf->backend) {
#if defined(CONFIG_ALP_SDK_INFERENCE_TFLM)
    case ALP_INFERENCE_BACKEND_CPU:
    case ALP_INFERENCE_BACKEND_ETHOS_U:
        alp_inference_tflm_close(inf);
        break;
#endif
    default:
        break;
    }
    inference_pool_release(inf);
}
