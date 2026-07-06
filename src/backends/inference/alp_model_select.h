/* SPDX-License-Identifier: Apache-2.0
 * Pure .alpmodel blob-selection engine (OS-agnostic; no Zephyr/registry deps).
 * NOT a public header. */
#ifndef ALP_BACKENDS_INFERENCE_MODEL_SELECT_H
#define ALP_BACKENDS_INFERENCE_MODEL_SELECT_H

#include <stddef.h>
#include <stdint.h>
#include "alp/inference.h"
#include "alp/model.h"
#include "alp/peripheral.h"

/** Device facts the selection runs against (injectable so the algorithm
 *  is unit-tested without a live SoC).  @c avail_silicon must be non-NULL
 *  when @c n_avail_silicon > 0.
 *
 *  Availability is ENGINE-gated, not SoC-string-gated: a ref belongs in
 *  @c soc_ref / @c avail_silicon only when this build compiles an
 *  inference-engine backend that can actually drive that silicon from
 *  the running core.  Hosting an NPU on the die is not enough -- e.g. a
 *  V2N M33 build must pass @c soc_ref = NULL because the DRP-AI3 engine
 *  is A55/Linux-side only (issues #58/#59); see the env composition in
 *  src/common/alp_model_loader.c. */
typedef struct {
	const char        *soc_ref;       /* host SoC ref when its on-SoC NPU engine is
					      compiled in (Ethos-U on M-class); else NULL */
	const char *const *avail_silicon; /* refs of every other compiled-in engine
					      (A55-side DRP-AI, discrete DX-M1, ...) */
	size_t             n_avail_silicon;
	uint32_t           arena_sram_kib; /* device NPU arena budget; 0 = unknown -> skip SRAM gate */
	alp_inference_backend_t preferred_backend; /* SoM preferred (tiebreak); AUTO if none */
} alp_model_select_env_t;

/** The chosen blob + its resolved descriptors. */
typedef struct {
	uint32_t                     target_index;
	alp_inference_backend_t      backend;
	alp_inference_model_format_t format;
	uint32_t                     arena_bytes;
} alp_model_select_result_t;

/**
 * @brief Pick the best-fit target from a parsed .alpmodel for this device.
 * @param m          Parsed model (from alp_model_parse).
 * @param env        Injectable device facts.
 * @param requested  AUTO, or a forced backend (errors NOT_FOUND if absent).
 * @param out        Filled on ALP_OK.
 * @return ALP_OK (+ *out); ALP_ERR_INVAL (m/env/out NULL or n_targets==0);
 *         ALP_ERR_NOT_FOUND; ALP_ERR_NO_FIT; ALP_ERR_NO_BACKEND.
 */
alp_status_t alp_model_select(const alp_model_t            *m,
                              const alp_model_select_env_t *env,
                              alp_inference_backend_t       requested,
                              alp_model_select_result_t    *out);
#endif /* ALP_BACKENDS_INFERENCE_MODEL_SELECT_H */
