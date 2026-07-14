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

#ifndef CONFIG_ALP_SDK_MAX_WDT_HANDLES
#define CONFIG_ALP_SDK_MAX_WDT_HANDLES 2
#endif

static struct alp_wdt _pool[CONFIG_ALP_SDK_MAX_WDT_HANDLES];

static struct alp_wdt *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_WDT_HANDLES; ++i) {
		/* Atomic claim: only the winner of the flag flip may touch the
		 * slot's other fields (in_use is the struct's last member, so
		 * zero everything before it -- incl. lifecycle/active_ops,
		 * parking a fresh slot at LC_UNOPENED). Issue #629. */
		if (alp_slot_try_claim(&_pool[i].in_use)) {
			memset(&_pool[i], 0, offsetof(struct alp_wdt, in_use));
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_wdt *h)
{
	alp_slot_release(&h->in_use);
}

alp_wdt_t *alp_wdt_open(const alp_wdt_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL || cfg->timeout_ms == 0u) {
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
	struct alp_wdt *h = _alloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
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
	alp_status_t rc = ops->open(cfg, &h->state, &caps);
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
	alp_status_t rc = h->state.ops->feed(&h->state);
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_wdt_disable(alp_wdt_t *h)
{
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc = h->state.ops->disable(&h->state);
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
	if (!alp_handle_begin_close(&h->lifecycle, &h->active_ops)) {
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
