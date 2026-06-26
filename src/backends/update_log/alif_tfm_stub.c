/* SPDX-License-Identifier: Apache-2.0
 *
 * Alif TF-M hardware-enforced tier for <alp/update_log.h> -- SEAM ONLY.
 *
 * This file documents and reserves the shape of the HW_ENFORCED backend:
 *   - store_if  -> TF-M PSA Protected Storage (psa_ps_set/get), an asset in
 *     the Secure Processing Environment unreachable by the non-secure app.
 *   - counter   -> a hardware monotonic counter (PSA NV counter or OPTIGA),
 *     non-decrementable.
 *   - assurance -> ALP_UPDATE_LOG_HW_ENFORCED.
 *
 * It is COMPILED (so it cannot bitrot) but deliberately NOT registered via
 * ALP_BACKEND_REGISTER: until the Alif board file + TF-M build land and the
 * acceptance criteria in the design spec pass on silicon, the software tier
 * serves every SoM. Wiring this up is a later slice. Every entry point
 * returns ALP_ERR_NOSUPPORT.
 *
 * @par Tracking: github.com/alplabai/alp-sdk/issues/111
 */
#include "alp/update_log.h"
#include "update_log/store.h"

alp_status_t alp_update_log_alif_tfm_store_put(void *c, const char *k, const uint8_t *b, size_t n)
{
	(void)c;
	(void)k;
	(void)b;
	(void)n;
	return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_update_log_alif_tfm_counter_read(void *c, uint32_t id, uint64_t *v)
{
	(void)c;
	(void)id;
	(void)v;
	return ALP_ERR_NOSUPPORT;
}
