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
		if (!_pool[i].in_use) {
			memset(&_pool[i], 0, sizeof(_pool[i]));
			_pool[i].in_use = true;
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_qenc *h)
{
	h->in_use = false;
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
	const alp_qenc_ops_t *ops = (const alp_qenc_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_qenc *h = _alloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	alp_capabilities_t caps = { .flags = be->base_caps };
	if (be->probe != NULL) {
		uint32_t refined = caps.flags;
		(void)be->probe(cfg->encoder_id, &refined);
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

alp_status_t alp_qenc_get_position(alp_qenc_t *h, int32_t *pos_out)
{
	if (pos_out == NULL) return ALP_ERR_INVAL;
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	return h->state.ops->get_position(&h->state, pos_out);
}

alp_status_t alp_qenc_reset_position(alp_qenc_t *h)
{
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	return h->state.ops->reset_position(&h->state);
}

void alp_qenc_close(alp_qenc_t *h)
{
	if (h == NULL || !h->in_use) return;
	if (h->state.ops != NULL && h->state.ops->close != NULL) {
		h->state.ops->close(&h->state);
	}
	_free(h);
}

const alp_capabilities_t *alp_qenc_capabilities(const alp_qenc_t *h)
{
	return (h != NULL) ? &h->cached_caps : NULL;
}
