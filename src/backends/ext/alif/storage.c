/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bodies for <alp/ext/alif/storage.h>.  OSPI SecAES engine on
 * Ensemble E4 / E6 / E8.
 *
 * No SoM in scope ships the SecAES HAL pack yet, so every function
 * here returns ALP_ERR_NOSUPPORT after the standard vendor-handle
 * gating (NULL handle -> INVAL; non-Alif backend ->
 * NOT_PRESENT_ON_THIS_SOC).  When the Alif HAL OSPI driver lands
 * its SecAES surface, the bodies below grow real implementations
 * without changing the header or the gating.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/ext/alif/storage.h>
#include <alp/storage.h>

#include "backends/storage/storage_ops.h"

static bool _is_alif_backend(const alp_storage_t *s) {
    return s != NULL && s->backend != NULL &&
           s->backend->vendor != NULL &&
           strcmp(s->backend->vendor, "alif") == 0;
}

alp_status_t alp_alif_storage_secaes_key_provision(alp_storage_t *s,
                                                   const uint8_t *key,
                                                   uint8_t        key_bytes)
{
    if (s == NULL) return ALP_ERR_INVAL;
    if (!_is_alif_backend(s)) return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
    if (key == NULL) return ALP_ERR_INVAL;
    if (key_bytes != 16u && key_bytes != 24u && key_bytes != 32u) {
        return ALP_ERR_INVAL;
    }
    /* SecAES HAL pack not in scope yet -- body lands with the
     * Ensemble E4 / E6 / E8 vendor integration. */
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_alif_storage_secaes_get_status(alp_storage_t *s,
                                                uint32_t      *status_out)
{
    if (s == NULL || status_out == NULL) return ALP_ERR_INVAL;
    if (!_is_alif_backend(s)) return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
    *status_out = (uint32_t)ALP_ALIF_STORAGE_SECAES_STATUS_IDLE;
    return ALP_ERR_NOSUPPORT;
}
