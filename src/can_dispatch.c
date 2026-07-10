/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN class dispatcher.  Routes the public alp_can_* API
 * through the .alp_backends_can registry.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/can.h>
#include <alp/soc_caps.h>

#include "alp_slot_claim.h"
#include "backends/can/can_ops.h"

ALP_BACKEND_DEFINE_CLASS(can);
/* Pull the can registry section into a static-archive link (#368). */
ALP_BACKEND_ANCHOR(can);

#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_MAX_CAN_HANDLES
#define CONFIG_ALP_SDK_MAX_CAN_HANDLES 4
#endif

static struct alp_can _pool[CONFIG_ALP_SDK_MAX_CAN_HANDLES];

static struct alp_can *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_CAN_HANDLES; ++i) {
		/* Atomic claim (issue #629): only the winner of the flag flip
		 * may touch the slot's other fields -- in_use is the
		 * struct's last member, so zero everything before it. */
		if (alp_slot_try_claim(&_pool[i].in_use)) {
			memset(&_pool[i], 0, offsetof(struct alp_can, in_use));
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_can *h)
{
	alp_slot_release(&h->in_use);
}

alp_can_t *alp_can_open(const alp_can_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL || cfg->bitrate_nominal_hz == 0u) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("can", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_can_ops_t *ops = (const alp_can_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_can *h = _alloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	h->cfg                  = *cfg;
	alp_capabilities_t caps = { .flags = be->base_caps };
	if (be->probe != NULL) {
		uint32_t refined = caps.flags;
		(void)be->probe(cfg->bus_id, &refined);
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

/* Every op below gates on the lifecycle byte via alp_handle_op_enter(),
 * not in_use -- in_use is now touched only by the atomic claim/release
 * in _alloc/_free (issue #629: mixing an atomic in_use with a plain
 * read elsewhere is still a data race).  A racing alp_can_close()
 * cannot free the slot until every entered op has left. */

alp_status_t alp_can_start(alp_can_t *can)
{
	if (can == NULL || !alp_handle_op_enter(&can->lifecycle, &can->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (can->state.ops->start == NULL) {
		rc = ALP_ERR_NOSUPPORT;
	} else {
		rc = can->state.ops->start(&can->state);
		if (rc == ALP_OK) can->started = true;
	}
	alp_handle_op_leave(&can->active_ops);
	return rc;
}

alp_status_t alp_can_stop(alp_can_t *can)
{
	if (can == NULL || !alp_handle_op_enter(&can->lifecycle, &can->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (can->state.ops->stop == NULL) {
		rc = ALP_ERR_NOSUPPORT;
	} else {
		rc = can->state.ops->stop(&can->state);
		if (rc == ALP_OK) can->started = false;
	}
	alp_handle_op_leave(&can->active_ops);
	return rc;
}

alp_status_t alp_can_send(alp_can_t *can, const alp_can_frame_t *frame, uint32_t timeout_ms)
{
	if (can == NULL || !alp_handle_op_enter(&can->lifecycle, &can->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (!can->started) {
		rc = ALP_ERR_NOT_READY;
	} else if (frame == NULL) {
		rc = ALP_ERR_INVAL;
	} else if (frame->payload_len > ALP_CAN_MAX_PAYLOAD_BYTES_FD) {
		rc = ALP_ERR_INVAL;
	} else if (!frame->fd && frame->payload_len > ALP_CAN_MAX_PAYLOAD_BYTES_CLASSIC) {
		rc = ALP_ERR_INVAL;
	} else if (can->state.ops->send == NULL) {
		rc = ALP_ERR_NOSUPPORT;
	} else {
		rc = can->state.ops->send(&can->state, frame, timeout_ms);
	}
	alp_handle_op_leave(&can->active_ops);
	return rc;
}

alp_status_t alp_can_add_filter(alp_can_t              *can,
                                const alp_can_filter_t *filter,
                                alp_can_rx_cb_t         cb,
                                void                   *user,
                                int32_t                *filter_id_out)
{
	if (can == NULL || !alp_handle_op_enter(&can->lifecycle, &can->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (filter == NULL || cb == NULL) {
		rc = ALP_ERR_INVAL;
	} else if (can->state.ops->add_filter == NULL) {
		rc = ALP_ERR_NOSUPPORT;
	} else {
		rc = can->state.ops->add_filter(&can->state, filter, cb, user, filter_id_out);
	}
	alp_handle_op_leave(&can->active_ops);
	return rc;
}

alp_status_t alp_can_remove_filter(alp_can_t *can, int32_t filter_id)
{
	if (can == NULL || !alp_handle_op_enter(&can->lifecycle, &can->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc = (can->state.ops->remove_filter == NULL)
	                      ? ALP_ERR_NOSUPPORT
	                      : can->state.ops->remove_filter(&can->state, filter_id);
	alp_handle_op_leave(&can->active_ops);
	return rc;
}

void alp_can_close(alp_can_t *can)
{
	if (can == NULL) return;
	/* Gate out new ops and drain any in-flight one before touching
	 * state.ops (issue #629). Losing the CAS (already closed/closing/
	 * never-opened) makes this a no-op, matching the existing
	 * void-close idempotency contract. */
	if (!alp_handle_begin_close(&can->lifecycle, &can->active_ops)) return;
	if (can->state.ops != NULL && can->state.ops->close != NULL) {
		can->state.ops->close(&can->state);
	}
	alp_lifecycle_set(&can->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free(can);
}

const alp_capabilities_t *alp_can_capabilities(const alp_can_t *can)
{
	return (can != NULL) ? &can->cached_caps : NULL;
}
