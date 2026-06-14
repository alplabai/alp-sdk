/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * [vendor-ext] Renesas DRP-AI (DRP-AI TVM / MERA) backend hook for
 * <alp/inference.h>, A55 / Linux / Yocto side of the RZ/V2N.
 *
 * BENCH-UNVERIFIED: header-checks against the real
 * MeraDrpRuntimeWrapper.h surface, but has NOT run on silicon and does
 * NOT cross-link here -- the EdgeCortix MERA2 runtime + DRP-AI TVM
 * runtime libs only exist on the RZ/V Yocto SDK sysroot (mera2_runtime /
 * drp_tvm_rt / tvm_runtime), not on this dev host.  Compiled only when
 * ALP_SDK_USE_DRPAI_V2N=ON (default OFF).  Same posture as the DEEPX
 * DX-M1 hook (inference_deepx.cpp).
 *
 * ----------------------------------------------------------------------
 * Real vendor API
 *   Written against the *real* DRP-AI TVM application runtime wrapper
 *   `MeraDrpRuntimeWrapper` (rzv_drp-ai_tvm/apps/MeraDrpRuntimeWrapper.h,
 *   EdgeCortix/Renesas).  Surface used here (all present in that header):
 *     - MeraDrpRuntimeWrapper()                          default ctor
 *     - bool LoadModel(const std::string& model_dir,
 *                      uint64_t start_address)           loads the .dat dir
 *     - void SetInput(int idx, const float*  data)
 *     - void SetInput(int idx, const uint16_t* data)     (fp16)
 *     - std::vector<std::tuple<std::string,size_t,InOutDataType>>
 *           GetInputInfo() / GetOutputInfo()
 *     - std::tuple<InOutDataType,void*,int64_t> GetOutput(int idx)
 *                                                        (dtype, ptr, elems)
 *     - void Run()
 *   `InOutDataType` is `enum class { FLOAT32, FLOAT16, INT32, INT64,
 *   OTHER }`.  The header also exposes GetInputDataType(int)/GetNumInput()/
 *   GetNumOutput(), but this body takes the input/output dtype straight
 *   from the InOutDataType in the GetInputInfo()/GetOutputInfo() tuple
 *   (std::get<2>) and the counts from those vectors' sizes, so those
 *   scalar accessors are part of the available surface but not invoked.
 *
 *   Header self-containedness: MeraDrpRuntimeWrapper.h uses std::tuple and
 *   std::vector but only #includes <string>/<memory>/<ostream> and
 *   <tvm/runtime/profiling.h> -- it does NOT pull in <tuple>/<vector>
 *   itself.  This file therefore #includes <tuple> and <vector> BEFORE the
 *   wrapper include below; that ordering is load-bearing, not incidental.
 *
 * Blob format ("drpai_dir")
 *   DRP-AI's compiled model is a multi-file object DIRECTORY (drp_desc.bin
 *   / weight.bin / addr_map.txt / deploy.json / deploy.so / preprocess/ ...)
 *   emitted by the host DRP-AI TVM compiler
 *   (scripts/alp_model/adapters/drpai.py).  It is NOT a single flat
 *   buffer, so the portable surface can't pass it by value.  Convention:
 *   for ALP_INFERENCE_MODEL_DRPAI, cfg.model_data points at a
 *   NUL-terminated FILESYSTEM PATH (UTF-8) to the installed object dir on
 *   the A55, and cfg.model_size is its length+1.
 *
 *   TODO(loader staging, BENCH-UNVERIFIED): this body REQUIRES cfg.model_data
 *   to be a directory path, but src/common/alp_model_loader.c currently
 *   passes the raw `drpai_dir` blob (the deterministic .tar produced by
 *   adapters/drpai.py) straight through as cfg.model_data/model_size -- it
 *   does NOT yet untar the blob to a temp dir and substitute the staged
 *   path.  Until that staging lands in alp_model_loader.c (untar-to-tempdir
 *   when sel.format == ALP_INFERENCE_MODEL_DRPAI, then pass the dir path),
 *   a real .alpmodel DRP-AI load would hand tar bytes to LoadModel() as if
 *   they were a path and fail on target.  The DEEPX path is unaffected (it
 *   takes the raw .dxnn buffer by value, matching the InferenceEngine
 *   ctor).  Pending follow-up; tracked alongside the Yocto runtime work.
 *
 * Vendor-artifact handling (classifying-public-vs-internal)
 *   rzv_drp-ai_tvm is Apache-2.0 (referenceable) BUT the prebuilt MERA2
 *   runtime libs + the DRP-AI Translator are Renesas/EdgeCortix
 *   account-gated.  Neither the wrapper sources nor the binaries are
 *   vendored here; this body links against them via the RZ/V Yocto SDK
 *   sysroot at build time.  Account-gated binaries belong in
 *   alp-sdk-internal (Git LFS).
 *
 *   Follow-up: place MeraDrpRuntimeWrapper.{h,cpp} + the MERA2 prebuilt
 *   libs in alp-sdk-internal and wire the RZ/V Yocto SDK sysroot find
 *   into src/yocto/CMakeLists.txt's ALP_SDK_USE_DRPAI_V2N block so the
 *   cross-build resolves mera2_runtime/drp_tvm_rt/tvm_runtime.
 *
 * Dispatcher contract
 *   Mirrors the 7-symbol hook shape the Yocto dispatcher in
 *   inference_yocto.c calls.  The handle layout below MUST match
 *   inference_yocto.c's `struct alp_inference` exactly.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <tuple>
#include <vector>

#include "MeraDrpRuntimeWrapper.h"

extern "C" {
#include "alp/inference.h"
}

/* Mirror of the yocto dispatcher's struct alp_inference layout.  MUST
 * match inference_yocto.c exactly -- keep in sync with the dispatcher. */
struct alp_inference_handle_layout {
    bool                    in_use;
    alp_inference_backend_t backend;
    void                   *be_state;
};

namespace {

/* DRP-AI runtime memory window start address on the RZ/V2N.  The DRP-AI
 * reserved CMA region base; the tutorial apps use the value the kernel
 * exports via /sys (udmabuf) -- a placeholder here, resolved on-target
 * before bench bring-up.  TODO(bench): read the real base from the
 * drpai udmabuf/CMA node rather than hard-coding. */
constexpr uint64_t kDrpAiMemStart = 0x80000000ULL;

/** Per-handle DRP-AI state.  Owns the MERA runtime wrapper + SDK-owned
 *  input staging buffers + a snapshot of the I/O tensor metadata taken
 *  at open() time. */
struct DrpaiState {
    MeraDrpRuntimeWrapper runtime;          /* default-constructed */
    std::string           model_dir;

    /* One SDK-owned input staging buffer per input tensor; the app fills
     * these via get_input(), invoke() pushes them with SetInput(). */
    std::vector<std::vector<uint8_t>> input_bufs;

    /* I/O metadata snapshots: (name, size_bytes, dtype). */
    std::vector<std::tuple<std::string, size_t, InOutDataType>> in_info;
    std::vector<std::tuple<std::string, size_t, InOutDataType>> out_info;
};

/** Map a MERA InOutDataType onto the alp_inference dtype enum.  The DRP-AI
 *  wrapper reports FLOAT32 / FLOAT16 / INT32 / INT64 / OTHER; INT64 and
 *  OTHER have no portable slot, so they fall back to INT32 / UINT8
 *  respectively (the raw bytes stay reachable via the descriptor). */
alp_inference_dtype_t mera_dtype_to_alp(InOutDataType t)
{
    switch (t) {
    case InOutDataType::FLOAT32:
        return ALP_INFERENCE_DTYPE_F32;
    case InOutDataType::FLOAT16:
        return ALP_INFERENCE_DTYPE_F16;
    case InOutDataType::INT32:
        return ALP_INFERENCE_DTYPE_INT32;
    case InOutDataType::INT64:
        /* No 64-bit slot in the portable enum; report as int32 (callers
         * needing true int64 read raw bytes via size_bytes). */
        return ALP_INFERENCE_DTYPE_INT32;
    case InOutDataType::OTHER:
    default:
        return ALP_INFERENCE_DTYPE_UINT8;
    }
}

}  /* namespace */

/* ------------------------------------------------------------------ */
/* Backend hooks (C ABI, matching inference_yocto.c's declarations).   */
/* ------------------------------------------------------------------ */

extern "C" alp_status_t alp_inference_drpai_open(struct alp_inference         *h_,
                                                 const alp_inference_config_t *cfg)
{
    auto *h = reinterpret_cast<alp_inference_handle_layout *>(h_);

    /* For ALP_INFERENCE_MODEL_DRPAI the blob is a directory PATH, not a
     * flat buffer (see the header comment).  Reject an empty path early. */
    if (cfg->model_data == nullptr || cfg->model_size == 0) {
        return ALP_ERR_INVAL;
    }

    auto *st = new (std::nothrow) DrpaiState();
    if (st == nullptr) {
        return ALP_ERR_NOMEM;
    }

    /* model_data is a NUL-terminated UTF-8 directory path; bound the copy
     * by model_size so a missing terminator can't run off the end. */
    st->model_dir.assign(static_cast<const char *>(cfg->model_data),
                         ::strnlen(static_cast<const char *>(cfg->model_data),
                                   cfg->model_size));

    /* LoadModel returns false on a missing/corrupt object dir or a DRP-AI
     * memory-mapping failure. */
    if (!st->runtime.LoadModel(st->model_dir, kDrpAiMemStart)) {
        delete st;
        return ALP_ERR_IO;
    }

    st->in_info  = st->runtime.GetInputInfo();
    st->out_info = st->runtime.GetOutputInfo();

    /* Stage one SDK-owned buffer per input tensor, sized from the
     * wrapper-reported byte size. */
    st->input_bufs.resize(st->in_info.size());
    for (size_t i = 0; i < st->in_info.size(); ++i) {
        st->input_bufs[i].resize(std::get<1>(st->in_info[i]));
    }

    h->be_state = st;
    return ALP_OK;
}

extern "C" std::size_t alp_inference_drpai_num_inputs(struct alp_inference *h_)
{
    auto *h  = reinterpret_cast<alp_inference_handle_layout *>(h_);
    auto *st = static_cast<DrpaiState *>(h->be_state);
    return (st != nullptr) ? st->in_info.size() : 0u;
}

extern "C" std::size_t alp_inference_drpai_num_outputs(struct alp_inference *h_)
{
    auto *h  = reinterpret_cast<alp_inference_handle_layout *>(h_);
    auto *st = static_cast<DrpaiState *>(h->be_state);
    return (st != nullptr) ? st->out_info.size() : 0u;
}

extern "C" alp_status_t alp_inference_drpai_get_input(struct alp_inference *h_, std::size_t index,
                                                      alp_inference_tensor_t *out)
{
    auto *h  = reinterpret_cast<alp_inference_handle_layout *>(h_);
    auto *st = static_cast<DrpaiState *>(h->be_state);
    if (st == nullptr) {
        return ALP_ERR_NOT_READY;
    }
    if (index >= st->in_info.size()) {
        return ALP_ERR_OUT_OF_RANGE;
    }

    /* Hand back the SDK-owned staging buffer; the app fills it before
     * invoke().  The MERA wrapper does not expose per-input shape via the
     * public surface, so rank/shape stay 0 (size_bytes is authoritative
     * for buffer sizing). */
    out->data       = st->input_bufs[index].data();
    out->size_bytes = std::get<1>(st->in_info[index]);
    out->dtype      = mera_dtype_to_alp(std::get<2>(st->in_info[index]));
    out->rank       = 0u;
    out->scale      = 1.0f;
    out->zero_point = 0;
    return ALP_OK;
}

extern "C" alp_status_t alp_inference_drpai_get_output(struct alp_inference *h_, std::size_t index,
                                                       alp_inference_tensor_t *out)
{
    auto *h  = reinterpret_cast<alp_inference_handle_layout *>(h_);
    auto *st = static_cast<DrpaiState *>(h->be_state);
    if (st == nullptr) {
        return ALP_ERR_NOT_READY;
    }
    if (index >= st->out_info.size()) {
        return ALP_ERR_OUT_OF_RANGE;
    }

    /* GetOutput(idx) -> (dtype, data ptr, elem_count).  Before the first
     * Run() the wrapper returns its zero-initialised output area; after
     * Run() it points at the live result buffer.  elem_count * dtype-size
     * gives the byte size; prefer the GetOutputInfo() byte size when the
     * dtype is known. */
    InOutDataType dtype;
    void         *data      = nullptr;
    int64_t       num_elems = 0;
    std::tie(dtype, data, num_elems) = st->runtime.GetOutput(static_cast<int>(index));

    out->data       = data;
    out->size_bytes = std::get<1>(st->out_info[index]);
    out->dtype      = mera_dtype_to_alp(dtype);
    out->rank       = 0u;
    out->scale      = 1.0f;
    out->zero_point = 0;
    return ALP_OK;
}

extern "C" alp_status_t alp_inference_drpai_invoke(struct alp_inference *h_)
{
    auto *h  = reinterpret_cast<alp_inference_handle_layout *>(h_);
    auto *st = static_cast<DrpaiState *>(h->be_state);
    if (st == nullptr) {
        return ALP_ERR_NOT_READY;
    }

    /* Push each SDK-owned input into the runtime.  SetInput is overloaded
     * on fp32 vs fp16; pick by the reported input dtype so fp16 models
     * route through the uint16_t overload (raw half-float bytes). */
    for (size_t i = 0; i < st->input_bufs.size(); ++i) {
        const InOutDataType dt = std::get<2>(st->in_info[i]);
        const void *buf        = st->input_bufs[i].data();
        if (dt == InOutDataType::FLOAT16) {
            st->runtime.SetInput(static_cast<int>(i),
                                 static_cast<const uint16_t *>(buf));
        } else {
            st->runtime.SetInput(static_cast<int>(i),
                                 static_cast<const float *>(buf));
        }
    }

    /* Run() is void and blocks until DRP-AI completes; it reports
     * hard faults via its own logging/abort path, not a return code. */
    st->runtime.Run();
    return ALP_OK;
}

extern "C" void alp_inference_drpai_close(struct alp_inference *h_)
{
    auto *h  = reinterpret_cast<alp_inference_handle_layout *>(h_);
    auto *st = static_cast<DrpaiState *>(h->be_state);
    if (st == nullptr) {
        return;
    }
    /* MeraDrpRuntimeWrapper owns its DRP-AI mappings + releases them in
     * its dtor (unique_ptr<Impl>); deleting the state tears it down. */
    delete st;
    h->be_state = nullptr;
}
