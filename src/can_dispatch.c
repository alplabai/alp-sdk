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

#include "backends/can/can_ops.h"

ALP_BACKEND_DEFINE_CLASS(can);

extern void alp_z_set_last_error(alp_status_t s);
extern void alp_z_clear_last_error(void);

#ifndef CONFIG_ALP_SDK_MAX_CAN_HANDLES
#define CONFIG_ALP_SDK_MAX_CAN_HANDLES 4
#endif

static struct alp_can _pool[CONFIG_ALP_SDK_MAX_CAN_HANDLES];

static struct alp_can *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_CAN_HANDLES; ++i) {
		if (!_pool[i].in_use) {
			memset(&_pool[i], 0, sizeof(_pool[i]));
			_pool[i].in_use = true;
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_can *h)
{
	h->in_use = false;
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
	return h;
}

alp_status_t alp_can_start(alp_can_t *can)
{
	if (can == NULL || !can->in_use) return ALP_ERR_NOT_READY;
	if (can->state.ops->start == NULL) return ALP_ERR_NOSUPPORT;
	alp_status_t rc = can->state.ops->start(&can->state);
	if (rc == ALP_OK) can->started = true;
	return rc;
}

alp_status_t alp_can_stop(alp_can_t *can)
{
	if (can == NULL || !can->in_use) return ALP_ERR_NOT_READY;
	if (can->state.ops->stop == NULL) return ALP_ERR_NOSUPPORT;
	alp_status_t rc = can->state.ops->stop(&can->state);
	if (rc == ALP_OK) can->started = false;
	return rc;
}

alp_status_t alp_can_send(alp_can_t *can, const alp_can_frame_t *frame, uint32_t timeout_ms)
{
	if (can == NULL || !can->in_use) return ALP_ERR_NOT_READY;
	if (frame == NULL) return ALP_ERR_INVAL;
	if (frame->dlc > ALP_CAN_MAX_DLC_FD) return ALP_ERR_INVAL;
	if (!frame->fd && frame->dlc > ALP_CAN_MAX_DLC_CLASSIC) return ALP_ERR_INVAL;
	if (can->state.ops->send == NULL) return ALP_ERR_NOSUPPORT;
	return can->state.ops->send(&can->state, frame, timeout_ms);
}

alp_status_t alp_can_add_filter(alp_can_t              *can,
                                const alp_can_filter_t *filter,
                                alp_can_rx_cb_t         cb,
                                void                   *user,
                                int32_t                *filter_id_out)
{
	if (can == NULL || !can->in_use) return ALP_ERR_NOT_READY;
	if (filter == NULL || cb == NULL) return ALP_ERR_INVAL;
	if (can->state.ops->add_filter == NULL) return ALP_ERR_NOSUPPORT;
	return can->state.ops->add_filter(&can->state, filter, cb, user, filter_id_out);
}

alp_status_t alp_can_remove_filter(alp_can_t *can, int32_t filter_id)
{
	if (can == NULL || !can->in_use) return ALP_ERR_NOT_READY;
	if (can->state.ops->remove_filter == NULL) return ALP_ERR_NOSUPPORT;
	return can->state.ops->remove_filter(&can->state, filter_id);
}

void alp_can_close(alp_can_t *can)
{
	if (can == NULL || !can->in_use) return;
	if (can->state.ops != NULL && can->state.ops->close != NULL) {
		can->state.ops->close(&can->state);
	}
	_free(can);
}

const alp_capabilities_t *alp_can_capabilities(const alp_can_t *can)
{
	return (can != NULL) ? &can->cached_caps : NULL;
}
