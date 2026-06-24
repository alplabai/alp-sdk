/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software inference fallback.  Stateless stub for native_sim
 * builds and any host without TFLM linked in.
 *
 * Every op besides open / close returns ALP_ERR_NOSUPPORT.  Apps
 * that link this backend should never reach get_input / invoke
 * on a real silicon build -- tflm (priority 50) wins on any
 * silicon_ref, and the vendor backends (ethos_u_aen at 100 on
 * AEN, ethos_u_n93 at 100 on N93, drpai_v2n at 100 on V2N,
 * deepx_dxm1 at 100 on DX-M1) win on theirs.
 *
 * Priority 0, silicon_ref=\"*\": picked only when no real backend
 * compiled in -- typically native_sim with CONFIG_TENSORFLOW_LITE_MICRO
 * absent.
 *
 * @par Cost: ROM ~250 B, RAM 0 bytes (no per-handle state beyond
 *      the dispatcher's pool slot).
 * @par Performance: O(1) on every call; deterministic NOSUPPORT
 *      returns mean test assertions don't need timing fences.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/inference.h>
#include <alp/peripheral.h>

#include "inference_ops.h"

static alp_status_t sw_open(const alp_inference_config_t  *cfg,
                            alp_inference_backend_state_t *state,
                            alp_capabilities_t            *caps_out)
{
	/* NOSUPPORT stub: no inference engine on native_sim.  The
     * dispatcher relays this as a NULL handle + last_error = NOSUPPORT. */
	(void)cfg;
	(void)state;
	(void)caps_out;
	return ALP_ERR_NOSUPPORT;
}

static size_t sw_num_inputs(alp_inference_backend_state_t *state)
{
	(void)state;
	return 0u;
}

static size_t sw_num_outputs(alp_inference_backend_state_t *state)
{
	(void)state;
	return 0u;
}

static alp_status_t
sw_get_input(alp_inference_backend_state_t *state, size_t index, alp_inference_tensor_t *out)
{
	(void)state;
	(void)index;
	(void)out;
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t
sw_get_output(alp_inference_backend_state_t *state, size_t index, alp_inference_tensor_t *out)
{
	(void)state;
	(void)index;
	(void)out;
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_invoke(alp_inference_backend_state_t *state)
{
	(void)state;
	return ALP_ERR_NOSUPPORT;
}

static const alp_inference_ops_t _ops = {
	.open        = sw_open,
	.num_inputs  = sw_num_inputs,
	.num_outputs = sw_num_outputs,
	.get_input   = sw_get_input,
	.get_output  = sw_get_output,
	.invoke      = sw_invoke,
	.close       = NULL,
};

ALP_BACKEND_REGISTER(inference,
                     sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_fallback",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
