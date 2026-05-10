/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v0.1 stub for <alp/inference.h>.  Every entry point returns
 * ALP_ERR_NOSUPPORT; alp_inference_open returns NULL.  Real
 * backends arrive on the per-backend roadmap:
 *
 *   - Ethos-U via TFLM + Vela on AEN-Zephyr  (v0.2)
 *   - CPU fallback via TFLM reference kernels (v0.2)
 *   - DRP-AI3 on V2N-Zephyr                   (v0.3)
 *   - DEEPX DX-M1 on V2N-M1                   (v0.4)
 *   - Ethos-U on i.MX 93                       (v0.4)
 *
 * Same shape as iot_stub.c / audio_stub.c.
 */

#include <stddef.h>

#include "alp/inference.h"

alp_inference_t *alp_inference_open(const alp_inference_config_t *cfg) {
    (void)cfg;
    return NULL;
}

size_t alp_inference_num_inputs(alp_inference_t *inf) {
    (void)inf;
    return 0u;
}

size_t alp_inference_num_outputs(alp_inference_t *inf) {
    (void)inf;
    return 0u;
}

alp_status_t alp_inference_get_input(alp_inference_t *inf,
                                     size_t index,
                                     alp_inference_tensor_t *out) {
    (void)inf; (void)index;
    if (out != NULL) {
        *out = (alp_inference_tensor_t){0};
    }
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_inference_get_output(alp_inference_t *inf,
                                      size_t index,
                                      alp_inference_tensor_t *out) {
    (void)inf; (void)index;
    if (out != NULL) {
        *out = (alp_inference_tensor_t){0};
    }
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_inference_invoke(alp_inference_t *inf) {
    (void)inf;
    return ALP_ERR_NOSUPPORT;
}

void alp_inference_close(alp_inference_t *inf) {
    (void)inf;
}
