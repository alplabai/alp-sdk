/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RTC class dispatcher.  Owns the public alp_rtc_* API surface
 * and routes through the backend registry mechanism shipped in
 * Slice 0 (PR #17).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/rtc.h>
#include <alp/soc_caps.h>

#include "backends/rtc/rtc_ops.h"

ALP_BACKEND_DEFINE_CLASS(rtc);
/* Pull the rtc registry section into a static-archive link (#368). */
ALP_BACKEND_ANCHOR(rtc);

#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_MAX_RTC_HANDLES
#define CONFIG_ALP_SDK_MAX_RTC_HANDLES 2
#endif

static struct alp_rtc _pool[CONFIG_ALP_SDK_MAX_RTC_HANDLES];

static struct alp_rtc *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_RTC_HANDLES; ++i) {
		if (!_pool[i].in_use) {
			memset(&_pool[i], 0, sizeof(_pool[i]));
			_pool[i].in_use = true;
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_rtc *h)
{
	h->in_use = false;
}

alp_rtc_t *alp_rtc_open(uint32_t rtc_id)
{
	alp_z_clear_last_error();
	const alp_backend_t *be = alp_backend_select("rtc", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_rtc_ops_t *ops = (const alp_rtc_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_rtc *h = _alloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	alp_capabilities_t caps = { .flags = be->base_caps };
	if (be->probe != NULL) {
		uint32_t refined = caps.flags;
		(void)be->probe(rtc_id, &refined);
		caps.flags = refined;
	}
	alp_status_t rc = ops->open(rtc_id, &h->state, &caps);
	if (rc != ALP_OK) {
		_free(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	return h;
}

alp_status_t alp_rtc_set_time(alp_rtc_t *h, const alp_rtc_time_t *t)
{
	if (t == NULL) return ALP_ERR_INVAL;
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	return h->state.ops->set_time(&h->state, t);
}

alp_status_t alp_rtc_get_time(alp_rtc_t *h, alp_rtc_time_t *t)
{
	if (t == NULL) return ALP_ERR_INVAL;
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	return h->state.ops->get_time(&h->state, t);
}

void alp_rtc_close(alp_rtc_t *h)
{
	if (h == NULL || !h->in_use) return;
	if (h->state.ops != NULL && h->state.ops->close != NULL) {
		h->state.ops->close(&h->state);
	}
	_free(h);
}

const alp_capabilities_t *alp_rtc_capabilities(const alp_rtc_t *h)
{
	return (h != NULL) ? &h->cached_caps : NULL;
}
