/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Inference class dispatcher.  Owns the public alp_inference_*
 * API surface and routes through the backend registry mechanism
 * shipped in Slice 0 (PR #17).
 *
 * The handle struct layout (struct alp_inference) lives in
 * src/backends/inference/inference_ops.h so per-backend .c / .cpp
 * files can reach the fields without duplicating the layout.
 *
 * Backend-pin semantics
 *   - ALP_INFERENCE_BACKEND_AUTO -> alp_backend_select() walks the
 *                                   registry and picks by priority.
 *   - Any non-AUTO selector       -> the dispatcher first calls
 *                                   alp_backend_select() to find the
 *                                   silicon-bound choice, then asks
 *                                   the backend's `open()` to honour
 *                                   the caller's preference; the open
 *                                   hook may return NOSUPPORT if the
 *                                   pinned variant doesn't match what
 *                                   that backend can serve (e.g. CPU-
 *                                   pinned model on an Ethos-U-only
 *                                   build).
 *
 * The customer-visible enum (CPU / ETHOS_U / DRPAI / DEEPX_DX) in
 * <alp/inference.h> is forwarded through the dispatcher into
 * state.backend_id so the picked backend sees the original intent.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/inference.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "backends/inference/inference_ops.h"

ALP_BACKEND_DEFINE_CLASS(inference);

/* Reuse the existing TLS-backed last-error mechanism from
 * src/zephyr/last_error.c.  Forward-declared here to avoid
 * pulling in the broader handles.h header. */
extern void alp_z_set_last_error(alp_status_t s);
extern void alp_z_clear_last_error(void);

#ifndef CONFIG_ALP_SDK_MAX_INFERENCE_HANDLES
#define CONFIG_ALP_SDK_MAX_INFERENCE_HANDLES 2
#endif

static struct alp_inference _pool[CONFIG_ALP_SDK_MAX_INFERENCE_HANDLES];

static struct alp_inference *_alloc(void)
{
    for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_INFERENCE_HANDLES; ++i) {
        if (!_pool[i].in_use) {
            memset(&_pool[i], 0, sizeof(_pool[i]));
            _pool[i].in_use = true;
            return &_pool[i];
        }
    }
    return NULL;
}

static void _free(struct alp_inference *h)
{
    h->in_use = false;
}

alp_inference_t *alp_inference_open(const alp_inference_config_t *cfg)
{
    alp_z_clear_last_error();

    if (cfg == NULL || cfg->model_data == NULL || cfg->model_size == 0u) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

    const alp_backend_t *be = alp_backend_select("inference", ALP_SOC_REF_STR);
    if (be == NULL) {
        alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
        return NULL;
    }
    const alp_inference_ops_t *ops = (const alp_inference_ops_t *)be->ops;
    if (ops == NULL || ops->open == NULL) {
        alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
        return NULL;
    }
    struct alp_inference *h = _alloc();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    h->backend          = be;
    h->state.ops        = ops;
    h->state.backend_id = cfg->backend;
    alp_capabilities_t caps = { .flags = be->base_caps };
    alp_status_t rc = ops->open(cfg, &h->state, &caps);
    if (rc != ALP_OK) {
        _free(h);
        alp_z_set_last_error(rc);
        return NULL;
    }
    h->cached_caps = caps;
    return h;
}

size_t alp_inference_num_inputs(alp_inference_t *inf)
{
    if (inf == NULL || !inf->in_use) return 0u;
    if (inf->state.ops == NULL || inf->state.ops->num_inputs == NULL) return 0u;
    return inf->state.ops->num_inputs(&inf->state);
}

size_t alp_inference_num_outputs(alp_inference_t *inf)
{
    if (inf == NULL || !inf->in_use) return 0u;
    if (inf->state.ops == NULL || inf->state.ops->num_outputs == NULL) return 0u;
    return inf->state.ops->num_outputs(&inf->state);
}

alp_status_t alp_inference_get_input(alp_inference_t *inf, size_t index,
                                     alp_inference_tensor_t *out)
{
    if (inf == NULL || !inf->in_use) return ALP_ERR_NOT_READY;
    if (out == NULL) return ALP_ERR_INVAL;
    *out = (alp_inference_tensor_t){0};
    if (inf->state.ops == NULL || inf->state.ops->get_input == NULL) {
        return ALP_ERR_NOT_IMPLEMENTED;
    }
    return inf->state.ops->get_input(&inf->state, index, out);
}

alp_status_t alp_inference_get_output(alp_inference_t *inf, size_t index,
                                      alp_inference_tensor_t *out)
{
    if (inf == NULL || !inf->in_use) return ALP_ERR_NOT_READY;
    if (out == NULL) return ALP_ERR_INVAL;
    *out = (alp_inference_tensor_t){0};
    if (inf->state.ops == NULL || inf->state.ops->get_output == NULL) {
        return ALP_ERR_NOT_IMPLEMENTED;
    }
    return inf->state.ops->get_output(&inf->state, index, out);
}

alp_status_t alp_inference_invoke(alp_inference_t *inf)
{
    if (inf == NULL || !inf->in_use) return ALP_ERR_NOT_READY;
    if (inf->state.ops == NULL || inf->state.ops->invoke == NULL) {
        return ALP_ERR_NOT_IMPLEMENTED;
    }
    return inf->state.ops->invoke(&inf->state);
}

void alp_inference_close(alp_inference_t *inf)
{
    if (inf == NULL || !inf->in_use) return;
    if (inf->state.ops != NULL && inf->state.ops->close != NULL) {
        inf->state.ops->close(&inf->state);
    }
    _free(inf);
}

const alp_capabilities_t *alp_inference_capabilities(const alp_inference_t *inf)
{
    return (inf != NULL && inf->in_use) ? &inf->cached_caps : NULL;
}
