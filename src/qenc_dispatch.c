/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * QEnc class dispatcher.  Routes the public alp_qenc_* API
 * through the .alp_backends_qenc registry.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/counter.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "alp_slot_claim.h"
#include "backends/qenc/qenc_ops.h"

ALP_BACKEND_DEFINE_CLASS(qenc);

#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_MAX_QENC_HANDLES
#define CONFIG_ALP_SDK_MAX_QENC_HANDLES 4
#endif

static struct alp_qenc _pool[CONFIG_ALP_SDK_MAX_QENC_HANDLES];

static struct alp_qenc *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_QENC_HANDLES; ++i) {
		/* Atomic claim: only the winner of the flag flip may touch the
		 * slot's other fields (in_use is the struct's last member, so
		 * zero everything before it -- incl. lifecycle/active_ops,
		 * parking a fresh slot at LC_UNOPENED). Issue #629. */
		if (alp_slot_try_claim(&_pool[i].in_use)) {
			memset(&_pool[i], 0, offsetof(struct alp_qenc, in_use));
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_qenc *h)
{
	alp_slot_release(&h->in_use);
}

alp_qenc_t *alp_qenc_open(const alp_qenc_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("qenc", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	struct alp_qenc *h = _alloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	/* Open-time fall-through (mirrors security_dispatch.c's
	 * hash_open, issue #239): the gpio_qdec backend (priority 110)
	 * declines with ALP_ERR_NOSUPPORT for an alp-qenc<N> alias that
	 * isn't a gpio-qdec node, so we walk down to the next candidate
	 * (zephyr_drv, priority 100) instead of failing the whole open --
	 * issue #2095.  Any other status is a real failure and is
	 * surfaced immediately.
	 *
	 * The walk stops before the vendor "sw_fallback" candidate: a
	 * real backend declining NOSUPPORT means "wrong shape for this
	 * alias" (try the next real backend), not "no real backend
	 * exists" -- silently landing on sw_fallback's fake counter here
	 * would return a plausible-looking position for hardware that
	 * was never actually opened. */
	alp_status_t rc = ALP_ERR_NOT_IMPLEMENTED;
	while (be != NULL && (be->vendor == NULL || strcmp(be->vendor, "sw_fallback") != 0)) {
		const alp_qenc_ops_t *ops = (const alp_qenc_ops_t *)be->ops;
		if (ops != NULL && ops->open != NULL) {
			memset(&h->state, 0, sizeof(h->state));
			h->state.ops            = ops;
			alp_capabilities_t caps = { .flags       = be->base_caps,
				                        .class_flags = be->base_class_flags };
			if (be->probe != NULL) {
				uint32_t refined = caps.flags;
				(void)be->probe(cfg->encoder_id, &refined);
				caps.flags = refined;
			}
			rc = ops->open(cfg, &h->state, &caps);
			if (rc == ALP_OK) {
				h->backend     = be;
				h->cached_caps = caps;
				alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
				return h;
			}
			if (rc != ALP_ERR_NOSUPPORT) {
				break;
			}
		}
		be = alp_backend_select_next("qenc", ALP_SOC_REF_STR, be);
	}
	_free(h);
	alp_z_set_last_error(rc);
	return NULL;
}

alp_status_t alp_qenc_get_position(alp_qenc_t *h, int32_t *pos_out)
{
	if (pos_out == NULL) return ALP_ERR_INVAL;
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (h->state.ops->get_position == NULL) {
		rc = ALP_ERR_NOSUPPORT;
	} else {
		rc = h->state.ops->get_position(&h->state, pos_out);
	}
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_qenc_reset_position(alp_qenc_t *h)
{
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (h->state.ops->reset_position == NULL) {
		rc = ALP_ERR_NOSUPPORT;
	} else {
		rc = h->state.ops->reset_position(&h->state);
	}
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

void alp_qenc_close(alp_qenc_t *h)
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

const alp_capabilities_t *alp_qenc_capabilities(const alp_qenc_t *h)
{
	return (h != NULL) ? &h->cached_caps : NULL;
}
