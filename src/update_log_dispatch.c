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
#include "update_log/boot_metadata.h"

ALP_BACKEND_DEFINE_CLASS(update_log);
ALP_BACKEND_ANCHOR(update_log);

#include "alp_slot_claim.h"
#include "alp_z_last_error.h"

static struct alp_update_log g_log;

/* Dispatcher-local intermediate lifecycle state (issue #629). The generic
 * guard in alp_slot_claim.h models UNOPENED/OPEN/CLOSING; the update_log
 * singleton adds OPENING to serialize concurrent FIRST opens -- exactly one
 * caller CASes UNOPENED->OPENING and runs the ranked backend walk, while
 * racing opens spin until it resolves to OPEN (return the singleton) or back
 * to UNOPENED (initializer failed -> a spinner retries as the initializer).
 * The value is distinct from LC_UNOPENED(0)/OPEN(1)/CLOSING(2). */
#define UL_LC_OPENING 3u

/* Run the ranked backend walk and publish the chosen ops into g_log. Caller
 * must hold the OPENING election. Returns ALP_OK on success (g_log populated),
 * else the failure status. */
static alp_status_t _elect_backend(void)
{
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
#if defined(CONFIG_ALP_SDK_UPDATE_LOG_REQUIRE_HW_ENFORCED)
				if (ops->assurance != ALP_UPDATE_LOG_HW_ENFORCED) {
					rc = ALP_ERR_NOSUPPORT;
					be = alp_backend_select_next("update_log", ALP_SOC_REF_STR, be);
					continue;
				}
#endif
				g_log.ops       = ops;
				g_log.assurance = ops->assurance;
				return ALP_OK;
			}
			if (rc != ALP_ERR_NOSUPPORT) break;
		}
		be = alp_backend_select_next("update_log", ALP_SOC_REF_STR, be);
	}
	return rc;
}

alp_update_log_t *alp_update_log_open(void)
{
	alp_z_clear_last_error();

	/* Idempotent singleton open, made race-safe (issue #629): the pre-fix
	 * `if (g_log.in_use) ...; g_log.in_use = true;` let two racing first-
	 * opens both run the backend walk and double-init g_log. Now exactly
	 * one caller wins the UNOPENED->OPENING election; the rest observe the
	 * outcome. */
	for (;;) {
		uint8_t s = alp_lifecycle_get(&g_log.lifecycle);
		if (s == ALP_HANDLE_LC_OPEN) {
			return &g_log; /* already initialised */
		}
		if (s == ALP_HANDLE_LC_UNOPENED) {
			if (!alp_lifecycle_cas(&g_log.lifecycle, ALP_HANDLE_LC_UNOPENED, UL_LC_OPENING)) {
				continue; /* lost the election; re-observe */
			}
			/* Elected initializer. */
			alp_status_t rc = _elect_backend();
			if (rc == ALP_OK) {
				alp_lifecycle_set(&g_log.lifecycle, ALP_HANDLE_LC_OPEN);
				return &g_log;
			}
			alp_z_set_last_error(rc);
			alp_lifecycle_set(&g_log.lifecycle, ALP_HANDLE_LC_UNOPENED);
			return NULL;
		}
		/* OPENING or CLOSING in flight: spin until it resolves, then loop. */
	}
}

alp_status_t alp_update_log_append(alp_update_log_t *log, const alp_update_log_entry_t *entry)
{
	if (log == NULL || entry == NULL) return ALP_ERR_INVAL;
	if (!alp_handle_op_enter(&log->lifecycle, &log->active_ops)) return ALP_ERR_INVAL;
	alp_status_t rc =
	    (log->ops->append == NULL) ? ALP_ERR_NOT_IMPLEMENTED : log->ops->append(entry);
	alp_handle_op_leave(&log->active_ops);
	return rc;
}

alp_status_t alp_update_log_entry_from_boot_metadata(alp_update_log_entry_t *entry_out,
                                                     uint64_t                timestamp)
{
	if (entry_out == NULL) return ALP_ERR_INVAL;

	alp_update_log_entry_t entry = { 0 };
	alp_status_t           rc    = alp_update_log_boot_metadata_read(&entry);
	if (rc != ALP_OK) return rc;

	entry.seq       = 0;
	entry.timestamp = timestamp;
	*entry_out      = entry;
	return ALP_OK;
}

alp_status_t alp_update_log_append_boot(alp_update_log_t *log, uint64_t timestamp)
{
	if (log == NULL) return ALP_ERR_INVAL;
	if (!alp_handle_op_enter(&log->lifecycle, &log->active_ops)) return ALP_ERR_INVAL;

	alp_status_t rc;
	if (log->ops->append == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		alp_update_log_entry_t entry = { 0 };
		rc                           = alp_update_log_entry_from_boot_metadata(&entry, timestamp);
		if (rc == ALP_OK) {
			rc = log->ops->append(&entry);
		}
	}
	alp_handle_op_leave(&log->active_ops);
	return rc;
}

alp_status_t alp_update_log_verify(alp_update_log_t         *log,
                                   alp_update_log_verdict_t *verdict_out,
                                   uint64_t                 *bad_seq_out)
{
	if (log == NULL || verdict_out == NULL) return ALP_ERR_INVAL;
	if (!alp_handle_op_enter(&log->lifecycle, &log->active_ops)) return ALP_ERR_INVAL;
	alp_status_t rc = (log->ops->verify == NULL) ? ALP_ERR_NOT_IMPLEMENTED
	                                             : log->ops->verify(verdict_out, bad_seq_out);
	alp_handle_op_leave(&log->active_ops);
	return rc;
}

alp_status_t alp_update_log_count(alp_update_log_t *log, uint64_t *count_out)
{
	if (log == NULL || count_out == NULL) return ALP_ERR_INVAL;
	if (!alp_handle_op_enter(&log->lifecycle, &log->active_ops)) return ALP_ERR_INVAL;
	alp_status_t rc =
	    (log->ops->count == NULL) ? ALP_ERR_NOT_IMPLEMENTED : log->ops->count(count_out);
	alp_handle_op_leave(&log->active_ops);
	return rc;
}

alp_status_t
alp_update_log_get(alp_update_log_t *log, uint64_t seq, alp_update_log_entry_t *entry_out)
{
	if (log == NULL || entry_out == NULL) return ALP_ERR_INVAL;
	if (!alp_handle_op_enter(&log->lifecycle, &log->active_ops)) return ALP_ERR_INVAL;
	alp_status_t rc =
	    (log->ops->get == NULL) ? ALP_ERR_NOT_IMPLEMENTED : log->ops->get(seq, entry_out);
	alp_handle_op_leave(&log->active_ops);
	return rc;
}

alp_update_log_assurance_t alp_update_log_assurance(const alp_update_log_t *log)
{
	return (log != NULL) ? log->assurance : ALP_UPDATE_LOG_SW_TAMPER_EVIDENT;
}

void alp_update_log_close(alp_update_log_t *log)
{
	if (log == NULL) return;
	/* Drain in-flight ops before dropping the singleton back to UNOPENED so
	 * a fresh open() re-elects a backend (issue #629). Idempotent: a second
	 * close, or a close racing an in-flight open, no-ops. */
	if (!alp_handle_begin_close(&log->lifecycle, &log->active_ops)) return;
	log->ops = NULL;
	alp_lifecycle_set(&log->lifecycle, ALP_HANDLE_LC_UNOPENED);
}
