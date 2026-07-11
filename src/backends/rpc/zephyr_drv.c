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
 *
 * @par Close protocol (GHSA-xhm8-7f87-93q5) -- authoritative redesign
 * Mirrors src/backends/rpc/yocto_drv.c's half of the contract (see
 * that file's header comment for the full two-round history): the
 * dispatcher (src/rpc_dispatch.c) owns single-owner election and the
 * active-op drain; this backend's z_shutdown()/z_destroy() pair
 * (replacing the old single z_close()) implement:
 *
 *   - z_shutdown(): a sticky `closing` flag guarded by a k_spinlock
 *     (ipc_service's `received` callback may run close to interrupt
 *     priority, so this can't be a blocking mutex) that z_call()
 *     rechecks both before staging and is woken from, replacing a
 *     one-shot cancel.  Self-close detection compares the CURRENT
 *     thread against the thread recorded the last time
 *     rpc_ept_recv() ran (BENCH-UNVERIFIED assumption: exactly one
 *     ipc_service worker thread ever invokes a given endpoint's
 *     `received` callback, never concurrently, never from ISR context
 *     -- see rpc_ept_recv()'s own comment).  External close
 *     deregisters the endpoint and waits for `cb_active` (a counter
 *     around rpc_ept_recv()'s body) to reach 0 before returning DONE,
 *     because ipc_service_deregister_endpoint()'s return does not, by
 *     itself, clearly guarantee no recv is still in flight (also
 *     BENCH-UNVERIFIED).
 *   - z_destroy(): trivial for this backend (a static pool slot, no
 *     heap, no fds) -- just releases the pool slot.  Called exactly
 *     once by the dispatcher, strictly after its own active-op drain.
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

#include "alp_slot_claim.h"
#include "rpc_ops.h"

#if defined(CONFIG_ALP_SDK_RPC)

#if !defined(CONFIG_OPENAMP) || !defined(CONFIG_IPC_SERVICE) || \
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
     * serialises calls via tx_mutex anyway.
     *
     * `closing` (GHSA-xhm8-7f87-93q5 redesign) is the sticky cancel
     * predicate: guarded by `lock` below (a spinlock, not a mutex --
     * rpc_ept_recv()/z_shutdown() may run close to interrupt priority
     * under ipc_service), set exactly once by z_shutdown() and never
     * cleared.  z_call() rechecks it both before staging and after
     * waking, replacing a one-shot cancel -- see z_call()'s doc
     * comment for why that matters (the late-staging gap). */
	struct k_sem call_sem;
	char         call_method[ALP_RPC_METHOD_MAX_LEN];
	void        *call_resp_buf;
	size_t       call_resp_cap;
	size_t       call_resp_len;
	alp_status_t call_result;
	bool         call_pending;

	/* GHSA-xhm8-7f87-93q5 redesign: guards `closing` + `recv_thread`
     * below. */
	struct k_spinlock lock;
	bool              closing;

	/* Last thread observed running rpc_ept_recv() (see that function).
     * z_shutdown() compares this against the CURRENT thread to detect
     * a self-close (a subscriber callback closing its own channel).
     * BENCH-UNVERIFIED assumption (see this file's header comment):
     * exactly one ipc_service worker thread ever invokes a given
     * endpoint's `received` callback. */
	k_tid_t recv_thread;

	/* Reentrancy counter around rpc_ept_recv()'s body.  An external
     * z_shutdown() deregisters the endpoint then waits for this to
     * reach 0 before returning DONE -- BENCH-UNVERIFIED: ipc_service's
     * deregister does not, by itself, clearly guarantee no recv is
     * still in flight. */
	atomic_t cb_active;

	/* Set once by z_shutdown() on the self-close (DEFERRED) path;
     * read once by rpc_ept_recv()'s epilogue -- see that function. */
	bool close_from_worker;

	/* Dispatcher-owned back-pointer (alp_rpc_backend_state_t::owner,
     * cached at z_open() time) -- see rpc_ops.h.  Used ONLY by
     * rpc_ept_recv()'s epilogue to call alp_rpc_close_finalize(owner)
     * exactly once on the self-close (DEFERRED) path. */
	void *owner;
#endif

	/* Pool-slot claim flag (GHSA-xhm8-7f87-93q5 / issue #629 pattern):
     * stays LAST so a fresh claim's memset(..., offsetof(..., in_use))
     * zeroes every field above without clobbering the flag the claim
     * itself just flipped -- see _be_alloc() below. */
	bool in_use;
};

#if defined(CONFIG_ALP_SDK_RPC)

static struct rpc_be _be_pool[CONFIG_ALP_SDK_RPC_MAX_CHANNELS];

static struct rpc_be *_be_alloc(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(_be_pool); ++i) {
		/* Atomic claim (GHSA-xhm8-7f87-93q5 / issue #629 pattern,
         * matching the dispatcher pool's own _alloc_rpc() and every
         * other backend using this idiom): CAS FREE -> OPEN on
         * `in_use` so two concurrent z_open() calls can never both
         * win the same slot -- replacing the pre-redesign non-atomic
         * `if (!_be_pool[i].in_use) { ...; in_use = true; }`
         * check-then-set. */
		if (alp_slot_try_claim(&_be_pool[i].in_use)) {
			memset(&_be_pool[i], 0, offsetof(struct rpc_be, in_use));
			return &_be_pool[i];
		}
	}
	return NULL;
}

static void _be_free(struct rpc_be *be)
{
	if (be == NULL) return;
	alp_slot_release(&be->in_use);
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

/**
 * @brief ipc_service `received` callback: parse + dispatch one frame.
 *
 * @par Close protocol (GHSA-xhm8-7f87-93q5)
 * Records the current thread (`recv_thread`, under `lock`) so
 * z_shutdown() can detect a self-close, and holds `cb_active` for the
 * entire body -- external z_shutdown() waits for this to reach 0
 * after deregistering the endpoint, guaranteeing no recv is still
 * touching `be` once it returns DONE (see this file's header
 * comment).
 *
 * The subscriber callback invoked below (see @ref alp_rpc_method_cb_t)
 * MAY call @ref alp_rpc_close on its OWN channel -- z_shutdown()
 * detects that (this same thread, mid-callback) and returns
 * ALP_RPC_SHUTDOWN_DEFERRED without deregistering/draining itself;
 * the epilogue at the bottom of this function completes that deferred
 * teardown, exactly once, right here, once the callback (if any) has
 * returned -- mirroring src/backends/rpc/yocto_drv.c's rpc_rx_main()
 * epilogue.  BENCH-UNVERIFIED: this assumes ipc_service never invokes
 * `received` from ISR context and never re-enters it concurrently
 * from a second thread for the same endpoint -- see this file's header
 * comment.
 */
static void rpc_ept_recv(const void *data, size_t len, void *priv)
{
	struct rpc_be *be = (struct rpc_be *)priv;
	if (be == NULL || !be->in_use) {
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&be->lock);
	be->recv_thread      = k_current_get();
	k_spin_unlock(&be->lock, key);
	atomic_inc(&be->cb_active);

	const void *payload     = NULL;
	size_t      payload_len = 0;
	const char *method      = frame_parse(data, len, &payload, &payload_len);
	if (method == NULL) {
		LOG_WRN("rpc: malformed frame on %s (len=%zu)", be->name, len);
		goto epilogue;
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
		goto epilogue;
	}

	{
		/* Async dispatch via the per-method subscribe table. */
		uint32_t        h   = fnv1a_32(method);
		struct rpc_sub *sub = sub_find(be, method, h);
		if (sub != NULL && sub->cb != NULL) {
			/* GHSA-xhm8-7f87-93q5: cb() may call alp_rpc_close() on
             * THIS channel (self-close) -- see this function's doc
             * comment and z_shutdown()'s from_worker detection. */
			sub->cb(payload, payload_len, sub->user);
		}
	}

epilogue:
	/* GHSA-xhm8-7f87-93q5 DEFERRED epilogue: if the callback above (if
     * any) closed its own channel, z_shutdown() set close_from_worker
     * on THIS same thread/call-stack -- finish the deferred teardown
     * here, exactly once, now that the callback has returned.
     * Deregistering here (rather than in z_shutdown()) avoids calling
     * ipc_service_deregister_endpoint() reentrantly from within its
     * own `received` callback. */
	if (be->close_from_worker) {
		(void)ipc_service_deregister_endpoint(&be->ept);
		atomic_dec(&be->cb_active);
		alp_rpc_close_finalize(be->owner);
		return;
	}
	atomic_dec(&be->cb_active);
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
	be->closing           = false;
	be->close_from_worker = false;
	be->owner             = st->owner;

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

/**
 * @brief Synchronous request/response.
 *
 * @par Sticky cancel (GHSA-xhm8-7f87-93q5 redesign)
 * `be->closing` is checked under `be->lock` (a spinlock -- see struct
 * rpc_be's doc comment) in the SAME critical section as staging the
 * call slot, so a call that stages itself AFTER z_shutdown() already
 * ran observes `closing` already true and bails immediately instead
 * of blocking on a k_sem_take() nobody is left to k_sem_give() --
 * closing the "late-staging" gap a one-shot cancel alone would miss.
 * z_shutdown() already leaves `call_result`/`call_pending` correctly
 * set to ALP_ERR_NOT_READY/false for a call that was ALREADY staged
 * when it ran, so no extra post-wait `closing` check is needed here
 * (unlike the yocto backend's belt-and-suspenders version -- see
 * y_call()'s doc comment in yocto_drv.c for why both are correct).
 */
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

	/* Check-then-stage as ONE spinlock critical section. */
	k_spinlock_key_t key = k_spin_lock(&be->lock);
	if (be->closing) {
		k_spin_unlock(&be->lock, key);
		k_mutex_unlock(&be->tx_mutex);
		return ALP_ERR_NOT_READY;
	}
	strncpy(be->call_method, method, sizeof(be->call_method) - 1);
	be->call_method[sizeof(be->call_method) - 1] = '\0';
	be->call_resp_buf                            = resp;
	be->call_resp_cap = (resp != NULL && resp_len != NULL) ? *resp_len : 0u;
	be->call_resp_len = 0u;
	be->call_result   = ALP_ERR_TIMEOUT;
	be->call_pending  = true;
	k_sem_reset(&be->call_sem);
	k_spin_unlock(&be->lock, key);

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
		key              = k_spin_lock(&be->lock);
		be->call_pending = false;
		k_spin_unlock(&be->lock, key);
		k_mutex_unlock(&be->tx_mutex);
		return s;
	}

	/* Wait for the response (or timeout). */
	k_timeout_t to = (timeout_ms == UINT32_MAX) ? K_FOREVER : K_MSEC(timeout_ms);
	int         rc = k_sem_take(&be->call_sem, to);
	if (rc == -EAGAIN) {
		key              = k_spin_lock(&be->lock);
		be->call_pending = false;
		k_spin_unlock(&be->lock, key);
		s = ALP_ERR_TIMEOUT;
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

/**
 * @brief Make every blocking wait on this channel terminate promptly,
 *        and report whether THIS call itself arrived from the
 *        channel's own ipc_service recv thread.
 *
 * See this file's header comment for the full protocol.  Sets the
 * sticky `closing` flag + cancels any pending call under `be->lock`,
 * wakes it via k_sem_give(), then either (external) deregisters the
 * endpoint and waits for `cb_active` to drain to 0 before returning
 * DONE, or (self-close) returns DEFERRED immediately -- deregistering
 * here would call ipc_service_deregister_endpoint() reentrantly from
 * within its own `received` callback, and waiting on `cb_active` would
 * deadlock (this very call holds it) -- rpc_ept_recv()'s epilogue
 * completes the deferred teardown once the callback that triggered it
 * returns.
 */
static alp_rpc_shutdown_result_t z_shutdown(alp_rpc_backend_state_t *st)
{
#if defined(CONFIG_ALP_SDK_RPC)
	struct rpc_be *be = (struct rpc_be *)st->be_data;
	if (be == NULL || !be->in_use) {
		return ALP_RPC_SHUTDOWN_DONE;
	}

	/* Self-detection assumes ipc_service invokes `received` from
     * ordinary thread context, never from ISR -- see this file's
     * header comment (BENCH-UNVERIFIED). */
	__ASSERT(!k_is_in_isr(), "alp_rpc: shutdown must not run from ISR context");

	k_spinlock_key_t key         = k_spin_lock(&be->lock);
	bool             from_worker = (k_current_get() == be->recv_thread);
	be->closing                  = true;
	if (be->call_pending) {
		be->call_result  = ALP_ERR_NOT_READY;
		be->call_pending = false;
	}
	if (from_worker) {
		be->close_from_worker = true;
	}
	k_spin_unlock(&be->lock, key);

	k_sem_give(&be->call_sem);

	if (from_worker) {
		/* DEFERRED: rpc_ept_recv()'s epilogue (this SAME thread, once
         * the callback returns) deregisters the endpoint, drains
         * cb_active, and calls alp_rpc_close_finalize(be->owner)
         * exactly once. */
		return ALP_RPC_SHUTDOWN_DEFERRED;
	}

	/* External close: deregister so no NEW recv is dispatched, then
     * wait for any recv ALREADY in flight to finish touching `be`
     * before returning DONE -- see this file's header comment for why
     * ipc_service_deregister_endpoint()'s return alone isn't trusted
     * as that barrier (BENCH-UNVERIFIED). */
	(void)ipc_service_deregister_endpoint(&be->ept);
	be->ept_bound = false;
	while (atomic_get(&be->cb_active) != 0) {
		/* Sleep, never spin -- see src/rpc_dispatch.c's _rpc_drain()
         * doc comment for the single-core priority-inversion trap a
         * busy spin (or k_yield(), which only cedes to equal
         * priority) would reintroduce here. */
		k_sleep(K_TICKS(1));
	}
	return ALP_RPC_SHUTDOWN_DONE;
#else
	(void)st;
	return ALP_RPC_SHUTDOWN_DONE;
#endif
}

/**
 * @brief Release the pool slot.  Called exactly once by the
 *        dispatcher, strictly after z_shutdown() has run and every op
 *        counted before the close won has left.  Trivial for this
 *        backend (a static pool slot, no heap, no fds) -- does not
 *        block.
 */
static void z_destroy(alp_rpc_backend_state_t *st)
{
#if defined(CONFIG_ALP_SDK_RPC)
	struct rpc_be *be = (struct rpc_be *)st->be_data;
	if (be == NULL) {
		return;
	}
	st->be_data = NULL;
	_be_free(be);
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
	.shutdown    = z_shutdown,
	.destroy     = z_destroy,
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
