/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stub of the DEEPX DX-M1 host-SDK header.  Declares only the
 * minimum surface the ALP SDK's backend hook
 * (src/yocto/inference_deepx.cpp) references, so the dispatcher
 * path compiles on hosts that do not have the proprietary DEEPX
 * runtime installed.
 *
 * When the real DEEPX runtime is on the include path (installed
 * by the upstream `dx-rt` Yocto recipe from
 * `https://github.com/DEEPX-AI/meta-deepx-m1`, scarthgap branch),
 * that header is picked up instead of this stub -- CMake places
 * the sysroot include path before vendors/deepx-dxm1/include.
 *
 * The names + signatures below mirror the DEEPX public ABI as
 * advertised in the developer portal documentation (DX-M1 host SDK
 * 0.9 series, 2026-Q1).  Newer SDK revisions may extend the surface
 * additively; the SDK's backend hook stays compatible by sticking
 * to this minimum set.
 */

#ifndef ALP_STUB_DXNN_DXNN_H_
#define ALP_STUB_DXNN_DXNN_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque runtime handle returned by @ref dxnn_init. */
typedef struct dxnn_runtime dxnn_runtime_t;

/** Opaque model handle returned by @ref dxnn_load_model. */
typedef struct dxnn_model dxnn_model_t;

/** Status codes returned by every dxnn entry point. */
typedef enum {
    DXNN_OK            = 0,
    DXNN_ERR_INVAL     = -1,
    DXNN_ERR_NOMEM     = -2,
    DXNN_ERR_NODEV     = -3, /**< no DX-M1 enumerated on PCIe */
    DXNN_ERR_BADMODEL  = -4, /**< magic / version mismatch */
    DXNN_ERR_TIMEOUT   = -5,
    DXNN_ERR_IO        = -6,
    DXNN_ERR_NOSUPPORT = -7
} dxnn_status_t;

/** Tensor element type the runtime reports to callers. */
typedef enum {
    DXNN_DTYPE_FLOAT32 = 0,
    DXNN_DTYPE_FLOAT16 = 1,
    DXNN_DTYPE_INT8    = 2,
    DXNN_DTYPE_UINT8   = 3,
    DXNN_DTYPE_INT16   = 4,
    DXNN_DTYPE_INT32   = 5
} dxnn_dtype_t;

/** Tensor descriptor returned by @ref dxnn_get_input / @ref dxnn_get_output. */
typedef struct {
    void        *data;
    size_t       size_bytes;
    dxnn_dtype_t dtype;
    uint8_t      rank;
    uint32_t     shape[4];
    float        scale;
    int32_t      zero_point;
} dxnn_tensor_t;

/**
 * @brief Initialise the host runtime and bind to the first DX-M1
 *        device enumerated on PCIe.
 *
 * @param[out] out  Receives the runtime handle on success.
 * @return DXNN_OK or DXNN_ERR_NODEV if no DX-M1 is present.
 */
dxnn_status_t dxnn_init(dxnn_runtime_t **out);

/** Release the runtime.  NULL-safe. */
void dxnn_destroy(dxnn_runtime_t *rt);

/**
 * @brief Load a DXNN-format compiled model into the device.
 *
 * @param rt          Runtime handle from @ref dxnn_init.
 * @param model_data  Pointer to the DXNN flatbuffer.
 * @param model_size  Size of the flatbuffer in bytes.
 * @param[out] out    Receives the model handle on success.
 */
dxnn_status_t dxnn_load_model(dxnn_runtime_t *rt, const void *model_data, size_t model_size,
                              dxnn_model_t **out);

/** Release the model.  NULL-safe. */
void dxnn_unload_model(dxnn_model_t *m);

/** Number of input / output tensors the model expects. */
size_t dxnn_num_inputs(dxnn_model_t *m);
size_t dxnn_num_outputs(dxnn_model_t *m);

/** Fill @p out with the descriptor for input / output tensor @p index. */
dxnn_status_t dxnn_get_input(dxnn_model_t *m, size_t index, dxnn_tensor_t *out);
dxnn_status_t dxnn_get_output(dxnn_model_t *m, size_t index, dxnn_tensor_t *out);

/** Run one inference pass.  Blocks until the device returns. */
dxnn_status_t dxnn_run(dxnn_model_t *m);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_STUB_DXNN_DXNN_H_ */
