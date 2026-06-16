/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Counter class dispatcher.  Routes the public alp_counter_* API
 * through the .alp_backends_counter registry.
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

#include "backends/counter/counter_ops.h"

ALP_BACKEND_DEFINE_CLASS(counter);

extern void alp_z_set_last_error(alp_status_t s);
extern void alp_z_clear_last_error(void);

#ifndef CONFIG_ALP_SDK_MAX_COUNTER_HANDLES
#define CONFIG_ALP_SDK_MAX_COUNTER_HANDLES 4
#endif

static struct alp_counter _pool[CONFIG_ALP_SDK_MAX_COUNTER_HANDLES];

static struct alp_counter *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_COUNTER_HANDLES; ++i) {
		if (!_pool[i].in_use) {
			memset(&_pool[i], 0, sizeof(_pool[i]));
			_pool[i].in_use = true;
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_counter *h)
{
	h->in_use = false;
}

alp_counter_t *alp_counter_open(const alp_counter_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("counter", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_counter_ops_t *ops = (const alp_counter_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_counter *h = _alloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	alp_capabilities_t caps = { .flags = be->base_caps };
	if (be->probe != NULL) {
		uint32_t refined = caps.flags;
		(void)be->probe(cfg->counter_id, &refined);
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

alp_status_t alp_counter_start(alp_counter_t *h)
{
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	return h->state.ops->start(&h->state);
}

alp_status_t alp_counter_stop(alp_counter_t *h)
{
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	return h->state.ops->stop(&h->state);
}

alp_status_t alp_counter_get_value(alp_counter_t *h, uint32_t *ticks_out)
{
	if (ticks_out == NULL) return ALP_ERR_INVAL;
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	return h->state.ops->get_value(&h->state, ticks_out);
}

alp_status_t alp_counter_us_to_ticks(alp_counter_t *h, uint32_t us, uint32_t *ticks_out)
{
	if (ticks_out == NULL) return ALP_ERR_INVAL;
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	return h->state.ops->us_to_ticks(&h->state, us, ticks_out);
}

alp_status_t alp_counter_set_alarm(alp_counter_t         *h,
                                   uint32_t               ticks_from_now,
                                   alp_counter_alarm_cb_t cb,
                                   void                  *user)
{
	if (cb == NULL) return ALP_ERR_INVAL;
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	h->state.alarm_cb   = cb;
	h->state.alarm_user = user;
	return h->state.ops->set_alarm(&h->state, ticks_from_now, h);
}

alp_status_t alp_counter_cancel_alarm(alp_counter_t *h)
{
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	alp_status_t rc     = h->state.ops->cancel_alarm(&h->state);
	h->state.alarm_cb   = NULL;
	h->state.alarm_user = NULL;
	return rc;
}

void alp_counter_close(alp_counter_t *h)
{
	if (h == NULL || !h->in_use) return;
	if (h->state.ops != NULL && h->state.ops->close != NULL) {
		h->state.ops->close(&h->state);
	}
	_free(h);
}

const alp_capabilities_t *alp_counter_capabilities(const alp_counter_t *h)
{
	return (h != NULL) ? &h->cached_caps : NULL;
}
