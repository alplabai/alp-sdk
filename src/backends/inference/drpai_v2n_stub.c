/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Renesas DRP-AI3 inference backend -- NOT_IMPLEMENTED stub.
 *
 * Registers against silicon_ref="renesas:rzv2n:n44" at priority 100
 * so it wins over the portable tflm backend on V2N builds.  The
 * vtable entries all return ALP_ERR_NOT_IMPLEMENTED until the
 * Renesas DRP-AI vendor pack lands in CI alongside the V2N HIL
 * bring-up; the real body needs:
 *
 *   - The DRP-AI3 translator-output blob magic + header layout
 *     (Renesas RZ/V2N Hardware User's Manual, DRP-AI3 chapter).
 *   - The drpai_* IOCTL family on the device's /dev/drpai0 on
 *     Linux, or the FSP r_drpai driver on bare-metal.
 *   - NPU command-stream-decoder prime + invoke sequence (TBD
 *     register addresses documented in the Renesas vendor pack).
 *
 * Until then this stub takes priority over the portable tflm
 * backend on V2N so customers opting in to ALP_INFERENCE_BACKEND_AUTO
 * on V2N get a clear NOT_IMPLEMENTED signal that the DRP-AI path
 * is queued -- preferable to silently falling through to TFLM on
 * the M-class core (which would be much slower than DRP-AI3 and
 * misleading).
 *
 * @par Tracking: github.com/alplabai/alp-sdk/issues/58
 */

#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/inference.h>
#include <alp/peripheral.h>

#include "inference_ops.h"

static alp_status_t drpai_open(const alp_inference_config_t  *cfg,
                               alp_inference_backend_state_t *state,
                               alp_capabilities_t            *caps_out)
{
	(void)cfg;
	state->be_data  = NULL;
	state->dev      = NULL;
	caps_out->flags = 0u;
	/* The real body validates the .dat blob magic + version,
     * primes the DRP-AI command-stream decoder, and stores the
     * inbound tensor descriptors for get_input / get_output to
     * surface.  See issue #58 for the v0.x landing target. */
	return ALP_ERR_NOT_IMPLEMENTED;
}

static size_t drpai_num_inputs(alp_inference_backend_state_t *state)
{
	(void)state;
	return 0u;
}

static size_t drpai_num_outputs(alp_inference_backend_state_t *state)
{
	(void)state;
	return 0u;
}

static alp_status_t
drpai_get_input(alp_inference_backend_state_t *state, size_t index, alp_inference_tensor_t *out)
{
	(void)state;
	(void)index;
	(void)out;
	return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t
drpai_get_output(alp_inference_backend_state_t *state, size_t index, alp_inference_tensor_t *out)
{
	(void)state;
	(void)index;
	(void)out;
	return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t drpai_invoke(alp_inference_backend_state_t *state)
{
	(void)state;
	/* drpai_start + wait-for-completion lands with the vendor pack.
     * See issue #58. */
	return ALP_ERR_NOT_IMPLEMENTED;
}

static void drpai_close(alp_inference_backend_state_t *state)
{
	if (state != NULL) {
		state->be_data = NULL;
	}
}

static const alp_inference_ops_t _ops = {
	.open        = drpai_open,
	.num_inputs  = drpai_num_inputs,
	.num_outputs = drpai_num_outputs,
	.get_input   = drpai_get_input,
	.get_output  = drpai_get_output,
	.invoke      = drpai_invoke,
	.close       = drpai_close,
};

ALP_BACKEND_REGISTER(inference,
                     drpai_v2n,
                     {
                         .silicon_ref = "renesas:rzv2n:n44",
                         .vendor      = "renesas",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
