/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * TFLM-backed executor for <alp/inference.h>.
 *
 * Compiled only when CONFIG_ALP_SDK_INFERENCE_TFLM=y, which depends
 * on CONFIG_TENSORFLOW_LITE_MICRO=y (the upstream Zephyr module
 * pulled into west.yml).  inference_zephyr.c's dispatcher calls the
 * `alp_inference_tflm_*` hooks declared here when the active backend
 * is CPU or ETHOS_U.
 *
 * Why a separate .cpp file?  TFLM exposes `tflite::MicroInterpreter`
 * + `tflite::MicroMutableOpResolver` only via its C++ surface; the
 * C wrapper in TFL is for the full runtime, not the micro variant.
 * Wrapping it in C++ keeps the binding simple and lets us reuse Arm's
 * Ethos-U op resolver (`AddEthosU()`) verbatim across every Ethos NPU
 * variant the SDK targets:
 *   - Alif AEN E3 / E5 / E7  -> Ethos-U55 (CONFIG_ALP_TFLM_ETHOS_U55=y).
 *   - Alif AEN E4 / E6 / E8  -> Ethos-U85 primary
 *                               (CONFIG_ALP_TFLM_ETHOS_U85=y) +
 *                               U55 fallback (always also linked).
 *   - NXP i.MX 93            -> Ethos-U65 (CONFIG_ALP_SDK_INFERENCE_ETHOS_U_N93=y).
 * The SDK source code below is identical across all three variants;
 * only the upstream Vela `--accelerator-config` + Arm Ethos-U driver
 * build config differ between the per-NPU `CONFIG_ALP_TFLM_ETHOS_U*`
 * gates.  Per-variant attach hooks live in
 * src/zephyr/inference_ethosu_n93.c (gated by
 * CONFIG_ALP_SDK_INFERENCE_ETHOS_U_N93); a future inference_ethosu_aen.c
 * would carry the AEN-side equivalents (U85 + U55 driver shims) if
 * Alif's driver layer needs distinct attach helpers.
 *
 * Native-sim does NOT enable CONFIG_TENSORFLOW_LITE_MICRO so this
 * file is excluded from the native_sim build entirely; the wrapper
 * in inference_zephyr.c falls back to ALP_ERR_NOSUPPORT.  Real CI
 * coverage lands when the hil-aen runner comes online with the Vela
 * toolchain.
 */

#include <cstring>
#include <cstdint>

extern "C" {
#include "alp/inference.h"
#include "handles.h"
}

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#if defined(CONFIG_ALP_SDK_INFERENCE_ETHOS_U)
#include "tensorflow/lite/micro/kernels/ethos_u/ethosu.h"
#endif

namespace
{

/* Per-handle TFLM state.  Lives in the handle's `be_state` slot. */
struct TflmState {
    const tflite::Model *model = nullptr;
    /* Op resolver -- 32 ops covers MobileNetV2-style models with
     * room for a couple of custom Ethos-U ops.  Bigger models can
     * raise the bound via CONFIG_ALP_SDK_INFERENCE_TFLM_MAX_OPS. */
    tflite::MicroMutableOpResolver<32> resolver;
    tflite::MicroInterpreter          *interp = nullptr;
    /* Tensor arena -- caller-supplied via cfg.arena, or a built-in
     * static fall-back when cfg.arena is NULL. */
    uint8_t *arena_buf  = nullptr;
    size_t   arena_size = 0;
    bool     own_arena  = false;
};

constexpr size_t kDefaultArenaBytes = 128 * 1024;
alignas(16) uint8_t g_default_arena[kDefaultArenaBytes];
bool g_default_arena_in_use = false;

/* Re-fetch the alp_inference struct from the handle pointer.  The
 * dispatcher in inference_zephyr.c hands us the `struct alp_inference`
 * pointer; we use its `be_state` to find our TflmState. */
struct AlpInferenceHandle {
    bool                    in_use;
    alp_inference_backend_t backend;
    void                   *be_state;
};

/* Map a TFL data type onto the alp_inference dtype enum. */
alp_inference_dtype_t tfl_dtype_to_alp(TfLiteType t)
{
    switch (t) {
    case kTfLiteFloat32:
        return ALP_INFERENCE_DTYPE_F32;
    case kTfLiteFloat16:
        return ALP_INFERENCE_DTYPE_F16;
    case kTfLiteInt8:
        return ALP_INFERENCE_DTYPE_INT8;
    case kTfLiteUInt8:
        return ALP_INFERENCE_DTYPE_UINT8;
    case kTfLiteInt16:
        return ALP_INFERENCE_DTYPE_INT16;
    case kTfLiteInt32:
        return ALP_INFERENCE_DTYPE_INT32;
    default:
        return ALP_INFERENCE_DTYPE_F32;
    }
}

void register_default_ops(tflite::MicroMutableOpResolver<32> &r)
{
    /* Minimum op set for the v0.2 reference MobileNetV2.  Vela-compiled
     * models replace the bulk of these with Ethos-U custom ops; the
     * fallback ones still need to be registered for the layers Vela
     * leaves in CPU-land. */
    r.AddConv2D();
    r.AddDepthwiseConv2D();
    r.AddFullyConnected();
    r.AddSoftmax();
    r.AddReshape();
    r.AddAveragePool2D();
    r.AddMaxPool2D();
    r.AddAdd();
    r.AddMul();
    r.AddQuantize();
    r.AddDequantize();
    r.AddPad();
    r.AddMean();
    r.AddRelu();
    r.AddRelu6();
    r.AddLogistic();
#if defined(CONFIG_ALP_SDK_INFERENCE_ETHOS_U)
    r.AddEthosU();
#endif
}

void fill_tensor_descriptor(const TfLiteTensor *t, alp_inference_tensor_t *out)
{
    out->data       = t->data.raw;
    out->size_bytes = t->bytes;
    out->dtype      = tfl_dtype_to_alp(t->type);
    out->rank       = (uint8_t)((t->dims->size <= 4) ? t->dims->size : 4);
    for (int i = 0; i < out->rank; ++i) {
        out->shape[i] = (uint16_t)t->dims->data[i];
    }
    out->scale =
        (t->quantization.type == kTfLiteAffineQuantization)
            ? static_cast<TfLiteAffineQuantization *>(t->quantization.params)->scale->data[0]
            : 1.0f;
    out->zero_point =
        (t->quantization.type == kTfLiteAffineQuantization)
            ? static_cast<TfLiteAffineQuantization *>(t->quantization.params)->zero_point->data[0]
            : 0;
}

} // namespace

/* ------------------------------------------------------------------ */
/* Backend hooks (called from inference_zephyr.c's dispatcher).        */
/* ------------------------------------------------------------------ */

extern "C" alp_status_t alp_inference_tflm_open(struct alp_inference         *h_,
                                                const alp_inference_config_t *cfg)
{
    auto *h     = reinterpret_cast<AlpInferenceHandle *>(h_);
    auto *state = new (std::nothrow) TflmState();
    if (state == nullptr) return ALP_ERR_NOMEM;

    if (cfg->arena != nullptr && cfg->arena_bytes > 0) {
        state->arena_buf  = static_cast<uint8_t *>(cfg->arena);
        state->arena_size = cfg->arena_bytes;
        state->own_arena  = false;
    } else {
        if (g_default_arena_in_use) {
            delete state;
            return ALP_ERR_NOMEM;
        }
        g_default_arena_in_use = true;
        state->arena_buf       = g_default_arena;
        state->arena_size      = kDefaultArenaBytes;
        state->own_arena       = true;
    }

    state->model = tflite::GetModel(cfg->model_data);
    if (state->model->version() != TFLITE_SCHEMA_VERSION) {
        if (state->own_arena) g_default_arena_in_use = false;
        delete state;
        return ALP_ERR_INVAL;
    }

    register_default_ops(state->resolver);

    state->interp = new (std::nothrow) tflite::MicroInterpreter(
        state->model, state->resolver, state->arena_buf, state->arena_size);
    if (state->interp == nullptr) {
        if (state->own_arena) g_default_arena_in_use = false;
        delete state;
        return ALP_ERR_NOMEM;
    }

    if (state->interp->AllocateTensors() != kTfLiteOk) {
        delete state->interp;
        if (state->own_arena) g_default_arena_in_use = false;
        delete state;
        return ALP_ERR_IO;
    }

    h->be_state = state;
    return ALP_OK;
}

extern "C" size_t alp_inference_tflm_num_inputs(struct alp_inference *h_)
{
    auto *h     = reinterpret_cast<AlpInferenceHandle *>(h_);
    auto *state = static_cast<TflmState *>(h->be_state);
    return (state != nullptr && state->interp != nullptr) ? state->interp->inputs_size() : 0;
}

extern "C" size_t alp_inference_tflm_num_outputs(struct alp_inference *h_)
{
    auto *h     = reinterpret_cast<AlpInferenceHandle *>(h_);
    auto *state = static_cast<TflmState *>(h->be_state);
    return (state != nullptr && state->interp != nullptr) ? state->interp->outputs_size() : 0;
}

extern "C" alp_status_t alp_inference_tflm_get_input(struct alp_inference *h_, size_t index,
                                                     alp_inference_tensor_t *out)
{
    auto *h     = reinterpret_cast<AlpInferenceHandle *>(h_);
    auto *state = static_cast<TflmState *>(h->be_state);
    if (state == nullptr || state->interp == nullptr) return ALP_ERR_NOT_READY;
    if (index >= state->interp->inputs_size()) return ALP_ERR_INVAL;
    fill_tensor_descriptor(state->interp->input(index), out);
    return ALP_OK;
}

extern "C" alp_status_t alp_inference_tflm_get_output(struct alp_inference *h_, size_t index,
                                                      alp_inference_tensor_t *out)
{
    auto *h     = reinterpret_cast<AlpInferenceHandle *>(h_);
    auto *state = static_cast<TflmState *>(h->be_state);
    if (state == nullptr || state->interp == nullptr) return ALP_ERR_NOT_READY;
    if (index >= state->interp->outputs_size()) return ALP_ERR_INVAL;
    fill_tensor_descriptor(state->interp->output(index), out);
    return ALP_OK;
}

extern "C" alp_status_t alp_inference_tflm_invoke(struct alp_inference *h_)
{
    auto *h     = reinterpret_cast<AlpInferenceHandle *>(h_);
    auto *state = static_cast<TflmState *>(h->be_state);
    if (state == nullptr || state->interp == nullptr) return ALP_ERR_NOT_READY;
    return (state->interp->Invoke() == kTfLiteOk) ? ALP_OK : ALP_ERR_IO;
}

extern "C" void alp_inference_tflm_close(struct alp_inference *h_)
{
    auto *h     = reinterpret_cast<AlpInferenceHandle *>(h_);
    auto *state = static_cast<TflmState *>(h->be_state);
    if (state == nullptr) return;

    delete state->interp;
    if (state->own_arena) g_default_arena_in_use = false;
    delete state;
    h->be_state = nullptr;
}
