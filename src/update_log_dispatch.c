/* SPDX-License-Identifier: Apache-2.0
 *
 * <alp/update_log.h> dispatcher. One log per device: a single static handle
 * bound to the selected backend. Mirrors the TMU stateless-cache pattern --
 * the surface is effectively a singleton, so we cache the chosen ops on the
 * first open().
 *
 * Tier selection (issues #111, #239). Two tiers can register into the
 * update_log class section: the universal SW tamper-evident tier
 * (silicon_ref="*", priority 10) and -- only where the platform advertises
 * TF-M isolation (CONFIG_ALP_SDK_UPDATE_LOG_TFM) -- the HW_ENFORCED tier
 * (higher priority). alp_backend_select() therefore hands us the HW tier
 * first where it exists. But a higher-priority tier may not be able to
 * serve on this boot (the HW tier is a stub until its secure store +
 * monotonic counter land -- issue #111), so open() calls the backend's
 * optional ready() probe and, on ALP_ERR_NOSUPPORT, walks down to the next
 * candidate via alp_backend_select_next() -- the same open-time
 * fall-through the security dispatcher uses (issue #239). Net effect: the
 * HW tier is preferred wherever it is genuinely ready, and the SW tier
 * transparently serves everywhere else.
 */
#include <stdbool.h>
#include <stddef.h>

#include "alp/backend.h"
#include "alp/peripheral.h"
#include "alp/soc_caps.h"
#include "alp/update_log.h"
#include "backends/update_log/update_log_ops.h"

ALP_BACKEND_DEFINE_CLASS(update_log);

extern void alp_z_set_last_error(alp_status_t s);
extern void alp_z_clear_last_error(void);

static struct alp_update_log g_log;

alp_update_log_t *alp_update_log_open(void)
{
	alp_z_clear_last_error();
	if (g_log.in_use) return &g_log;

	/* Ranked walk with open-time fall-through (issues #111, #239): try the
	 * top backend, and if it declines this boot with ALP_ERR_NOSUPPORT
	 * (e.g. the HW_ENFORCED TF-M tier before it is provisioned) drop to the
	 * next candidate. A ready() of NULL means "always ready". Any status
	 * other than NOSUPPORT is a real failure and is surfaced. */
	const alp_backend_t *be = alp_backend_select("update_log", ALP_SOC_REF_STR);
	alp_status_t         rc = ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
	while (be != NULL) {
		const alp_update_log_ops_t *ops = (const alp_update_log_ops_t *)be->ops;
		if (ops != NULL) {
			rc = (ops->ready != NULL) ? ops->ready() : ALP_OK;
			if (rc == ALP_OK) {
				g_log.ops       = ops;
				g_log.assurance = ops->assurance;
				g_log.in_use    = true;
				return &g_log;
			}
			if (rc != ALP_ERR_NOSUPPORT) break;
		}
		be = alp_backend_select_next("update_log", ALP_SOC_REF_STR, be);
	}
	alp_z_set_last_error(rc);
	return NULL;
}

alp_status_t alp_update_log_append(alp_update_log_t *log, const alp_update_log_entry_t *entry)
{
	if (log == NULL || !log->in_use || entry == NULL) return ALP_ERR_INVAL;
	if (log->ops->append == NULL) return ALP_ERR_NOT_IMPLEMENTED;
	return log->ops->append(entry);
}

alp_status_t alp_update_log_verify(alp_update_log_t         *log,
                                   alp_update_log_verdict_t *verdict_out,
                                   uint64_t                 *bad_seq_out)
{
	if (log == NULL || !log->in_use || verdict_out == NULL) return ALP_ERR_INVAL;
	if (log->ops->verify == NULL) return ALP_ERR_NOT_IMPLEMENTED;
	return log->ops->verify(verdict_out, bad_seq_out);
}

alp_status_t alp_update_log_count(alp_update_log_t *log, uint64_t *count_out)
{
	if (log == NULL || !log->in_use || count_out == NULL) return ALP_ERR_INVAL;
	if (log->ops->count == NULL) return ALP_ERR_NOT_IMPLEMENTED;
	return log->ops->count(count_out);
}

alp_status_t
alp_update_log_get(alp_update_log_t *log, uint64_t seq, alp_update_log_entry_t *entry_out)
{
	if (log == NULL || !log->in_use || entry_out == NULL) return ALP_ERR_INVAL;
	if (log->ops->get == NULL) return ALP_ERR_NOT_IMPLEMENTED;
	return log->ops->get(seq, entry_out);
}

alp_update_log_assurance_t alp_update_log_assurance(const alp_update_log_t *log)
{
	return (log != NULL) ? log->assurance : ALP_UPDATE_LOG_SW_TAMPER_EVIDENT;
}

void alp_update_log_close(alp_update_log_t *log)
{
	if (log != NULL) log->in_use = false;
}
