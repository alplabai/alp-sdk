/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Display class dispatcher.  Owns the public alp_display_* API
 * surface and routes through the backend registry.
 *
 * The handle struct layout (struct alp_display) lives in
 * src/backends/display/display_ops.h so per-backend .c files can
 * reach the fields without duplicating the layout.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/display.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "alp_slot_claim.h"
#include "backends/display/display_ops.h"

ALP_BACKEND_DEFINE_CLASS(display);
ALP_BACKEND_ANCHOR(display);

/* Reuse the existing TLS-backed last-error mechanism from
 * src/zephyr/last_error.c.  Forward-declared here to avoid pulling
 * in the broader handles.h header (which carries unrelated
 * peripheral pool declarations the dispatcher does not touch). */
#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_MAX_DISPLAY_HANDLES
#define CONFIG_ALP_SDK_MAX_DISPLAY_HANDLES 2
#endif

static struct alp_display _pool[CONFIG_ALP_SDK_MAX_DISPLAY_HANDLES];

static struct alp_display *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_DISPLAY_HANDLES; ++i) {
		/* Atomic claim: only the winner of the flag flip may touch the
		 * slot's other fields (in_use is the struct's last member, so
		 * zero everything before it -- incl. lifecycle/active_ops,
		 * parking a fresh slot at LC_UNOPENED). Issue #629. */
		if (alp_slot_try_claim(&_pool[i].in_use)) {
			memset(&_pool[i], 0, offsetof(struct alp_display, in_use));
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_display *h)
{
	alp_slot_release(&h->in_use);
}

alp_display_t *alp_display_open(const alp_display_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("display", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_display_ops_t *ops = (const alp_display_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_display *h = _alloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	h->state.display_id     = cfg->display_id;
	alp_capabilities_t caps = { .flags = be->base_caps };
	alp_status_t       rc   = ops->open(cfg, &h->state, &caps);
	if (rc != ALP_OK) {
		_free(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN); /* #629 */
	return h;
}

alp_status_t alp_display_get_caps(alp_display_t *h, alp_display_caps_t *out)
{
	if (out == NULL) {
		return ALP_ERR_INVAL;
	}
	/* Gate on the lifecycle byte, not a plain in_use read: in_use is
	 * claimed/released atomically in _alloc/_free, so mixing it with a
	 * plain read here is a data race, and a racing close could free the
	 * slot mid-op. op_enter counts this op in; begin_close drains it. #629 */
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc = h->state.ops->get_caps(&h->state, out);
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_display_blit(alp_display_t *h,
                              uint16_t       x,
                              uint16_t       y,
                              uint16_t       w,
                              uint16_t       h_rect,
                              const void    *pixels)
{
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	if (pixels == NULL) {
		alp_handle_op_leave(&h->active_ops);
		return ALP_ERR_INVAL;
	}
	alp_status_t rc = h->state.ops->blit(&h->state, x, y, w, h_rect, pixels);
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_display_clear(alp_display_t *h)
{
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc = h->state.ops->clear(&h->state);
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

void alp_display_close(alp_display_t *h)
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

const alp_capabilities_t *alp_display_capabilities(const alp_display_t *h)
{
	return (h != NULL) ? &h->cached_caps : NULL;
}
