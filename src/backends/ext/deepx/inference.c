/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr bodies for <alp/ext/deepx/inference.h>.
 *
 * Vendor-handle gate (mirrors src/backends/ext/alif/storage.c +
 * src/backends/ext/renesas/inference.c):
 *   - NULL handle / bad argument -> ALP_ERR_INVAL.
 *   - non-DEEPX backend -> ALP_ERR_NOT_PRESENT_ON_THIS_SOC.
 *
 * On Zephyr the vendor gate is always the terminal answer: the registry
 * ships NO deepx-vendor inference backend because the DX-M1 hangs off
 * the A55's PCIe and is driven by libdxrt on Linux only.  The Yocto
 * bodies live in src/yocto/inference_yocto.c (#482).
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/ext/deepx/inference.h>
#include <alp/inference.h>
#include <alp/peripheral.h>

#include "../../inference/inference_ops.h"

static bool _is_deepx_backend(const alp_inference_t *inf)
{
	return inf != NULL && inf->backend != NULL && inf->backend->vendor != NULL &&
	       strcmp(inf->backend->vendor, "deepx") == 0;
}

alp_status_t alp_deepx_inference_bind_cores(alp_inference_t *inf, alp_deepx_npu_cores_t cores)
{
	if (inf == NULL || (unsigned)cores > (unsigned)ALP_DEEPX_NPU_CORES_02) return ALP_ERR_INVAL;
	if (!_is_deepx_backend(inf)) return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
	return ALP_ERR_NOSUPPORT; /* no libdxrt on an M-class core */
}

alp_status_t alp_deepx_inference_get_status(alp_inference_t *inf, alp_deepx_device_status_t *status_out)
{
	if (inf == NULL || status_out == NULL) return ALP_ERR_INVAL;
	if (!_is_deepx_backend(inf)) return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
	return ALP_ERR_NOSUPPORT; /* no libdxrt on an M-class core */
}
