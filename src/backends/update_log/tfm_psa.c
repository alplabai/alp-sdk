/* SPDX-License-Identifier: Apache-2.0
 *
 * Hardware-enforced TF-M update-log client for <alp/update_log.h>.
 *
 * This is the TrustZone-M route for Alif E4/E8-class parts: the non-secure
 * application calls the portable update-log API, while the actual writer and
 * store live in a secure owner that the application cannot rewrite. On Alif
 * silicon that secure owner must use storage protected by the SE-programmed
 * attribution/firewall policy.
 *
 * This file intentionally contains no direct PSA Protected Storage calls. A
 * normal non-secure application that owns the PSA UIDs itself would still be
 * able to rewrite its own history. The backend below only forwards to the
 * secure-service seam; if no secure owner is linked/provisioned, ready()
 * returns NOSUPPORT and the dispatcher falls through to the software tier.
 */

#include "alp/update_log.h"

#if defined(CONFIG_ALP_SDK_UPDATE_LOG_TFM)

#include <stdint.h>

#include "alp/backend.h"
#include "backends/update_log/update_log_ops.h"
#include "update_log/secure_service.h"

#if defined(__GNUC__) || defined(__clang__)
#define ALP_WEAK __attribute__((weak))
#else
#define ALP_WEAK
#endif

ALP_WEAK alp_status_t alp_update_log_secure_ready(void)
{
	return ALP_ERR_NOSUPPORT;
}

ALP_WEAK alp_status_t alp_update_log_secure_append(const alp_update_log_entry_t *entry)
{
	return (entry != NULL) ? ALP_ERR_NOSUPPORT : ALP_ERR_INVAL;
}

ALP_WEAK alp_status_t alp_update_log_secure_verify(alp_update_log_verdict_t *verdict,
                                                   uint64_t                 *bad_seq)
{
	(void)bad_seq;
	return (verdict != NULL) ? ALP_ERR_NOSUPPORT : ALP_ERR_INVAL;
}

ALP_WEAK alp_status_t alp_update_log_secure_count(uint64_t *out)
{
	return (out != NULL) ? ALP_ERR_NOSUPPORT : ALP_ERR_INVAL;
}

ALP_WEAK alp_status_t alp_update_log_secure_get(uint64_t seq, alp_update_log_entry_t *out)
{
	(void)seq;
	return (out != NULL) ? ALP_ERR_NOSUPPORT : ALP_ERR_INVAL;
}

static alp_status_t tfm_ready(void)
{
	return alp_update_log_secure_ready();
}

static alp_status_t tfm_append(const alp_update_log_entry_t *entry)
{
	return alp_update_log_secure_append(entry);
}

static alp_status_t tfm_verify(alp_update_log_verdict_t *verdict, uint64_t *bad_seq)
{
	return alp_update_log_secure_verify(verdict, bad_seq);
}

static alp_status_t tfm_count(uint64_t *out)
{
	return alp_update_log_secure_count(out);
}

static alp_status_t tfm_get(uint64_t seq, alp_update_log_entry_t *out)
{
	return alp_update_log_secure_get(seq, out);
}

static const alp_update_log_ops_t _tfm_ops = {
	.assurance = ALP_UPDATE_LOG_HW_ENFORCED,
	.ready     = tfm_ready,
	.append    = tfm_append,
	.verify    = tfm_verify,
	.count     = tfm_count,
	.get       = tfm_get,
};

ALP_BACKEND_REGISTER(update_log,
                     tfm_psa,
                     {
                         .silicon_ref = "*",
                         .vendor      = "tfm_psa",
                         .base_caps   = 0u,
                         .priority    = 20,
                         .ops         = &_tfm_ops,
                         .probe       = NULL,
                     });

#endif /* CONFIG_ALP_SDK_UPDATE_LOG_TFM */
