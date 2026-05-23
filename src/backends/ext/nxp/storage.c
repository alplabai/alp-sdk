/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bodies for <alp/ext/nxp/storage.h>.  FlexSPI OTFAD on i.MX 93 +
 * RT11xx.
 *
 * No SoM in scope ships the MCUXpresso FlexSPI OTFAD HAL pack yet,
 * so every function here returns ALP_ERR_NOSUPPORT after the
 * standard vendor-handle gating (NULL handle -> INVAL; non-NXP
 * backend -> NOT_PRESENT_ON_THIS_SOC).  When the MCUXpresso pack
 * lands, the bodies below grow real implementations without
 * changing the header or the gating.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/ext/nxp/storage.h>
#include <alp/storage.h>

#include "backends/storage/storage_ops.h"

/* OTFAD's hardware granularity for address-window bounds. */
#define _OTFAD_GRANULE  1024u

static bool _is_nxp_backend(const alp_storage_t *s) {
    return s != NULL && s->backend != NULL &&
           s->backend->vendor != NULL &&
           strcmp(s->backend->vendor, "nxp") == 0;
}

alp_status_t alp_nxp_storage_otfad_provision(alp_storage_t *s,
                                             uint8_t        window_id,
                                             const uint8_t *key,
                                             const uint8_t *counter)
{
    if (s == NULL) return ALP_ERR_INVAL;
    if (!_is_nxp_backend(s)) return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
    if (window_id >= ALP_NXP_STORAGE_OTFAD_WINDOW_COUNT) return ALP_ERR_INVAL;
    if (key == NULL || counter == NULL) return ALP_ERR_INVAL;
    /* OTFAD HAL pack not in scope yet -- body lands with the i.MX 93
     * / RT11xx vendor integration. */
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_nxp_storage_otfad_set_window(alp_storage_t *s,
                                              uint8_t        window_id,
                                              uint32_t       start_addr,
                                              uint32_t       end_addr)
{
    if (s == NULL) return ALP_ERR_INVAL;
    if (!_is_nxp_backend(s)) return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
    if (window_id >= ALP_NXP_STORAGE_OTFAD_WINDOW_COUNT) return ALP_ERR_INVAL;
    if (end_addr <= start_addr) return ALP_ERR_INVAL;
    if ((start_addr % _OTFAD_GRANULE) != 0u ||
        (end_addr   % _OTFAD_GRANULE) != 0u) {
        return ALP_ERR_INVAL;
    }
    return ALP_ERR_NOSUPPORT;
}
