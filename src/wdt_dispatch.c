/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * WDT class dispatcher.  Owns the public alp_wdt_* surface and
 * routes through the .alp_backends_wdt registry.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>
#include <alp/wdt.h>

#include "alp_slot_claim.h"
#include "backends/wdt/wdt_ops.h"

ALP_BACKEND_DEFINE_CLASS(wdt);
/* Pull the wdt registry section into a static-archive link (#368). */
ALP_BACKEND_ANCHOR(wdt);

#include "alp_z_last_error.h"

/* The pool is indexed by wdt_id: one slot per watchdog INSTANCE, not
 * one per caller.  The watchdog is the class where a second handle on
 * the same instance is itself the defect: before #1637, the backend
 * close path disabled the whole DEVICE rather than the handle's
 * channel (src/backends/wdt/zephyr_drv.c z_close used to call
 * wdt_disable(dev) with the error (void)-cast away), so two
 * subsystems each holding ALP_E1M_WDT0 meant the first one to close
 * silently removed the other's protection, with no error on any path.
 * z_close() no longer calls wdt_disable(dev) at all -- see its own
 * comment for what closing does and does not disarm now -- so it is
 * indexing by id, below, that actually prevents this class of defect:
 * making the existing atomic slot claim BE the exclusivity check, one
 * compare-exchange, no scan of the pool, and no TOCTOU window between
 * "is this instance taken?" and "take it".  Issue #1637.
 *
 * The pool size is therefore also the portable instance bound, which
 * is exactly what the public surface names -- ALP_E1M_WDT0..1 /
 * ALP_E1M_X_WDT0..1 are 0u and 1u (include/alp/e1m_pinout.h:165-166,
 * include/alp/e1m_x_pinout.h:194-195). */
#ifndef CONFIG_ALP_SDK_MAX_WDT_HANDLES
#define CONFIG_ALP_SDK_MAX_WDT_HANDLES 2
#endif

static struct alp_wdt _pool[CONFIG_ALP_SDK_MAX_WDT_HANDLES];

/* Claim the slot belonging to @p wdt_id.  Caller has already bounded
 * wdt_id against the pool.  NULL means the instance is already open. */
static struct alp_wdt *_alloc(uint32_t wdt_id)
{
	/* Atomic claim: only the winner of the flag flip may touch the
	 * slot's other fields (in_use is the struct's last member, so
	 * zero everything before it -- incl. lifecycle/active_ops,
	 * parking a fresh slot at LC_UNOPENED). Issue #629. */
	if (!alp_slot_try_claim(&_pool[wdt_id].in_use)) {
		return NULL;
	}
	memset(&_pool[wdt_id], 0, offsetof(struct alp_wdt, in_use));
	return &_pool[wdt_id];
}

static void _free(struct alp_wdt *h)
{
	alp_slot_release(&h->in_use);
}

alp_wdt_t *alp_wdt_open(const alp_wdt_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL || cfg->timeout_ms == 0u ||
	    cfg->wdt_id >= (uint32_t)CONFIG_ALP_SDK_MAX_WDT_HANDLES ||
	    (cfg->on_timeout == ALP_WDT_INTERRUPT_ONLY && cfg->on_expire == NULL)) {
		/* The last arm rejects an INTERRUPT_ONLY request with no way to
		 * observe the interrupt -- that combination neither resets the
		 * SoC nor notifies anyone, which is strictly worse than not
		 * offering the mode at all (#1637). */
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("wdt", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_wdt_ops_t *ops = (const alp_wdt_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_wdt *h = _alloc(cfg->wdt_id);
	if (h == NULL) {
		/* The instance already has an owner.  Not NOMEM: the pool is
		 * indexed by wdt_id, so a failed claim is always "this
		 * watchdog is taken", never "no free slots". */
		alp_z_set_last_error(ALP_ERR_BUSY);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	alp_capabilities_t caps = { .flags = be->base_caps };
	if (be->probe != NULL) {
		uint32_t refined = caps.flags;
		(void)be->probe(cfg->wdt_id, &refined);
		caps.flags = refined;
	}
	/* h is passed through as the owner back-ref: the Zephyr backend's
	 * ISR trampoline needs it to reach cfg.on_expire/cfg.user, because
	 * Zephyr's wdt_callback_t (unlike counter's counter_alarm_cfg or
	 * RTC's) carries no user_data cookie of its own (#1637). */
	alp_status_t rc = ops->open(cfg, &h->state, &caps, h);
	if (rc != ALP_OK) {
		_free(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
}

alp_status_t alp_wdt_feed(alp_wdt_t *h)
{
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (h->state.ops->feed == NULL) {
		rc = ALP_ERR_NOSUPPORT;
	} else {
		rc = h->state.ops->feed(&h->state);
	}
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_wdt_disable(alp_wdt_t *h)
{
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (h->state.ops->disable == NULL) {
		rc = ALP_ERR_NOSUPPORT;
	} else {
		rc = h->state.ops->disable(&h->state);
	}
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

void alp_wdt_close(alp_wdt_t *h)
{
	if (h == NULL) {
		return;
	}
	/* begin_close CAS OPEN->CLOSING then spins until every op that
	 * entered before the CAS has left -- so teardown never races an
	 * in-flight op. Idempotent: a second/never-opened close no-ops. #629 */
	if (!alp_handle_begin_close_blocking(&h->lifecycle, &h->active_ops)) {
		return;
	}
	if (h->state.ops != NULL && h->state.ops->close != NULL) {
		h->state.ops->close(&h->state);
	}
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free(h);
}

const alp_capabilities_t *alp_wdt_capabilities(const alp_wdt_t *h)
{
	return (h != NULL) ? &h->cached_caps : NULL;
}
