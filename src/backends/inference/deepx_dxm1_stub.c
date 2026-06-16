/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * DEEPX DX-M1 inference backend -- NOT_IMPLEMENTED stub
 * (Zephyr-side).
 *
 * Registers against silicon_ref="deepx:dx:m1" at priority 100 so
 * V2N-M1 builds that pick ALP_INFERENCE_BACKEND_AUTO get a clear
 * NOT_IMPLEMENTED signal that the DX-M1 path is queued rather
 * than silently falling through to TFLM on the M-class core.
 *
 * The Yocto-side DEEPX body already exists at
 * src/yocto/inference_deepx.cpp and is owned by the Yocto-registry
 * migration slice (#33).  This file covers the Zephyr-side path
 * where M-core firmware drives the DX-M1 NPU via PCIe-over-
 * LinkBoost (V2N-M1 SoM topology).
 *
 * The real body needs:
 *
 *   - The DEEPX SDK proprietary adapter (not in west.yml yet).
 *   - The .dxnn translator output magic + tile metadata format.
 *   - DRAM-aware tile management knobs surfaced via
 *     <alp/ext/deepx/inference.h>.
 *   - PCIe-over-LinkBoost transport binding.
 *
 * @par Tracking: github.com/alplabai/alp-sdk/issues/59
 */

#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/inference.h>
#include <alp/peripheral.h>

#include "inference_ops.h"

static alp_status_t deepx_open(const alp_inference_config_t  *cfg,
                               alp_inference_backend_state_t *state,
                               alp_capabilities_t            *caps_out)
{
	(void)cfg;
	state->be_data  = NULL;
	state->dev      = NULL;
	caps_out->flags = 0u;
	/* The real body negotiates the .dxnn version, primes the
     * DX-M1 command queue + DRAM tile allocator, and surfaces
     * the slot-management knobs via <alp/ext/deepx/inference.h>.
     * See issue #59 for the v0.x landing target. */
	return ALP_ERR_NOT_IMPLEMENTED;
}

static size_t deepx_num_inputs(alp_inference_backend_state_t *state)
{
	(void)state;
	return 0u;
}

static size_t deepx_num_outputs(alp_inference_backend_state_t *state)
{
	(void)state;
	return 0u;
}

static alp_status_t
deepx_get_input(alp_inference_backend_state_t *state, size_t index, alp_inference_tensor_t *out)
{
	(void)state;
	(void)index;
	(void)out;
	return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t
deepx_get_output(alp_inference_backend_state_t *state, size_t index, alp_inference_tensor_t *out)
{
	(void)state;
	(void)index;
	(void)out;
	return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t deepx_invoke(alp_inference_backend_state_t *state)
{
	(void)state;
	/* DX-M1 dispatch lands with the DEEPX SDK adapter.
     * See issue #59. */
	return ALP_ERR_NOT_IMPLEMENTED;
}

static void deepx_close(alp_inference_backend_state_t *state)
{
	if (state != NULL) {
		state->be_data = NULL;
	}
}

static const alp_inference_ops_t _ops = {
	.open        = deepx_open,
	.num_inputs  = deepx_num_inputs,
	.num_outputs = deepx_num_outputs,
	.get_input   = deepx_get_input,
	.get_output  = deepx_get_output,
	.invoke      = deepx_invoke,
	.close       = deepx_close,
};

ALP_BACKEND_REGISTER(inference,
                     deepx_dxm1,
                     {
                         .silicon_ref = "deepx:dx:m1",
                         .vendor      = "deepx",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
