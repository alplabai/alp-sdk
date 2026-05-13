/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file inference.h
 * @brief ALP SDK unified ML inference abstraction.
 *
 * Lifts compiled-model loaders + dispatch into a single uniform
 * surface so apps don't need to know which NPU is bonded out on
 * the active SoM.  Backends:
 *
 *   - **Ethos-U** (Alif Ensemble E3/E4/E7/E8 NPUs, NXP i.MX 93)
 *     via TensorFlow Lite Micro + Vela compiler.  Lands in v0.2
 *     for AEN-Zephyr; the i.MX 93 path follows in v0.4.
 *   - **DRP-AI3** (Renesas RZ/V2N) via Renesas's DRP-AI translator
 *     toolchain.  Lands in v0.3 alongside the V2N-Zephyr work.
 *   - **DEEPX DX-M1** via the DEEPX SDK adapter (proprietary).
 *     Lands in v0.4 for V2N-M1.
 *   - **CPU fallback** via TFLM's reference kernels.  Lands in v0.2;
 *     useful for development and for parts of a model that don't
 *     map to the NPU's supported op set.
 *
 * v0.1 ships the surface as a stub (everything returns
 * ALP_ERR_NOSUPPORT, *_open returns NULL) so apps that target
 * `<alp/inference.h>` can compile against the full v1.0-shape
 * surface today.  Same contract as `<alp/iot.h>` shipped in v0.1.
 *
 * Vendor-specific accelerator paths (`<alp/vendors/alif/ethosu.h>`,
 * `<alp/vendors/renesas/drpai.h>`, `<alp/vendors/deepx/dxm1.h>`)
 * remain available as escape hatches when the unified API can't
 * express what the vendor SDK offers.  The unification stance is
 * "best-effort, not absolute".
 */

#ifndef ALP_INFERENCE_H
#define ALP_INFERENCE_H

#include <stdint.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Backend selector.  AUTO routes to the best available NPU; the
 *  others force a specific backend (useful for benchmarking, or to
 *  fall through to CPU when the model doesn't map to NPU ops). */
typedef enum {
    ALP_INFERENCE_BACKEND_AUTO     = 0,
    ALP_INFERENCE_BACKEND_CPU      = 1,    /**< TFLM reference kernels. */
    ALP_INFERENCE_BACKEND_ETHOS_U  = 2,    /**< Arm Ethos-U via Vela. */
    ALP_INFERENCE_BACKEND_DRPAI    = 3,    /**< Renesas DRP-AI3. */
    ALP_INFERENCE_BACKEND_DEEPX_DX = 4     /**< DEEPX DX-M1. */
} alp_inference_backend_t;

/** Model format.  Each backend supports a subset; AUTO picks based
 *  on whichever loader matches the magic bytes at the head of the
 *  model buffer. */
typedef enum {
    ALP_INFERENCE_MODEL_TFLITE     = 0,    /**< `.tflite` flatbuffer. */
    ALP_INFERENCE_MODEL_VELA       = 1,    /**< Vela-compiled `.tflite`. */
    ALP_INFERENCE_MODEL_DRPAI      = 2,    /**< Renesas DRP-AI binary. */
    ALP_INFERENCE_MODEL_DXNN       = 3,    /**< DEEPX DXNN binary. */
    ALP_INFERENCE_MODEL_EXECUTORCH = 4     /**< ExecuTorch program. */
} alp_inference_model_format_t;

/** Tensor element type. */
typedef enum {
    ALP_INFERENCE_DTYPE_F32    = 0,
    ALP_INFERENCE_DTYPE_F16    = 1,
    ALP_INFERENCE_DTYPE_INT8   = 2,
    ALP_INFERENCE_DTYPE_UINT8  = 3,
    ALP_INFERENCE_DTYPE_INT16  = 4,
    ALP_INFERENCE_DTYPE_INT32  = 5
} alp_inference_dtype_t;

/** Tensor descriptor — what `get_input` / `get_output` return. */
typedef struct {
    void                  *data;          /**< Backend-owned buffer. */
    size_t                 size_bytes;    /**< Total buffer size. */
    alp_inference_dtype_t  dtype;
    uint8_t                rank;          /**< 0..4 typical. */
    uint16_t               shape[4];      /**< Most-significant first. */
    /** Quantisation params (only meaningful when dtype is integer). */
    float                  scale;
    int32_t                zero_point;
} alp_inference_tensor_t;

typedef struct alp_inference alp_inference_t;

typedef struct {
    const void                  *model_data;   /**< Pointer to model bytes. */
    size_t                       model_size;
    alp_inference_model_format_t format;
    alp_inference_backend_t      backend;
    /** Bytes of scratch arena the backend may use.  TFLM-style
     *  backends size this from the compile-time tensor arena
     *  estimate; if 0, the backend uses a built-in default. */
    size_t                       arena_bytes;
    /** Caller-allocated arena, or NULL to let the backend use heap. */
    void                        *arena;
} alp_inference_config_t;

/**
 * @brief Load a compiled model and prepare it for invocation.
 *
 * Verifies the model's format / signature, allocates per-tensor
 * buffers (or maps them into the caller's arena), and binds the
 * selected backend.
 *
 * @param[in] cfg  Configuration; @c model_data must be non-NULL.
 * @return Open handle, or NULL on:
 *         - bad magic / unsupported model format
 *         - backend not available (e.g. ETHOS_U requested on V2N)
 *         - arena too small (caller must provide more bytes)
 *         - allocation failure
 *         Read @ref alp_last_error for the precise reason.
 */
alp_inference_t *alp_inference_open(const alp_inference_config_t *cfg);

/** Number of input tensors the model expects. */
size_t           alp_inference_num_inputs(alp_inference_t *inf);

/** Number of output tensors the model produces. */
size_t           alp_inference_num_outputs(alp_inference_t *inf);

/**
 * @brief Get a descriptor for input tensor @p index.
 *
 * The returned tensor's `data` pointer is owned by the SDK and
 * remains valid until @ref alp_inference_close.  Apps fill the
 * buffer before calling @ref alp_inference_invoke.
 */
alp_status_t     alp_inference_get_input(alp_inference_t *inf,
                                         size_t index,
                                         alp_inference_tensor_t *out);

/** Get a descriptor for output tensor @p index. */
alp_status_t     alp_inference_get_output(alp_inference_t *inf,
                                          size_t index,
                                          alp_inference_tensor_t *out);

/**
 * @brief Run one inference pass.
 *
 * Dispatches to the bound backend.  On Ethos-U / DRP-AI / DX-M1
 * backends this offloads to the NPU and blocks the calling thread
 * until the result lands; on the CPU backend it executes in-thread.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY (handle closed) /
 *         ALP_ERR_INVAL / ALP_ERR_TIMEOUT (NPU stuck) /
 *         ALP_ERR_IO (NPU error).
 */
alp_status_t     alp_inference_invoke(alp_inference_t *inf);

/** Release the model + tensor buffers.  NULL-safe. */
void             alp_inference_close(alp_inference_t *inf);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_INFERENCE_H */
