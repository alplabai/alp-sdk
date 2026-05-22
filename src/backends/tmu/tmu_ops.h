/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between the alp_tmu dispatcher and per-backend
 * implementations.  TMU is unusual in the registry family: the
 * public surface in <alp/tmu.h> is a collection of STATELESS math
 * primitives (sin / cos / sqrt / ...) with no `alp_tmu_open` and no
 * handle type, so there is no backend state to thread through and
 * no handle-pool entry inside the dispatcher.  The vtable below is
 * therefore the only contract between the dispatcher and backend --
 * one function pointer per op, each carrying the same signature as
 * the matching public alp_tmu_* primitive.
 *
 * The dispatcher caches the selected backend's ops vtable on first
 * call (see src/tmu_dispatch.c) and every subsequent alp_tmu_* call
 * walks `_cached_ops->fn(...)` directly.
 *
 * NOT a public header.
 */

#ifndef ALP_BACKENDS_TMU_OPS_H
#define ALP_BACKENDS_TMU_OPS_H

#include <alp/backend.h>
#include <alp/peripheral.h>
#include <alp/tmu.h>

/* ------------------------------------------------------------------ */
/* Ops vtable                                                          */
/* ------------------------------------------------------------------ */

typedef struct alp_tmu_ops {
    alp_status_t (*sin)(float in_a, float *out);
    alp_status_t (*cos)(float in_a, float *out);
    alp_status_t (*tan)(float in_a, float *out);
    alp_status_t (*atan)(float in_a, float *out);
    alp_status_t (*atan2)(float in_a, float in_b, float *out);
    alp_status_t (*sqrt)(float in_a, float *out);
    alp_status_t (*log)(float in_a, float *out);
    alp_status_t (*exp)(float in_a, float *out);
    alp_status_t (*sinh)(float in_a, float *out);
    alp_status_t (*cosh)(float in_a, float *out);
    alp_status_t (*tanh)(float in_a, float *out);
    alp_status_t (*hypot)(float in_a, float in_b, float *out);
} alp_tmu_ops_t;

#endif /* ALP_BACKENDS_TMU_OPS_H */
