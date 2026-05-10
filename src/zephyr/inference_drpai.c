/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Renesas DRP-AI3 backend hook for <alp/inference.h>.
 *
 * Compiled only when CONFIG_ALP_SDK_INFERENCE_DRPAI=y.  The
 * dispatcher in inference_zephyr.c calls the
 * `alp_inference_drpai_*` symbols declared here when the active
 * backend is ALP_INFERENCE_BACKEND_DRPAI.
 *
 * Real impl wires Renesas's DRP-AI translator runtime (`drpai_*`
 * IOCTLs on the device's `/dev/drpai0` on Linux, or the FSP
 * `r_drpai` driver on bare-metal).  The translator output is a
 * `.dat` blob produced offline by the Renesas DRP-AI translator
 * tooling -- this wrapper takes that blob via cfg.model_data and
 * primes the NPU's command-stream-decoder for inference.
 *
 * v0.3 ships the dispatcher hook with the Renesas SDK call sites
 * marked TODO; v0.4 fills the bodies once the vendor pack lands
 * in CI alongside the V2N HIL bring-up.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/inference.h"
#include "handles.h"

/* Mirror of the dispatcher's struct alp_inference layout so we
 * can read be_state without exposing the type. */
struct alp_inference_handle_layout {
    bool                    in_use;
    alp_inference_backend_t backend;
    void                   *be_state;
};

struct drpai_state {
    /* DRP-AI translator output blob the caller provided.  The
     * actual NPU prime/invoke calls land in v0.4 -- this struct
     * is the place where Renesas's drpai_status_t + cmd-stream
     * descriptors will live. */
    const void *model_data;
    size_t      model_size;
};

alp_status_t alp_inference_drpai_open(struct alp_inference *h_, const alp_inference_config_t *cfg)
{
    struct alp_inference_handle_layout *h = (struct alp_inference_handle_layout *)h_;
    /* Renesas DRP-AI translator output is a .dat blob; future
     * versions of the format are detected via the magic bytes at
     * the head.  v0.3 just stores the pointer + length so the
     * v0.4 invoke path has them ready. */
    static struct drpai_state s_state;
    s_state.model_data = cfg->model_data;
    s_state.model_size = cfg->model_size;
    h->be_state        = &s_state;
    /* Returning ALP_OK lets apps verify dispatch routing today;
     * the actual NPU prime call lands v0.4 -- num_inputs and
     * num_outputs report 0 until then. */
    return ALP_OK;
}

size_t alp_inference_drpai_num_inputs(struct alp_inference *h)
{
    (void)h;
    /* Translator output exposes input shape; v0.4 reads it. */
    return 0u;
}

size_t alp_inference_drpai_num_outputs(struct alp_inference *h)
{
    (void)h;
    return 0u;
}

alp_status_t alp_inference_drpai_get_input(struct alp_inference *h, size_t index,
                                           alp_inference_tensor_t *out)
{
    (void)h;
    (void)index;
    (void)out;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_inference_drpai_get_output(struct alp_inference *h, size_t index,
                                            alp_inference_tensor_t *out)
{
    (void)h;
    (void)index;
    (void)out;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_inference_drpai_invoke(struct alp_inference *h)
{
    (void)h;
    /* drpai_start + wait-for-completion lands v0.4. */
    return ALP_ERR_NOSUPPORT;
}

void alp_inference_drpai_close(struct alp_inference *h)
{
    struct alp_inference_handle_layout *layout = (struct alp_inference_handle_layout *)h;
    layout->be_state                           = NULL;
}
