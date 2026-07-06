/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_inference dispatcher and per-backend
 * implementations.  NOT a public header -- customer code never
 * sees this struct.  Layout may change between SDK versions.
 *
 * Zephyr/M-class registry backends:
 *
 *   - tflm           (priority 50, "*")  -- portable TFLM CPU executor.
 *   - ethos_u_aen    (priority 100, alif:ensemble:e3/e4/e5/e6/e7/e8)
 *                                       -- TFLM + Ethos-U op resolver on AEN.
 *   - ethos_u_n93    (priority 100, nxp:imx9:imx93)
 *                                       -- TFLM + Ethos-U op resolver on i.MX 93.
 *   - sw_fallback    (priority 0,   "*") -- stateless NOSUPPORT stub.
 *
 * There is deliberately NO M-class DRP-AI or DEEPX DX-M1 backend:
 * both engines are A55/Linux-side only (MERA runtime / PCIe libdxrt;
 * issues #58/#59) and live in src/yocto/inference_{drpai,deepx}.cpp
 * behind the Yocto dispatcher, not this registry.
 *
 * Vendor extensions plug in via the alp_<vendor>_inference_*
 * surface defined in <alp/ext/<vendor>/inference.h>; each
 * function gates on the handle's backend->vendor string before
 * touching hardware.
 *
 * Zephyr leakage: backends type `dev` as void * so the dispatcher
 * TU stays free of <zephyr/device.h>.  Each backend casts in/out
 * of its own TU.
 */

#ifndef ALP_BACKENDS_INFERENCE_OPS_H
#define ALP_BACKENDS_INFERENCE_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/inference.h>
#include <alp/peripheral.h>

typedef struct alp_inference_ops alp_inference_ops_t;

/** Backend-owned per-handle state. */
typedef struct alp_inference_backend_state {
	void                      *dev;        /* opaque backend device pointer */
	alp_inference_backend_t    backend_id; /* AUTO-resolved or caller-pinned */
	void                      *be_data;    /* backend-private state */
	const alp_inference_ops_t *ops;
} alp_inference_backend_state_t;

/** Vtable each inference backend implements. */
struct alp_inference_ops {
	alp_status_t (*open)(const alp_inference_config_t  *cfg,
	                     alp_inference_backend_state_t *state,
	                     alp_capabilities_t            *caps_out);
	size_t (*num_inputs)(alp_inference_backend_state_t *state);
	size_t (*num_outputs)(alp_inference_backend_state_t *state);
	alp_status_t (*get_input)(alp_inference_backend_state_t *state,
	                          size_t                         index,
	                          alp_inference_tensor_t        *out);
	alp_status_t (*get_output)(alp_inference_backend_state_t *state,
	                           size_t                         index,
	                           alp_inference_tensor_t        *out);
	alp_status_t (*invoke)(alp_inference_backend_state_t *state);
	void (*close)(alp_inference_backend_state_t *state);
};

/**
 * Handle struct layout.  Opaque to customers via the public
 * `typedef struct alp_inference alp_inference_t;` forward
 * declaration in <alp/inference.h>.  Defined here so the
 * dispatcher (src/inference_dispatch.c) and any per-backend .c /
 * .cpp files can access the fields without duplicating the
 * layout.
 */
struct alp_inference {
	alp_inference_backend_state_t state;
	const alp_backend_t          *backend;
	alp_capabilities_t            cached_caps;
	bool                          in_use;
};

#endif /* ALP_BACKENDS_INFERENCE_OPS_H */
