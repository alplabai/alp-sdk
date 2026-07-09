/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr backend for the <alp/rpc.h> surface -- framed
 * RPC over OpenAMP / RPMsg.
 *
 * Builds on Zephyr v4.4's subsys/ipc/ipc_service framework with
 * the rpmsg backend (CONFIG_IPC_SERVICE_BACKEND_RPMSG=y).  The
 * orchestrator's generated <alp/system_ipc.h> + the per-board DT
 * overlay under zephyr/boards/ wire up the carve-out, mailbox
 * channel, and endpoint IDs; this file plumbs the ipc_service
 * callbacks behind the dispatcher's ops vtable.
 *
 * Per-channel backend state lives in a tiny fixed-size pool
 * (CONFIG_ALP_SDK_RPC_MAX_CHANNELS, default 4 -- apps rarely run
 * more than two on a single Zephyr slice).  Each channel carries
 * an N-entry per-method dispatch table (cap
 * CONFIG_ALP_SDK_RPC_SUBS_PER_CHANNEL, default 8) keyed on an
 * FNV-1a hash of the method name; collisions store a linear chain.
 *
 * alp_rpc_call uses a per-channel response slot + a Zephyr
 * k_sem(0,1) for the synchronous wait.  Concurrent calls on the
 * same channel are serialised via the channel's tx_mutex (per the
 * public-API note in alp/rpc.h).
 *
 * Registers as silicon_ref="*" at priority 100 -- mirrors the
 * design spec Section 2 backend matrix (zephyr_drv wins on every
 * SoC unless a more specific backend registers).
 *
 * Gated on CONFIG_ALP_SDK_RPC -- when OFF the I/O ops return
 * NOSUPPORT but the registry entry still links so the dispatcher
 * picks it ahead of sw_fallback on real silicon builds with the
 * ipc_service / rpmsg backend present.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/rpc.h>

#include "rpc_ops.h"

#if defined(CONFIG_ALP_SDK_RPC)

#if !defined(CONFIG_OPENAMP) || !defined(CONFIG_IPC_SERVICE) ||                                    \
    !defined(CONFIG_IPC_SERVICE_BACKEND_RPMSG)
#error "alp_rpc requires CONFIG_ALP_SDK_RPC=y + CONFIG_OPENAMP=y + " \
       "CONFIG_IPC_SERVICE=y + CONFIG_IPC_SERVICE_BACKEND_RPMSG=y"
#endif

#include <zephyr/ipc/ipc_service.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(alp_rpc, CONFIG_LOG_DEFAULT_LEVEL);

#endif /* CONFIG_ALP_SDK_RPC */

#ifndef CONFIG_ALP_SDK_RPC_MAX_CHANNELS
#define CONFIG_ALP_SDK_RPC_MAX_CHANNELS 4
#endif

#ifndef CONFIG_ALP_SDK_RPC_SUBS_PER_CHANNEL
#define CONFIG_ALP_SDK_RPC_SUBS_PER_CHANNEL 8
#endif

#ifndef CONFIG_ALP_SDK_RPC_TX_FRAME_MAX
#define CONFIG_ALP_SDK_RPC_TX_FRAME_MAX 1024
#endif

/* ------------------------------------------------------------------ */
/* Backend-owned per-handle state                                      */
/* ------------------------------------------------------------------ */

struct rpc_sub {
	uint32_t            method_hash; /* FNV-1a(method) */
	char                method[ALP_RPC_METHOD_MAX_LEN];
	alp_rpc_method_cb_t cb;
	void               *user;
};

struct rpc_be {
	bool in_use;

#if defined(CONFIG_ALP_SDK_RPC)
	/* Cached config (name copied locally so the customer's literal
     * doesn't have to outlive open()). */
	char     name[ALP_RPC_METHOD_MAX_LEN];
	uint32_t src_ept;
	uint32_t dst_ept;
	uint32_t mbox_ch;
	bool     cacheable;

	/* Zephyr ipc_service handles. */
	const struct device *ipc_dev;
	struct ipc_ept       ept;
	struct ipc_ept_cfg   ept_cfg;
	bool                 ept_bound;

	/* Subscribe table.  Linear probe on collision; 8 slots is plenty
     * for the v0.6 framing budget. */
	struct rpc_sub subs[CONFIG_ALP_SDK_RPC_SUBS_PER_CHANNEL];

	/* TX serialisation. */
	struct k_mutex tx_mutex;
	uint8_t        tx_scratch[CONFIG_ALP_SDK_RPC_TX_FRAME_MAX];

	/* Synchronous-call slot.  Single-element because the channel
     * serialises calls via tx_mutex anyway. */
	struct k_sem call_sem;
	char         call_method[ALP_RPC_METHOD_MAX_LEN];
	void        *call_resp_buf;
	size_t       call_resp_cap;
	size_t       call_resp_len;
	alp_status_t call_result;
	bool         call_pending;
#endif
};

#if defined(CONFIG_ALP_SDK_RPC)

static struct rpc_be _be_pool[CONFIG_ALP_SDK_RPC_MAX_CHANNELS];

static struct rpc_be *_be_alloc(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(_be_pool); ++i) {
		if (!_be_pool[i].in_use) {
			memset(&_be_pool[i], 0, sizeof(_be_pool[i]));
			_be_pool[i].in_use = true;
			return &_be_pool[i];
		}
	}
	return NULL;
}

static void _be_free(struct rpc_be *be)
{
	if (be == NULL) return;
	be->in_use = false;
}

#endif /* CONFIG_ALP_SDK_RPC */

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Bounded strlen -- C99-portable replacement for POSIX strnlen().
 * Returns the index of the first '\0' or `cap` if none found within
 * the budget. */
static size_t bounded_strlen(const char *s, size_t cap)
{
	for (size_t i = 0; i < cap; ++i) {
		if (s[i] == '\0') {
			return i;
		}
	}
	return cap;
}

static bool method_valid(const char *m)
{
	if (m == NULL || m[0] == '\0') {
		return false;
	}
	return bounded_strlen(m, ALP_RPC_METHOD_MAX_LEN) < ALP_RPC_METHOD_MAX_LEN;
}

#if defined(CONFIG_ALP_SDK_RPC)

static uint32_t fnv1a_32(const char *s)
{
	uint32_t h = 0x811c9dc5u;
	for (; *s; ++s) {
		h ^= (uint8_t)*s;
		h *= 0x01000193u;
	}
	return h;
}

static alp_status_t errno_to_alp(int err)
{
	switch (err) {
	case 0:
		return ALP_OK;
	case -EINVAL:
		return ALP_ERR_INVAL;
	case -EBUSY:
		return ALP_ERR_BUSY;
	case -EAGAIN: /* fallthrough */
	case -ETIMEDOUT:
		return ALP_ERR_TIMEOUT;
	case -EIO:
		return ALP_ERR_IO;
	case -ENOTSUP: /* fallthrough */
	case -ENOSYS:
		return ALP_ERR_NOSUPPORT;
	case -ENOMEM:
		return ALP_ERR_NOMEM;
	default:
		return ALP_ERR_IO;
	}
}

static struct rpc_sub *sub_find(struct rpc_be *be, const char *method, uint32_t hash)
{
	for (size_t i = 0; i < ARRAY_SIZE(be->subs); ++i) {
		struct rpc_sub *s = &be->subs[i];
		if (s->cb != NULL && s->method_hash == hash &&
		    strncmp(s->method, method, ALP_RPC_METHOD_MAX_LEN) == 0) {
			return s;
		}
	}
	return NULL;
}

static struct rpc_sub *sub_alloc(struct rpc_be *be)
{
	for (size_t i = 0; i < ARRAY_SIZE(be->subs); ++i) {
		if (be->subs[i].cb == NULL) {
			return &be->subs[i];
		}
	}
	return NULL;
}

/* Parse the on-wire frame: NUL-terminated method header followed by
 * the opaque payload.  Returns the method-name pointer + payload
 * window; NULL on malformed frames. */
static const char *
frame_parse(const void *data, size_t len, const void **payload_out, size_t *payload_len_out)
{
	if (data == NULL || len == 0) {
		return NULL;
	}
	const char *bytes      = (const char *)data;
	size_t      cap        = len < ALP_RPC_METHOD_MAX_LEN ? len : ALP_RPC_METHOD_MAX_LEN;
	size_t      method_len = 0;
	while (method_len < cap && bytes[method_len] != '\0') {
		method_len++;
	}
	if (method_len == cap) {
		return NULL;
	}
	*payload_out     = (const void *)(bytes + method_len + 1u);
	*payload_len_out = len - method_len - 1u;
	return bytes;
}

static int
frame_build(uint8_t *out, size_t cap, const char *method, const void *payload, size_t payload_len)
{
	size_t method_len = bounded_strlen(method, ALP_RPC_METHOD_MAX_LEN);
	if (method_len == ALP_RPC_METHOD_MAX_LEN) {
		return -EINVAL;
	}
	/* Overflow-safe capacity check (see alp_rpc_frame_size): a near-SIZE_MAX
     * payload_len must not wrap the framed total past `cap`. */
	size_t total;
	if (!alp_rpc_frame_size(method_len, payload_len, cap, &total)) {
		return -ENOMEM;
	}
	memcpy(out, method, method_len);
	out[method_len] = '\0';
	if (payload_len > 0) {
		memcpy(out + method_len + 1u, payload, payload_len);
	}
	return (int)total;
}

/* ------------------------------------------------------------------ */
/* ipc_service callbacks                                               */
/* ------------------------------------------------------------------ */

static void rpc_ept_bound(void *priv)
{
	struct rpc_be *be = (struct rpc_be *)priv;
	if (be != NULL) {
		be->ept_bound = true;
		LOG_DBG("rpc: endpoint bound name=%s", be->name);
	}
}

static void rpc_ept_recv(const void *data, size_t len, void *priv)
{
	struct rpc_be *be = (struct rpc_be *)priv;
	if (be == NULL || !be->in_use) {
		return;
	}

	const void *payload     = NULL;
	size_t      payload_len = 0;
	const char *method      = frame_parse(data, len, &payload, &payload_len);
	if (method == NULL) {
		LOG_WRN("rpc: malformed frame on %s (len=%zu)", be->name, len);
		return;
	}

	/* Synchronous-call path: a pending alp_rpc_call wakes the caller
     * only when the response method matches. */
	if (be->call_pending && strncmp(method, be->call_method, ALP_RPC_METHOD_MAX_LEN) == 0) {
		if (be->call_resp_buf != NULL) {
			if (payload_len > be->call_resp_cap) {
				be->call_result   = ALP_ERR_NOMEM;
				be->call_resp_len = 0;
			} else {
				memcpy(be->call_resp_buf, payload, payload_len);
				be->call_resp_len = payload_len;
				be->call_result   = ALP_OK;
			}
		} else {
			be->call_resp_len = payload_len;
			be->call_result   = ALP_OK;
		}
		be->call_pending = false;
		k_sem_give(&be->call_sem);
		return;
	}

	/* Async dispatch via the per-method subscribe table. */
	uint32_t        h   = fnv1a_32(method);
	struct rpc_sub *sub = sub_find(be, method, h);
	if (sub != NULL && sub->cb != NULL) {
		sub->cb(payload, payload_len, sub->user);
	}
}

#endif /* CONFIG_ALP_SDK_RPC */

/* ================================================================== */
/* Ops                                                                 */
/* ================================================================== */

static alp_status_t
z_open(const alp_rpc_config_t *cfg, alp_rpc_backend_state_t *st, alp_capabilities_t *caps_out)
{
	caps_out->flags = 0u;
	if (cfg == NULL || cfg->name == NULL || cfg->name[0] == '\0') {
		return ALP_ERR_INVAL;
	}
#if defined(CONFIG_ALP_SDK_RPC)
	if (bounded_strlen(cfg->name, ALP_RPC_METHOD_MAX_LEN) == ALP_RPC_METHOD_MAX_LEN) {
		return ALP_ERR_INVAL;
	}

	struct rpc_be *be = _be_alloc();
	if (be == NULL) {
		return ALP_ERR_NOMEM;
	}

	strncpy(be->name, cfg->name, sizeof(be->name) - 1);
	be->src_ept   = cfg->src_ept != 0u ? cfg->src_ept : (0x400u | (fnv1a_32(cfg->name) & 0x0FFu));
	be->dst_ept   = cfg->dst_ept != 0u ? cfg->dst_ept : be->src_ept + 1u;
	be->mbox_ch   = cfg->mbox_ch != 0u ? cfg->mbox_ch : ALP_RPC_DEFAULT_MBOX_CH;
	be->cacheable = cfg->cacheable;

	k_mutex_init(&be->tx_mutex);
	k_sem_init(&be->call_sem, 0, 1);

	/* The Zephyr DT overlay's chosen { zephyr,ipc = ... } picks the
     * default ipc backend.  When the chosen alias isn't set we surface
     * a clean NOT_READY so the customer learns to fix their overlay. */
#if DT_HAS_CHOSEN(zephyr_ipc)
	be->ipc_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_ipc));
#else
	be->ipc_dev = NULL;
#endif
	if (be->ipc_dev == NULL || !device_is_ready(be->ipc_dev)) {
		LOG_ERR("rpc: zephyr,ipc chosen node missing or not ready");
		_be_free(be);
		return ALP_ERR_NOT_READY;
	}

	int rc = ipc_service_open_instance(be->ipc_dev);
	if (rc != 0 && rc != -EALREADY) {
		LOG_ERR("rpc: ipc_service_open_instance failed: %d", rc);
		_be_free(be);
		return errno_to_alp(rc);
	}

	be->ept_cfg.name        = be->name;
	be->ept_cfg.cb.bound    = rpc_ept_bound;
	be->ept_cfg.cb.received = rpc_ept_recv;
	be->ept_cfg.priv        = be;

	rc = ipc_service_register_endpoint(be->ipc_dev, &be->ept, &be->ept_cfg);
	if (rc < 0) {
		LOG_ERR("rpc: ipc_service_register_endpoint(%s) failed: %d", be->name, rc);
		_be_free(be);
		return errno_to_alp(rc);
	}

	LOG_INF("rpc: opened %s src=0x%x dst=0x%x mbox=%u",
	        be->name,
	        be->src_ept,
	        be->dst_ept,
	        be->mbox_ch);
	st->be_data = be;
	return ALP_OK;
#else /* !CONFIG_ALP_SDK_RPC */
	(void)st;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t
z_subscribe(alp_rpc_backend_state_t *st, const char *method, alp_rpc_method_cb_t cb, void *user)
{
	if (!method_valid(method)) {
		return ALP_ERR_INVAL;
	}
#if defined(CONFIG_ALP_SDK_RPC)
	struct rpc_be *be = (struct rpc_be *)st->be_data;
	if (be == NULL || !be->in_use) {
		return ALP_ERR_NOT_READY;
	}
	/* NULL cb == unsubscribe -- matches the documented behaviour. */
	if (cb == NULL) {
		uint32_t        h   = fnv1a_32(method);
		struct rpc_sub *sub = sub_find(be, method, h);
		if (sub == NULL) {
			return ALP_ERR_INVAL;
		}
		sub->cb          = NULL;
		sub->user        = NULL;
		sub->method[0]   = '\0';
		sub->method_hash = 0u;
		return ALP_OK;
	}
	uint32_t h = fnv1a_32(method);

	/* Replace if already present. */
	struct rpc_sub *sub = sub_find(be, method, h);
	if (sub == NULL) {
		sub = sub_alloc(be);
		if (sub == NULL) {
			LOG_WRN("rpc: subscribe table full on %s (cap=%d)",
			        be->name,
			        CONFIG_ALP_SDK_RPC_SUBS_PER_CHANNEL);
			return ALP_ERR_NOMEM;
		}
		sub->method_hash = h;
		strncpy(sub->method, method, sizeof(sub->method) - 1);
		sub->method[sizeof(sub->method) - 1] = '\0';
	}
	sub->cb   = cb;
	sub->user = user;
	return ALP_OK;
#else
	(void)st;
	(void)cb;
	(void)user;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_unsubscribe(alp_rpc_backend_state_t *st, const char *method)
{
	if (!method_valid(method)) {
		return ALP_ERR_INVAL;
	}
#if defined(CONFIG_ALP_SDK_RPC)
	struct rpc_be *be = (struct rpc_be *)st->be_data;
	if (be == NULL || !be->in_use) {
		return ALP_ERR_NOT_READY;
	}
	uint32_t        h   = fnv1a_32(method);
	struct rpc_sub *sub = sub_find(be, method, h);
	if (sub == NULL) {
		return ALP_ERR_INVAL;
	}
	sub->cb          = NULL;
	sub->user        = NULL;
	sub->method[0]   = '\0';
	sub->method_hash = 0u;
	return ALP_OK;
#else
	(void)st;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t
z_send(alp_rpc_backend_state_t *st, const char *method, const void *payload, size_t len)
{
	if (!method_valid(method)) {
		return ALP_ERR_INVAL;
	}
	if (payload == NULL && len > 0) {
		return ALP_ERR_INVAL;
	}
#if defined(CONFIG_ALP_SDK_RPC)
	struct rpc_be *be = (struct rpc_be *)st->be_data;
	if (be == NULL || !be->in_use) {
		return ALP_ERR_NOT_READY;
	}
	k_mutex_lock(&be->tx_mutex, K_FOREVER);
	int          built = frame_build(be->tx_scratch, sizeof(be->tx_scratch), method, payload, len);
	alp_status_t s;
	if (built < 0) {
		s = errno_to_alp(built);
	} else {
		int rc = ipc_service_send(&be->ept, be->tx_scratch, (size_t)built);
		s      = rc >= 0 ? ALP_OK : errno_to_alp(rc);
	}
	k_mutex_unlock(&be->tx_mutex);
	return s;
#else
	(void)st;
	(void)payload;
	(void)len;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_call(alp_rpc_backend_state_t *st,
                           const char              *method,
                           const void              *req,
                           size_t                   req_len,
                           void                    *resp,
                           size_t                  *resp_len,
                           uint32_t                 timeout_ms)
{
	if (!method_valid(method)) {
		return ALP_ERR_INVAL;
	}
	if (resp != NULL && resp_len == NULL) {
		return ALP_ERR_INVAL;
	}
#if defined(CONFIG_ALP_SDK_RPC)
	struct rpc_be *be = (struct rpc_be *)st->be_data;
	if (be == NULL || !be->in_use) {
		return ALP_ERR_NOT_READY;
	}
	/* Serialise calls on this channel; the per-channel call slot is
     * single-element by design (see the public-API note in rpc.h). */
	k_mutex_lock(&be->tx_mutex, K_FOREVER);

	/* Stage the call slot. */
	strncpy(be->call_method, method, sizeof(be->call_method) - 1);
	be->call_method[sizeof(be->call_method) - 1] = '\0';
	be->call_resp_buf                            = resp;
	be->call_resp_cap = (resp != NULL && resp_len != NULL) ? *resp_len : 0u;
	be->call_resp_len = 0u;
	be->call_result   = ALP_ERR_TIMEOUT;
	be->call_pending  = true;
	k_sem_reset(&be->call_sem);

	/* Frame + send. */
	int          built = frame_build(be->tx_scratch, sizeof(be->tx_scratch), method, req, req_len);
	alp_status_t s     = ALP_OK;
	if (built < 0) {
		s = errno_to_alp(built);
	} else {
		int rc = ipc_service_send(&be->ept, be->tx_scratch, (size_t)built);
		if (rc < 0) {
			s = errno_to_alp(rc);
		}
	}

	if (s != ALP_OK) {
		be->call_pending = false;
		k_mutex_unlock(&be->tx_mutex);
		return s;
	}

	/* Wait for the response (or timeout). */
	k_timeout_t to = (timeout_ms == UINT32_MAX) ? K_FOREVER : K_MSEC(timeout_ms);
	int         rc = k_sem_take(&be->call_sem, to);
	if (rc == -EAGAIN) {
		be->call_pending = false;
		s                = ALP_ERR_TIMEOUT;
	} else {
		s = be->call_result;
		if (s == ALP_OK && resp_len != NULL) {
			*resp_len = be->call_resp_len;
		}
	}

	k_mutex_unlock(&be->tx_mutex);
	return s;
#else
	(void)st;
	(void)req;
	(void)req_len;
	(void)resp;
	(void)resp_len;
	(void)timeout_ms;
	return ALP_ERR_NOSUPPORT;
#endif
}

static void z_close(alp_rpc_backend_state_t *st)
{
#if defined(CONFIG_ALP_SDK_RPC)
	struct rpc_be *be = (struct rpc_be *)st->be_data;
	if (be == NULL || !be->in_use) {
		return;
	}
	/* Wake any pending alp_rpc_call so the caller unblocks. */
	if (be->call_pending) {
		be->call_result  = ALP_ERR_NOT_READY;
		be->call_pending = false;
		k_sem_give(&be->call_sem);
	}
	(void)ipc_service_deregister_endpoint(&be->ept);
	be->ept_bound = false;
	_be_free(be);
	st->be_data = NULL;
#else
	(void)st;
#endif
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const alp_rpc_ops_t _ops = {
	.open        = z_open,
	.subscribe   = z_subscribe,
	.unsubscribe = z_unsubscribe,
	.send        = z_send,
	.call        = z_call,
	.close       = z_close,
};

ALP_BACKEND_REGISTER(rpc,
                     zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
