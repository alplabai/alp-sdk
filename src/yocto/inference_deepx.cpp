/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * DEEPX DX-M1 backend hook for <alp/inference.h>.
 *
 * Compiled only when ALP_SDK_USE_DEEPX_DXM1=ON (the CMake option set
 * by src/yocto/CMakeLists.txt, which the meta-alp e1m-x-v2n-m1
 * MACHINE recipe drives ON).  The dispatcher in
 * src/yocto/inference_yocto.c calls the alp_inference_deepx_*
 * symbols declared here when the active backend is
 * ALP_INFERENCE_BACKEND_DEEPX_DX.
 *
 * Real implementation bridges the DEEPX DX-M1 host SDK
 * (vendors/deepx-dxm1/include/dxnn/dxnn.h or the SDK-installed
 * /usr/include/dxnn/dxnn.h).  The model_data the caller passes via
 * cfg.model_data is a DXNN flatbuffer produced by the DEEPX compiler;
 * dxnn_load_model primes the device's command-stream decoder and
 * dxnn_run blocks until inference completes.
 *
 * v0.3 ships the dispatcher hook with the SDK call sites marked
 * TODO; v0.4 fills the bodies once the deepx-dxm1-host-sdk Yocto
 * recipe lands and CI's V2N-M1 HIL bring-up validates round-trip
 * inference on real silicon.  Until then alp_inference_deepx_open
 * returns ALP_OK so the dispatcher routing is exercised end-to-end,
 * but num_inputs / num_outputs report 0 and invoke returns NOSUPPORT.
 * Same shape as src/zephyr/inference_drpai.c.
 */

#include <cstddef>
#include <cstdint>

#include <dxnn/dxnn.h>

#include "alp/inference.h"

/* Mirror of the yocto dispatcher's struct alp_inference layout so we
 * can read be_state without exposing the type.  Must match
 * inference_yocto.c exactly.  Keep this in sync if the dispatcher's
 * fields change. */
struct alp_inference_handle_layout {
    bool                    in_use;
    alp_inference_backend_t backend;
    void                   *be_state;
};

namespace
{

struct DeepxState {
    /* DEEPX DXNN flatbuffer the caller provided.  v0.4 owns the
     * dxnn_runtime_t * and dxnn_model_t * once the SDK is on the
     * sysroot. */
    const void     *model_data;
    std::size_t     model_size;
    dxnn_runtime_t *rt;
    dxnn_model_t   *model;
};

DeepxState s_state{};

} /* namespace */

extern "C" alp_status_t alp_inference_deepx_open(struct alp_inference         *h_,
                                                 const alp_inference_config_t *cfg)
{
    auto *h = reinterpret_cast<alp_inference_handle_layout *>(h_);

    /* v0.4 wires dxnn_init() + dxnn_load_model() here.  Today we
     * record the inputs so the v0.4 invoke path has them ready and
     * leave the runtime / model pointers null.  Returning ALP_OK
     * lets the dispatcher routing be exercised by tests; the actual
     * inference is gated behind invoke -> NOSUPPORT. */
    s_state.model_data = cfg->model_data;
    s_state.model_size = cfg->model_size;
    s_state.rt         = nullptr;
    s_state.model      = nullptr;

    h->be_state        = &s_state;
    return ALP_OK;
}

extern "C" std::size_t alp_inference_deepx_num_inputs(struct alp_inference *h)
{
    (void)h;
    /* dxnn_num_inputs(s_state.model) lands v0.4. */
    return 0u;
}

extern "C" std::size_t alp_inference_deepx_num_outputs(struct alp_inference *h)
{
    (void)h;
    return 0u;
}

extern "C" alp_status_t alp_inference_deepx_get_input(struct alp_inference *h, std::size_t index,
                                                      alp_inference_tensor_t *out)
{
    (void)h;
    (void)index;
    (void)out;
    return ALP_ERR_NOSUPPORT;
}

extern "C" alp_status_t alp_inference_deepx_get_output(struct alp_inference *h, std::size_t index,
                                                       alp_inference_tensor_t *out)
{
    (void)h;
    (void)index;
    (void)out;
    return ALP_ERR_NOSUPPORT;
}

extern "C" alp_status_t alp_inference_deepx_invoke(struct alp_inference *h)
{
    (void)h;
    /* dxnn_run(s_state.model) lands v0.4. */
    return ALP_ERR_NOSUPPORT;
}

extern "C" void alp_inference_deepx_close(struct alp_inference *h_)
{
    auto *h = reinterpret_cast<alp_inference_handle_layout *>(h_);
    /* v0.4 calls dxnn_unload_model(s_state.model) + dxnn_destroy(s_state.rt). */
    s_state.model      = nullptr;
    s_state.rt         = nullptr;
    s_state.model_data = nullptr;
    s_state.model_size = 0;
    h->be_state        = nullptr;
}
