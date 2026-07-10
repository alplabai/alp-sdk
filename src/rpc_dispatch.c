/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RPC class dispatcher.  Owns the public <alp/rpc.h> surface
 * (framed RPC over OpenAMP / RPMsg) on top of the backend registry
 * mechanism shipped in Slice 0 (PR #17).
 *
 * Per design spec Section 4: ONE class registry covers the single
 * alp_rpc_channel_t surface.  The dispatcher copies the customer's
 * alp_rpc_config_t into the handle's backend state before
 * delegating to the backend's open(); every subsequent op walks
 * state->ops.  Subscription tables and the per-channel sync-call
 * slot live entirely behind state->be_data inside the backend.
 *
 * Slice 4c ships no vendor extensions for RPC: the Zephyr
 * ipc_service-backed backend covers every E1M-conformant SoM with
 * an OpenAMP / RPMsg-capable Zephyr build; the SW fallback covers
 * native_sim and trimmed images.  No second registry tier is
 * needed.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/rpc.h>
#include <alp/soc_caps.h>

#include "backends/rpc/rpc_ops.h"

ALP_BACKEND_DEFINE_CLASS(rpc);
/* Pull the rpc registry section into a static-archive link (#368). */
ALP_BACKEND_ANCHOR(rpc);

#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_MAX_RPC_HANDLES
#define CONFIG_ALP_SDK_MAX_RPC_HANDLES 2
#endif

static struct alp_rpc_channel _rpc_pool[CONFIG_ALP_SDK_MAX_RPC_HANDLES];

static struct alp_rpc_channel *_alloc_rpc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_RPC_HANDLES; ++i) {
		if (!_rpc_pool[i].in_use) {
			memset(&_rpc_pool[i], 0, sizeof(_rpc_pool[i]));
			_rpc_pool[i].in_use = true;
			return &_rpc_pool[i];
		}
	}
	return NULL;
}

static void _free_rpc(struct alp_rpc_channel *h)
{
	h->in_use = false;
}

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

alp_rpc_channel_t *alp_rpc_open(const alp_rpc_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL || cfg->name == NULL || cfg->name[0] == '\0') {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("rpc", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_rpc_ops_t *ops = (const alp_rpc_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_rpc_channel *h = _alloc_rpc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	h->state.cfg            = *cfg;
	alp_capabilities_t caps = { .flags = be->base_caps };
	alp_status_t       rc   = ops->open(cfg, &h->state, &caps);
	if (rc != ALP_OK) {
		_free_rpc(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	return h;
}

void alp_rpc_close(alp_rpc_channel_t *ch)
{
	if (ch == NULL || !ch->in_use) return;
	if (ch->state.ops != NULL && ch->state.ops->close != NULL) {
		ch->state.ops->close(&ch->state);
	}
	_free_rpc(ch);
}

/* ================================================================== */
/* Subscriptions                                                       */
/* ================================================================== */

alp_status_t
alp_rpc_subscribe(alp_rpc_channel_t *ch, const char *method, alp_rpc_method_cb_t cb, void *user)
{
	if (ch == NULL || !ch->in_use) return ALP_ERR_NOT_READY;
	if (method == NULL || method[0] == '\0') return ALP_ERR_INVAL;
	if (ch->state.ops == NULL || ch->state.ops->subscribe == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return ch->state.ops->subscribe(&ch->state, method, cb, user);
}

alp_status_t alp_rpc_unsubscribe(alp_rpc_channel_t *ch, const char *method)
{
	if (ch == NULL || !ch->in_use) return ALP_ERR_NOT_READY;
	if (method == NULL || method[0] == '\0') return ALP_ERR_INVAL;
	if (ch->state.ops == NULL || ch->state.ops->unsubscribe == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return ch->state.ops->unsubscribe(&ch->state, method);
}

/* ================================================================== */
/* Send + call                                                         */
/* ================================================================== */

alp_status_t
alp_rpc_send(alp_rpc_channel_t *ch, const char *method, const void *payload, size_t len)
{
	if (ch == NULL || !ch->in_use) return ALP_ERR_NOT_READY;
	if (method == NULL || method[0] == '\0') return ALP_ERR_INVAL;
	if (payload == NULL && len > 0) return ALP_ERR_INVAL;
	if (ch->state.ops == NULL || ch->state.ops->send == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return ch->state.ops->send(&ch->state, method, payload, len);
}

alp_status_t alp_rpc_call(alp_rpc_channel_t *ch,
                          const char        *method,
                          const void        *req,
                          size_t             req_len,
                          void              *resp,
                          size_t            *resp_len,
                          uint32_t           timeout_ms)
{
	if (ch == NULL || !ch->in_use) return ALP_ERR_NOT_READY;
	if (method == NULL || method[0] == '\0') return ALP_ERR_INVAL;
	if (req == NULL && req_len > 0) return ALP_ERR_INVAL;
	if (resp != NULL && resp_len == NULL) return ALP_ERR_INVAL;
	if (ch->state.ops == NULL || ch->state.ops->call == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return ch->state.ops->call(&ch->state, method, req, req_len, resp, resp_len, timeout_ms);
}

/* ================================================================== */
/* Capability getter                                                   */
/* ================================================================== */

const alp_capabilities_t *alp_rpc_capabilities(const alp_rpc_channel_t *ch)
{
	return (ch != NULL) ? &ch->cached_caps : NULL;
}
