/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/rpc.h> -- framed RPC over OpenAMP / RPMsg.
 *
 * Builds on Zephyr v4.4's `subsys/ipc/ipc_service` framework with
 * the rpmsg backend (CONFIG_IPC_SERVICE_BACKEND_RPMSG=y).  The
 * orchestrator's generated <alp/system_ipc.h> + the per-board DT
 * overlay under zephyr/boards/ wire up the carve-out, mailbox
 * channel, and endpoint IDs; this file just plumbs the public
 * <alp/rpc.h> surface onto the ipc_service callbacks.
 *
 * Per-handle state lives in a tiny fixed-size pool (4 channels --
 * apps rarely run more than two on a single Zephyr slice).  Each
 * channel carries an 8-entry per-method dispatch table keyed on
 * an FNV-1a hash of the method name; collisions store a linear
 * chain.  Real overflow returns ALP_ERR_NOMEM with a clear log
 * message so the customer learns to raise the cap.
 *
 * alp_rpc_call uses a per-channel response slot + a Zephyr
 * k_sem(0,1) for the synchronous wait.  Concurrent calls on the
 * same channel are serialised via the channel's tx_mutex (per
 * the public-API note in alp/rpc.h).
 *
 * Gated on CONFIG_ALP_SDK_RPC.  Off-builds compile to a
 * NOSUPPORT shim so apps that don't opt in still link.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "alp/rpc.h"
#include "handles.h"

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

/* v0.6 cap: 8 method subscriptions per channel.  Document the limit
 * in the header; raise here + in zephyr/Kconfig when an app legitimately
 * needs more.  Subscribe beyond the cap returns ALP_ERR_NOMEM. */
#ifndef CONFIG_ALP_SDK_RPC_SUBS_PER_CHANNEL
#define CONFIG_ALP_SDK_RPC_SUBS_PER_CHANNEL 8
#endif

/* Max TX framed payload size including the method header (<=32 B).  The
 * payload itself is opaque -- raise this when apps need larger frames.
 * 1024 B comfortably fits short sensor records + JSON snippets. */
#ifndef CONFIG_ALP_SDK_RPC_TX_FRAME_MAX
#define CONFIG_ALP_SDK_RPC_TX_FRAME_MAX 1024
#endif

/* ------------------------------------------------------------------ */
/* Internal handle structures                                          */
/* ------------------------------------------------------------------ */

struct alp_rpc_sub {
    uint32_t            method_hash; /* FNV-1a(method) */
    char                method[ALP_RPC_METHOD_MAX_LEN];
    alp_rpc_method_cb_t cb;
    void               *user;
};

struct alp_rpc_channel {
    bool in_use;

#if defined(CONFIG_ALP_SDK_RPC)
    /* Configuration (cached at open time). */
    char     name[ALP_RPC_METHOD_MAX_LEN];
    uint32_t src_ept;
    uint32_t dst_ept;
    uint32_t mbox_ch;
    bool     cacheable;

    /* Zephyr ipc_service handles.  The endpoint name follows the
     * RPMsg name-service convention; the orchestrator picks IDs
     * deterministically so both sides agree without a NS handshake. */
    const struct device *ipc_dev;
    struct ipc_ept       ept;
    struct ipc_ept_cfg   ept_cfg;
    bool                 ept_bound;

    /* Subscribe table.  Linear probe on collision; 8 slots is plenty
     * for the v0.6 framing budget. */
    struct alp_rpc_sub subs[CONFIG_ALP_SDK_RPC_SUBS_PER_CHANNEL];

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

/* Pure-string helper -- always compiled in so the public API stubs
 * below (which run even when CONFIG_ALP_SDK_RPC is off and just
 * return ALP_ERR_NOSUPPORT) can still validate caller arguments
 * before short-circuiting.  Pre-bounds-check protects strnlen() from
 * unterminated input. */
static bool method_valid(const char *m)
{
    if (m == NULL || m[0] == '\0') {
        return false;
    }
    /* Bounded length check.  strnlen() is POSIX, not C99 -- Zephyr's
     * default libc settings don't always expose it; -Werror catches
     * the implicit declaration.  This open-coded loop is portable. */
    for (size_t i = 0; i < ALP_RPC_METHOD_MAX_LEN; ++i) {
        if (m[i] == '\0') {
            return true;
        }
    }
    return false; /* unterminated within budget */
}

#if defined(CONFIG_ALP_SDK_RPC)

static struct alp_rpc_channel g_rpc_pool[CONFIG_ALP_SDK_RPC_MAX_CHANNELS];

static uint32_t               fnv1a_32(const char *s)
{
    uint32_t h = 0x811c9dc5u;
    for (; *s; ++s) {
        h ^= (uint8_t)*s;
        h *= 0x01000193u;
    }
    return h;
}

static struct alp_rpc_channel *rpc_pool_acquire(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_rpc_pool); ++i) {
        if (!g_rpc_pool[i].in_use) {
            memset(&g_rpc_pool[i], 0, sizeof(g_rpc_pool[i]));
            g_rpc_pool[i].in_use = true;
            return &g_rpc_pool[i];
        }
    }
    return NULL;
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

static struct alp_rpc_sub *sub_find(struct alp_rpc_channel *ch, const char *method, uint32_t hash)
{
    for (size_t i = 0; i < ARRAY_SIZE(ch->subs); ++i) {
        struct alp_rpc_sub *s = &ch->subs[i];
        if (s->cb != NULL && s->method_hash == hash &&
            strncmp(s->method, method, ALP_RPC_METHOD_MAX_LEN) == 0) {
            return s;
        }
    }
    return NULL;
}

static struct alp_rpc_sub *sub_alloc(struct alp_rpc_channel *ch)
{
    for (size_t i = 0; i < ARRAY_SIZE(ch->subs); ++i) {
        if (ch->subs[i].cb == NULL) {
            return &ch->subs[i];
        }
    }
    return NULL;
}

/* Parse the on-wire frame: NUL-terminated method header followed by
 * the opaque payload.  Returns the method-name pointer + payload
 * window; NULL on malformed frames. */
static const char *frame_parse(const void *data, size_t len, const void **payload_out,
                               size_t *payload_len_out)
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
        /* no NUL terminator within the budget */
        return NULL;
    }
    *payload_out     = (const void *)(bytes + method_len + 1u);
    *payload_len_out = len - method_len - 1u;
    return bytes;
}

/* Bounded strlen -- C99-portable replacement for POSIX strnlen().
 * Returns the index of the first '\0' or `cap` if none found within
 * the budget.  Matches strnlen() semantics for the way this file
 * uses it (callers compare the result against cap to detect
 * unterminated strings). */
static size_t bounded_strlen(const char *s, size_t cap)
{
    for (size_t i = 0; i < cap; ++i) {
        if (s[i] == '\0') {
            return i;
        }
    }
    return cap;
}

static int frame_build(uint8_t *out, size_t cap, const char *method, const void *payload,
                       size_t payload_len)
{
    size_t method_len = bounded_strlen(method, ALP_RPC_METHOD_MAX_LEN);
    if (method_len == ALP_RPC_METHOD_MAX_LEN) {
        return -EINVAL;
    }
    size_t total = method_len + 1u + payload_len;
    if (total > cap) {
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
    struct alp_rpc_channel *ch = (struct alp_rpc_channel *)priv;
    if (ch != NULL) {
        ch->ept_bound = true;
        LOG_DBG("rpc: endpoint bound name=%s", ch->name);
    }
}

static void rpc_ept_recv(const void *data, size_t len, void *priv)
{
    struct alp_rpc_channel *ch = (struct alp_rpc_channel *)priv;
    if (ch == NULL || !ch->in_use) {
        return;
    }

    const void *payload     = NULL;
    size_t      payload_len = 0;
    const char *method      = frame_parse(data, len, &payload, &payload_len);
    if (method == NULL) {
        LOG_WRN("rpc: malformed frame on %s (len=%zu)", ch->name, len);
        return;
    }

    /* Synchronous-call path: a pending alp_rpc_call wakes the caller
     * only when the response method matches. */
    if (ch->call_pending && strncmp(method, ch->call_method, ALP_RPC_METHOD_MAX_LEN) == 0) {
        if (ch->call_resp_buf != NULL) {
            if (payload_len > ch->call_resp_cap) {
                ch->call_result   = ALP_ERR_NOMEM;
                ch->call_resp_len = 0;
            } else {
                memcpy(ch->call_resp_buf, payload, payload_len);
                ch->call_resp_len = payload_len;
                ch->call_result   = ALP_OK;
            }
        } else {
            ch->call_resp_len = payload_len;
            ch->call_result   = ALP_OK;
        }
        ch->call_pending = false;
        k_sem_give(&ch->call_sem);
        return;
    }

    /* Async dispatch via the per-method subscribe table. */
    uint32_t            h   = fnv1a_32(method);
    struct alp_rpc_sub *sub = sub_find(ch, method, h);
    if (sub != NULL && sub->cb != NULL) {
        sub->cb(payload, payload_len, sub->user);
    }
}

#endif /* CONFIG_ALP_SDK_RPC */

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

alp_rpc_channel_t *alp_rpc_open(const alp_rpc_config_t *cfg)
{
    alp_z_clear_last_error();
    if (cfg == NULL || cfg->name == NULL || cfg->name[0] == '\0') {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
#if defined(CONFIG_ALP_SDK_RPC)
    if (bounded_strlen(cfg->name, ALP_RPC_METHOD_MAX_LEN) == ALP_RPC_METHOD_MAX_LEN) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

    /* Acquire pool slot. */
    struct alp_rpc_channel *ch = rpc_pool_acquire();
    if (ch == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }

    strncpy(ch->name, cfg->name, sizeof(ch->name) - 1);
    ch->src_ept   = cfg->src_ept != 0u ? cfg->src_ept : (0x400u | (fnv1a_32(cfg->name) & 0x0FFu));
    ch->dst_ept   = cfg->dst_ept != 0u ? cfg->dst_ept : ch->src_ept + 1u;
    ch->mbox_ch   = cfg->mbox_ch != 0u ? cfg->mbox_ch : ALP_RPC_DEFAULT_MBOX_CH;
    ch->cacheable = cfg->cacheable;

    k_mutex_init(&ch->tx_mutex);
    k_sem_init(&ch->call_sem, 0, 1);

    /* The Zephyr DT overlay's chosen { zephyr,ipc = ... } picks the
     * default ipc backend.  DEVICE_DT_GET(DT_CHOSEN(zephyr_ipc)) is
     * the standard handle.  In OpenAMP / rpmsg builds the chosen
     * node lives under the rpmsg subnode of the carve-out.  When the
     * chosen alias isn't set we surface a clean NOT_READY so the
     * customer learns to fix their overlay. */
#if DT_HAS_CHOSEN(zephyr_ipc)
    ch->ipc_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_ipc));
#else
    ch->ipc_dev = NULL;
#endif
    if (ch->ipc_dev == NULL || !device_is_ready(ch->ipc_dev)) {
        LOG_ERR("rpc: zephyr,ipc chosen node missing or not ready");
        ch->in_use = false;
        alp_z_set_last_error(ALP_ERR_NOT_READY);
        return NULL;
    }

    int rc = ipc_service_open_instance(ch->ipc_dev);
    if (rc != 0 && rc != -EALREADY) {
        LOG_ERR("rpc: ipc_service_open_instance failed: %d", rc);
        ch->in_use = false;
        alp_z_set_last_error(errno_to_alp(rc));
        return NULL;
    }

    ch->ept_cfg.name        = ch->name;
    ch->ept_cfg.cb.bound    = rpc_ept_bound;
    ch->ept_cfg.cb.received = rpc_ept_recv;
    ch->ept_cfg.priv        = ch;

    rc                      = ipc_service_register_endpoint(ch->ipc_dev, &ch->ept, &ch->ept_cfg);
    if (rc < 0) {
        LOG_ERR("rpc: ipc_service_register_endpoint(%s) failed: %d", ch->name, rc);
        ch->in_use = false;
        alp_z_set_last_error(errno_to_alp(rc));
        return NULL;
    }

    LOG_INF("rpc: opened %s src=0x%x dst=0x%x mbox=%u", ch->name, ch->src_ept, ch->dst_ept,
            ch->mbox_ch);
    return ch;
#else /* !CONFIG_ALP_SDK_RPC */
    (void)cfg;
    alp_z_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
#endif
}

void alp_rpc_close(alp_rpc_channel_t *ch)
{
    if (ch == NULL || !ch->in_use) {
        return;
    }
#if defined(CONFIG_ALP_SDK_RPC)
    /* Wake any pending alp_rpc_call so the caller unblocks. */
    if (ch->call_pending) {
        ch->call_result  = ALP_ERR_NOT_READY;
        ch->call_pending = false;
        k_sem_give(&ch->call_sem);
    }
    (void)ipc_service_deregister_endpoint(&ch->ept);
    ch->ept_bound = false;
    ch->in_use    = false;
#endif
}

alp_status_t alp_rpc_subscribe(alp_rpc_channel_t *ch, const char *method, alp_rpc_method_cb_t cb,
                               void *user)
{
    if (ch == NULL || !ch->in_use) {
        return ALP_ERR_NOT_READY;
    }
    if (!method_valid(method)) {
        return ALP_ERR_INVAL;
    }
#if defined(CONFIG_ALP_SDK_RPC)
    /* NULL cb == unsubscribe -- matches the documented behaviour. */
    if (cb == NULL) {
        return alp_rpc_unsubscribe(ch, method);
    }
    uint32_t h = fnv1a_32(method);

    /* Replace if already present. */
    struct alp_rpc_sub *sub = sub_find(ch, method, h);
    if (sub == NULL) {
        sub = sub_alloc(ch);
        if (sub == NULL) {
            LOG_WRN("rpc: subscribe table full on %s (cap=%d)", ch->name,
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
    (void)cb;
    (void)user;
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_rpc_unsubscribe(alp_rpc_channel_t *ch, const char *method)
{
    if (ch == NULL || !ch->in_use) {
        return ALP_ERR_NOT_READY;
    }
    if (!method_valid(method)) {
        return ALP_ERR_INVAL;
    }
#if defined(CONFIG_ALP_SDK_RPC)
    uint32_t            h   = fnv1a_32(method);
    struct alp_rpc_sub *sub = sub_find(ch, method, h);
    if (sub == NULL) {
        return ALP_ERR_INVAL;
    }
    sub->cb          = NULL;
    sub->user        = NULL;
    sub->method[0]   = '\0';
    sub->method_hash = 0u;
    return ALP_OK;
#else
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_rpc_send(alp_rpc_channel_t *ch, const char *method, const void *payload,
                          size_t len)
{
    if (ch == NULL || !ch->in_use) {
        return ALP_ERR_NOT_READY;
    }
    if (!method_valid(method)) {
        return ALP_ERR_INVAL;
    }
    if (payload == NULL && len > 0) {
        return ALP_ERR_INVAL;
    }
#if defined(CONFIG_ALP_SDK_RPC)
    k_mutex_lock(&ch->tx_mutex, K_FOREVER);
    int          built = frame_build(ch->tx_scratch, sizeof(ch->tx_scratch), method, payload, len);
    alp_status_t s;
    if (built < 0) {
        s = errno_to_alp(built);
    } else {
        int rc = ipc_service_send(&ch->ept, ch->tx_scratch, (size_t)built);
        s      = rc >= 0 ? ALP_OK : errno_to_alp(rc);
    }
    k_mutex_unlock(&ch->tx_mutex);
    return s;
#else
    (void)payload;
    (void)len;
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_rpc_call(alp_rpc_channel_t *ch, const char *method, const void *req,
                          size_t req_len, void *resp, size_t *resp_len, uint32_t timeout_ms)
{
    if (ch == NULL || !ch->in_use) {
        return ALP_ERR_NOT_READY;
    }
    if (!method_valid(method)) {
        return ALP_ERR_INVAL;
    }
    if (resp != NULL && resp_len == NULL) {
        return ALP_ERR_INVAL;
    }
#if defined(CONFIG_ALP_SDK_RPC)
    /* Serialise calls on this channel; the per-channel call slot is
     * single-element by design (see the public-API note in rpc.h). */
    k_mutex_lock(&ch->tx_mutex, K_FOREVER);

    /* Stage the call slot. */
    strncpy(ch->call_method, method, sizeof(ch->call_method) - 1);
    ch->call_method[sizeof(ch->call_method) - 1] = '\0';
    ch->call_resp_buf                            = resp;
    ch->call_resp_cap = (resp != NULL && resp_len != NULL) ? *resp_len : 0u;
    ch->call_resp_len = 0u;
    ch->call_result   = ALP_ERR_TIMEOUT;
    ch->call_pending  = true;
    k_sem_reset(&ch->call_sem);

    /* Frame + send. */
    int          built = frame_build(ch->tx_scratch, sizeof(ch->tx_scratch), method, req, req_len);
    alp_status_t s     = ALP_OK;
    if (built < 0) {
        s = errno_to_alp(built);
    } else {
        int rc = ipc_service_send(&ch->ept, ch->tx_scratch, (size_t)built);
        if (rc < 0) {
            s = errno_to_alp(rc);
        }
    }

    if (s != ALP_OK) {
        ch->call_pending = false;
        k_mutex_unlock(&ch->tx_mutex);
        return s;
    }

    /* Wait for the response (or timeout). */
    k_timeout_t to = (timeout_ms == UINT32_MAX) ? K_FOREVER : K_MSEC(timeout_ms);
    int         rc = k_sem_take(&ch->call_sem, to);
    if (rc == -EAGAIN) {
        ch->call_pending = false;
        s                = ALP_ERR_TIMEOUT;
    } else {
        s = ch->call_result;
        if (s == ALP_OK && resp_len != NULL) {
            *resp_len = ch->call_resp_len;
        }
    }

    k_mutex_unlock(&ch->tx_mutex);
    return s;
#else
    (void)req;
    (void)req_len;
    (void)resp;
    (void)resp_len;
    (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
#endif
}
