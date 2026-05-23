/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bodies for <alp/ext/deepx/inference.h>.  DX-M1 slot + DRAM
 * tile knobs.
 *
 * Vendor-handle gate (mirrors src/backends/ext/alif/storage.c +
 * src/backends/ext/renesas/inference.c):
 *   - NULL handle -> ALP_ERR_INVAL.
 *   - non-DEEPX backend -> ALP_ERR_NOT_PRESENT_ON_THIS_SOC.
 *
 * After the gate the calls return NOSUPPORT today; the
 * deepx_dxm1 backend itself is a NOT_IMPLEMENTED stub (see
 * issue #59).  When the DEEPX SDK adapter lands, the bodies
 * below dispatch through the per-handle slot + tile state.
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

/* V2N-M1 SoM dedicates 256 MB of DDR to DX-M1.  Used to clamp
 * tile reservations; the real body checks against the live
 * SDK-reported carve-out when the vendor pack lands. */
#define ALP_DEEPX_INFERENCE_DDR_CARVEOUT_BYTES (256u * 1024u * 1024u)

static bool _is_deepx_backend(const alp_inference_t *inf)
{
    return inf != NULL && inf->backend != NULL &&
           inf->backend->vendor != NULL &&
           strcmp(inf->backend->vendor, "deepx") == 0;
}

alp_status_t alp_deepx_inference_slot_pin(alp_inference_t *inf,
                                          alp_deepx_inference_slot_t slot)
{
    if (inf == NULL) return ALP_ERR_INVAL;
    if (!_is_deepx_backend(inf)) return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
    if ((unsigned)slot >= ALP_DEEPX_INFERENCE_SLOT_COUNT) {
        return ALP_ERR_INVAL;
    }
    /* Slot-pin enforcement lands when the deepx_dxm1 backend
     * grows past its NOT_IMPLEMENTED stub (issue #59).  The
     * BUSY check against other in-flight handles requires
     * adapter-side reservation state. */
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_deepx_inference_dram_tile_reserve(alp_inference_t *inf,
                                                   uint32_t         tile_bytes)
{
    if (inf == NULL) return ALP_ERR_INVAL;
    if (!_is_deepx_backend(inf)) return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
    if (tile_bytes == 0u) return ALP_ERR_OUT_OF_RANGE;
    if (tile_bytes > ALP_DEEPX_INFERENCE_DDR_CARVEOUT_BYTES) {
        return ALP_ERR_OUT_OF_RANGE;
    }
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_deepx_inference_get_status(alp_inference_t *inf,
                                            uint32_t        *status_out)
{
    if (inf == NULL || status_out == NULL) return ALP_ERR_INVAL;
    if (!_is_deepx_backend(inf)) return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
    *status_out = (uint32_t)ALP_DEEPX_INFERENCE_STATUS_IDLE;
    return ALP_ERR_NOSUPPORT;
}
