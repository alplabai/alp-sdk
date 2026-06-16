/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal: shared TFLM ops vtable + variant helpers reused by the
 * vendor-specific Ethos-U backends (ethos_u_aen on AEN family,
 * ethos_u_n93 on i.MX 93).  The Ethos-U backends register the same
 * vtable against a higher-priority silicon_ref + vendor pair so
 * the registry routes their builds through TFLM with AddEthosU()
 * compiled in via the per-vendor backend gate.
 *
 * NOT a public header.
 */

#ifndef ALP_BACKENDS_INFERENCE_TFLM_SHARED_H
#define ALP_BACKENDS_INFERENCE_TFLM_SHARED_H

#include "inference_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const alp_inference_ops_t alp_inference_tflm_ops;

const char *alp_inference_tflm_cpu_kernel_variant(void);
const char *alp_inference_tflm_npu_variant_name(void);

#ifdef __cplusplus
}
#endif

#endif /* ALP_BACKENDS_INFERENCE_TFLM_SHARED_H */
