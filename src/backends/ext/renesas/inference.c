/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bodies for <alp/ext/renesas/inference.h>.  DRP-AI3 pipeline-stage
 * + AI-SRAM controls on Renesas RZ/V2N N44.
 *
 * Vendor-handle gate (mirrors src/backends/ext/renesas/camera.c +
 * src/backends/ext/alif/storage.c):
 *   - NULL handle -> ALP_ERR_INVAL.
 *   - non-Renesas backend -> ALP_ERR_NOT_PRESENT_ON_THIS_SOC.
 *
 * After the gate the calls return NOSUPPORT today; the drpai_v2n
 * backend itself is a NOT_IMPLEMENTED stub (see issue #58) so
 * there is no per-handle state to reach into yet.  When the real
 * DRP-AI body lands, the bodies below grow into pokes against the
 * NPU register surface (Renesas RZ/V2N Hardware User's Manual,
 * DRP-AI3 chapter -- TBD register addresses and bit layouts
 * documented in the vendor pack).
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/ext/renesas/inference.h>
#include <alp/inference.h>
#include <alp/peripheral.h>

#include "../../inference/inference_ops.h"

/* DRP-AI3 AI-SRAM physical size on the N44 (1.5 MB per the
 * Renesas datasheet).  Used to clamp reservation requests; the
 * real body checks against the live carve-out from the
 * translator-emitted runtime descriptor when the vendor pack
 * lands. */
#define ALP_RENESAS_INFERENCE_AI_SRAM_BYTES (1536u * 1024u)

static bool _is_renesas_backend(const alp_inference_t *inf)
{
    return inf != NULL && inf->backend != NULL &&
           inf->backend->vendor != NULL &&
           strcmp(inf->backend->vendor, "renesas") == 0;
}

alp_status_t alp_renesas_inference_pipeline_stage_pin(alp_inference_t *inf,
                                                      uint32_t         layer_index,
                                                      alp_renesas_inference_stage_t stage)
{
    if (inf == NULL) return ALP_ERR_INVAL;
    if (!_is_renesas_backend(inf)) return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
    if ((unsigned)stage > (unsigned)ALP_RENESAS_INFERENCE_STAGE_DMA) {
        return ALP_ERR_INVAL;
    }
    (void)layer_index;
    /* Per-layer pin lands when the drpai_v2n backend grows past
     * its NOT_IMPLEMENTED stub (issue #58).  The layer-count
     * range check requires translator metadata which only the
     * real body has access to. */
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_renesas_inference_ai_sram_reserve(alp_inference_t *inf,
                                                   uint32_t         reserve_bytes)
{
    if (inf == NULL) return ALP_ERR_INVAL;
    if (!_is_renesas_backend(inf)) return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
    if (reserve_bytes == 0u) return ALP_ERR_OUT_OF_RANGE;
    if (reserve_bytes > ALP_RENESAS_INFERENCE_AI_SRAM_BYTES) {
        return ALP_ERR_OUT_OF_RANGE;
    }
    /* AI-SRAM reservation pokes happen when the drpai_v2n backend
     * grows past its NOT_IMPLEMENTED stub. */
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_renesas_inference_get_status(alp_inference_t *inf,
                                              uint32_t        *status_out)
{
    if (inf == NULL || status_out == NULL) return ALP_ERR_INVAL;
    if (!_is_renesas_backend(inf)) return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
    *status_out = (uint32_t)ALP_RENESAS_INFERENCE_STATUS_IDLE;
    return ALP_ERR_NOSUPPORT;
}
