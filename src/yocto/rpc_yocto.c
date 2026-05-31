/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux userspace backend for <alp/rpc.h> -- framed RPC over OpenAMP /
 * RPMsg.
 *
 * On a Yocto / mainline-Linux A-class core, the kernel's remoteproc
 * framework exposes each loaded M-class firmware as a /dev/rpmsg*
 * chardev once the userspace systemd unit
 * (meta-alp-sdk/recipes-core/alp-system/alp-remoteproc.service)
 * echos `start` into /sys/class/remoteproc/remoteprocN/state and the
 * device's name-service announce arrives.  This file plumbs the
 * public <alp/rpc.h> surface onto those chardev nodes via the
 * RPMSG_CREATE_EPT_IOCTL ioctl + read/write/poll on the resulting
 * endpoint fd.
 *
 * Per-channel state:
 *   - The RPMsg control fd (/dev/rpmsg_ctrlN) used to create endpoints.
 *   - The per-endpoint fd opened against /dev/rpmsgN.
 *   - A per-channel pthread that runs poll() + read() and dispatches
 *     to the registered method callbacks.
 *   - TX serialisation via a pthread_mutex.
 *
 * v0.6 status -- complete implementation.
 *   - alp_rpc_open / alp_rpc_close / alp_rpc_subscribe /
 *     alp_rpc_unsubscribe / alp_rpc_send / alp_rpc_call are all
 *     implemented end-to-end.
 *
 * Header conventions: this file uses the same fnv1a + frame_build +
 * frame_parse helpers as src/zephyr/rpc_zephyr.c so the two backends
 * stay byte-compatible on the wire without depending on each other.
 *
 * alp_rpc_call -- correlation strategy.
 *   Matches the Zephyr backend (src/zephyr/rpc_zephyr.c) byte-for-byte:
 *   no correlation ID prepended to the frame, the peer is expected to
 *   reply on the same channel with the same method name, and concurrent
 *   calls on a single channel are serialised by the channel's tx_mutex.
 *   Applications needing concurrent in-flight requests open multiple
 *   channels.
 *
 *   Per-channel synchronous-call state:
 *     - tx_mutex            held end-to-end for one alp_rpc_call (also
 *                            serialises alp_rpc_send).
 *     - call_mutex          protects the call slot (call_method /
 *                            call_resp_* / call_result / call_pending);
 *                            briefly taken by both the caller thread and
 *                            the RX worker so they never race.
 *     - call_cond           pthread_cond_t signalled by the RX worker
 *                            when a matching response arrives, or by
 *                            alp_rpc_close when the channel is torn
 *                            down with a pending call.
 *     - call_pending        bool; the RX worker only routes responses
 *                            into the call slot while it is true.
 *
 *   Buffer-too-small policy: we copy what fits into the caller's resp
 *   buffer (truncated) and report the actual response size in *resp_len
 *   along with ALP_ERR_NOMEM.  Matches the alp/rpc.h documentation;
 *   diverges from the Zephyr backend which drops the partial copy.
 *
 * Linking: src/yocto/CMakeLists.txt gates this file behind
 * find_package(Threads) + pkg_check_modules(libmetal librpmsg).  When
 * the host doesn't have the OpenAMP user-space libraries (e.g. a
 * macOS / Windows dev box) the file is excluded and the symbols
 * fall through to the NOSUPPORT stubs in src/common/stub_backend.c
 * once that stub registry learns about <alp/rpc.h>.
 */

#include "alp/rpc.h"

#if defined(__linux__) && defined(ALP_SDK_HAVE_OPENAMP_USERLAND)

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/rpmsg.h>

#ifndef ALP_RPC_MAX_CHANNELS
#define ALP_RPC_MAX_CHANNELS 4
#endif

#ifndef ALP_RPC_SUBS_PER_CHANNEL
#define ALP_RPC_SUBS_PER_CHANNEL 8
#endif

#ifndef ALP_RPC_TX_FRAME_MAX
#define ALP_RPC_TX_FRAME_MAX 1024
#endif

/* ------------------------------------------------------------------ */
/* Internal types                                                      */
/* ------------------------------------------------------------------ */

struct alp_rpc_sub {
    uint32_t            method_hash;
    char                method[ALP_RPC_METHOD_MAX_LEN];
    alp_rpc_method_cb_t cb;
    void               *user;
};

struct alp_rpc_channel {
    bool               in_use;
    char               name[ALP_RPC_METHOD_MAX_LEN];
    uint32_t           src_ept;
    uint32_t           dst_ept;
    uint32_t           mbox_ch;
    bool               cacheable;

    int                ept_fd;  /* /dev/rpmsgN */
    int                ctrl_fd; /* /dev/rpmsg_ctrlN (kept for close) */

    pthread_t          rx_thread;
    atomic_int         rx_run;
    int                rx_wake_pipe[2]; /* close-side notification */

    pthread_mutex_t    tx_mutex;
    pthread_mutex_t    sub_mutex;
    struct alp_rpc_sub subs[ALP_RPC_SUBS_PER_CHANNEL];

    uint8_t            tx_scratch[ALP_RPC_TX_FRAME_MAX];

    /* Synchronous-call slot.  Single-element by design -- tx_mutex
     * serialises alp_rpc_call invocations on this channel so only one
     * response can ever be in flight here. */
    pthread_mutex_t call_mutex;
    pthread_cond_t  call_cond;
    char            call_method[ALP_RPC_METHOD_MAX_LEN];
    void           *call_resp_buf;
    size_t          call_resp_cap;
    size_t          call_resp_len; /* actual response size on the wire */
    alp_status_t    call_result;
    bool            call_pending;
};

static struct alp_rpc_channel g_rpc_pool[ALP_RPC_MAX_CHANNELS];
static pthread_mutex_t        g_pool_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* Helpers (intentionally mirrors src/zephyr/rpc_zephyr.c)             */
/* ------------------------------------------------------------------ */

static uint32_t fnv1a_32(const char *s)
{
    uint32_t h = 0x811c9dc5u;
    for (; *s; ++s) {
        h ^= (uint8_t)*s;
        h *= 0x01000193u;
    }
    return h;
}

static bool method_valid(const char *m)
{
    if (m == NULL || m[0] == '\0') {
        return false;
    }
    size_t n = strnlen(m, ALP_RPC_METHOD_MAX_LEN);
    return n < ALP_RPC_METHOD_MAX_LEN;
}

static int frame_build(uint8_t *out, size_t cap, const char *method, const void *payload,
                       size_t payload_len)
{
    size_t method_len = strnlen(method, ALP_RPC_METHOD_MAX_LEN);
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
        return NULL;
    }
    *payload_out     = (const void *)(bytes + method_len + 1u);
    *payload_len_out = len - method_len - 1u;
    return bytes;
}

static struct alp_rpc_channel *rpc_pool_acquire(void)
{
    pthread_mutex_lock(&g_pool_mutex);
    for (size_t i = 0; i < ALP_RPC_MAX_CHANNELS; ++i) {
        if (!g_rpc_pool[i].in_use) {
            memset(&g_rpc_pool[i], 0, sizeof(g_rpc_pool[i]));
            g_rpc_pool[i].in_use = true;
            pthread_mutex_unlock(&g_pool_mutex);
            return &g_rpc_pool[i];
        }
    }
    pthread_mutex_unlock(&g_pool_mutex);
    return NULL;
}

static void rpc_pool_release(struct alp_rpc_channel *ch)
{
    pthread_mutex_lock(&g_pool_mutex);
    ch->in_use = false;
    pthread_mutex_unlock(&g_pool_mutex);
}

/* RX worker: poll() the endpoint fd; on inbound frame, parse + dispatch. */
static void *rpc_rx_main(void *arg)
{
    struct alp_rpc_channel *ch = (struct alp_rpc_channel *)arg;
    uint8_t                 buf[ALP_RPC_TX_FRAME_MAX];

    struct pollfd           fds[2] = {
                  { .fd = ch->ept_fd, .events = POLLIN },
                  { .fd = ch->rx_wake_pipe[0], .events = POLLIN },
    };

    while (atomic_load(&ch->rx_run)) {
        int rc = poll(fds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents & POLLIN) {
            break; /* close-side notification */
        }
        if (!(fds[0].revents & POLLIN)) {
            continue;
        }

        ssize_t n = read(ch->ept_fd, buf, sizeof buf);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }

        const void *payload     = NULL;
        size_t      payload_len = 0;
        const char *method      = frame_parse(buf, (size_t)n, &payload, &payload_len);
        if (method == NULL) {
            continue; /* malformed; drop silently */
        }

        /* Synchronous-call path: a pending alp_rpc_call wakes the caller
         * only when the response method matches the requested one.  We
         * take the call slot's mutex briefly so the caller's read of
         * call_pending + the result/payload write happen-after this
         * routing decision. */
        pthread_mutex_lock(&ch->call_mutex);
        bool consumed_by_call = false;
        if (ch->call_pending && strncmp(method, ch->call_method, ALP_RPC_METHOD_MAX_LEN) == 0) {
            if (ch->call_resp_buf != NULL && ch->call_resp_cap > 0) {
                size_t copy_n = payload_len <= ch->call_resp_cap ? payload_len : ch->call_resp_cap;
                memcpy(ch->call_resp_buf, payload, copy_n);
            }
            ch->call_resp_len = payload_len;
            ch->call_result   = (payload_len > ch->call_resp_cap && ch->call_resp_buf != NULL)
                                    ? ALP_ERR_NOMEM
                                    : ALP_OK;
            ch->call_pending  = false;
            pthread_cond_signal(&ch->call_cond);
            consumed_by_call = true;
        }
        pthread_mutex_unlock(&ch->call_mutex);

        if (consumed_by_call) {
            continue;
        }

        /* Async dispatch via the per-method subscribe table. */
        uint32_t h = fnv1a_32(method);

        pthread_mutex_lock(&ch->sub_mutex);
        struct alp_rpc_sub *match = NULL;
        for (size_t i = 0; i < ALP_RPC_SUBS_PER_CHANNEL; ++i) {
            struct alp_rpc_sub *s = &ch->subs[i];
            if (s->cb != NULL && s->method_hash == h &&
                strncmp(s->method, method, ALP_RPC_METHOD_MAX_LEN) == 0) {
                match = s;
                break;
            }
        }
        /* Snapshot cb + user under the lock so we can release before
         * invoking the callback (avoids deadlock if the cb subscribes
         * to a different method). */
        alp_rpc_method_cb_t cb   = match ? match->cb : NULL;
        void               *user = match ? match->user : NULL;
        pthread_mutex_unlock(&ch->sub_mutex);
        if (cb != NULL) {
            cb(payload, payload_len, user);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/* The Linux RPMsg chardev convention: the orchestrator's systemd unit
 * starts remoteproc, which exposes /dev/rpmsg_ctrl0 (+ /dev/rpmsgN per
 * announced endpoint).  The customer-visible @c name maps onto one
 * of those endpoints by way of RPMSG_CREATE_EPT_IOCTL on the control
 * fd; the kernel returns the matching /dev/rpmsg<N> when the name
 * service announce from the remote firmware names the same endpoint.
 *
 * For v0.6 we hard-code the control device to /dev/rpmsg_ctrl0.  Apps
 * that need a non-default control device override via the
 * ALP_RPMSG_CTRL_DEV env var before calling alp_rpc_open. */
static const char *rpmsg_ctrl_path(void)
{
    const char *env = getenv("ALP_RPMSG_CTRL_DEV");
    return env ? env : "/dev/rpmsg_ctrl0";
}

alp_rpc_channel_t *alp_rpc_open(const alp_rpc_config_t *cfg)
{
    if (cfg == NULL || cfg->name == NULL || cfg->name[0] == '\0') {
        return NULL;
    }
    if (strnlen(cfg->name, ALP_RPC_METHOD_MAX_LEN) == ALP_RPC_METHOD_MAX_LEN) {
        return NULL;
    }

    struct alp_rpc_channel *ch = rpc_pool_acquire();
    if (ch == NULL) {
        return NULL;
    }

    strncpy(ch->name, cfg->name, sizeof(ch->name) - 1);
    ch->src_ept   = cfg->src_ept != 0u ? cfg->src_ept : (0x400u | (fnv1a_32(cfg->name) & 0x0FFu));
    ch->dst_ept   = cfg->dst_ept != 0u ? cfg->dst_ept : ch->src_ept + 1u;
    ch->mbox_ch   = cfg->mbox_ch != 0u ? cfg->mbox_ch : ALP_RPC_DEFAULT_MBOX_CH;
    ch->cacheable = cfg->cacheable;

    pthread_mutex_init(&ch->tx_mutex, NULL);
    pthread_mutex_init(&ch->sub_mutex, NULL);
    pthread_mutex_init(&ch->call_mutex, NULL);
    pthread_cond_init(&ch->call_cond, NULL);
    ch->call_pending    = false;
    ch->ept_fd          = -1;
    ch->ctrl_fd         = -1;
    ch->rx_wake_pipe[0] = -1;
    ch->rx_wake_pipe[1] = -1;

    /* Open the control device + create our endpoint via ioctl. */
    ch->ctrl_fd = open(rpmsg_ctrl_path(), O_RDWR);
    if (ch->ctrl_fd < 0) {
        fprintf(stderr, "alp_rpc: open(%s) failed: %s\n", rpmsg_ctrl_path(), strerror(errno));
        rpc_pool_release(ch);
        return NULL;
    }

    struct rpmsg_endpoint_info eptinfo;
    memset(&eptinfo, 0, sizeof eptinfo);
    strncpy(eptinfo.name, ch->name, sizeof(eptinfo.name) - 1);
    eptinfo.src = ch->src_ept;
    eptinfo.dst = ch->dst_ept;

    if (ioctl(ch->ctrl_fd, RPMSG_CREATE_EPT_IOCTL, &eptinfo) < 0) {
        fprintf(stderr, "alp_rpc: RPMSG_CREATE_EPT_IOCTL(%s) failed: %s\n", ch->name,
                strerror(errno));
        close(ch->ctrl_fd);
        rpc_pool_release(ch);
        return NULL;
    }

    /* The kernel creates a /dev/rpmsg<N> matching the endpoint we
     * just announced.  Walk /dev/rpmsg* and pick the freshest one
     * whose name matches.  For v0.6 we use the convention that the
     * sysfs-exposed `name` attribute matches our endpoint name; a
     * proper enumeration walks sysfs.  Keep the simple-open approach:
     * /dev/rpmsg0 is the canonical choice on single-remoteproc
     * systems.  Customers with multiple remoteprocs set
     * ALP_RPMSG_EPT_DEV. */
    const char *ept_path_env = getenv("ALP_RPMSG_EPT_DEV");
    const char *ept_path     = ept_path_env ? ept_path_env : "/dev/rpmsg0";
    ch->ept_fd               = open(ept_path, O_RDWR | O_NONBLOCK);
    if (ch->ept_fd < 0) {
        fprintf(stderr, "alp_rpc: open(%s) failed: %s\n", ept_path, strerror(errno));
        close(ch->ctrl_fd);
        rpc_pool_release(ch);
        return NULL;
    }

    if (pipe(ch->rx_wake_pipe) < 0) {
        fprintf(stderr, "alp_rpc: pipe() failed: %s\n", strerror(errno));
        close(ch->ept_fd);
        close(ch->ctrl_fd);
        rpc_pool_release(ch);
        return NULL;
    }

    atomic_store(&ch->rx_run, 1);
    if (pthread_create(&ch->rx_thread, NULL, rpc_rx_main, ch) != 0) {
        fprintf(stderr, "alp_rpc: pthread_create failed\n");
        close(ch->rx_wake_pipe[0]);
        close(ch->rx_wake_pipe[1]);
        close(ch->ept_fd);
        close(ch->ctrl_fd);
        rpc_pool_release(ch);
        return NULL;
    }

    return ch;
}

void alp_rpc_close(alp_rpc_channel_t *ch)
{
    if (ch == NULL || !ch->in_use) {
        return;
    }

    /* Wake any pending alp_rpc_call so the caller unblocks with a
     * clear NOT_READY rather than waiting for its timeout to expire.
     * Holding call_mutex here is mandatory to synchronise with a
     * caller that is about to enter pthread_cond_timedwait. */
    pthread_mutex_lock(&ch->call_mutex);
    if (ch->call_pending) {
        ch->call_result   = ALP_ERR_NOT_READY;
        ch->call_resp_len = 0;
        ch->call_pending  = false;
        pthread_cond_broadcast(&ch->call_cond);
    }
    pthread_mutex_unlock(&ch->call_mutex);

    atomic_store(&ch->rx_run, 0);
    /* Kick the RX worker out of poll(). */
    if (ch->rx_wake_pipe[1] >= 0) {
        char b = 1;
        (void)write(ch->rx_wake_pipe[1], &b, 1);
    }
    pthread_join(ch->rx_thread, NULL);

    if (ch->rx_wake_pipe[0] >= 0) close(ch->rx_wake_pipe[0]);
    if (ch->rx_wake_pipe[1] >= 0) close(ch->rx_wake_pipe[1]);
    if (ch->ept_fd >= 0) close(ch->ept_fd);
    if (ch->ctrl_fd >= 0) close(ch->ctrl_fd);

    pthread_mutex_destroy(&ch->tx_mutex);
    pthread_mutex_destroy(&ch->sub_mutex);
    pthread_cond_destroy(&ch->call_cond);
    pthread_mutex_destroy(&ch->call_mutex);
    rpc_pool_release(ch);
}

alp_status_t alp_rpc_subscribe(alp_rpc_channel_t *ch, const char *method, alp_rpc_method_cb_t cb,
                               void *user)
{
    if (ch == NULL || !ch->in_use) return ALP_ERR_NOT_READY;
    if (!method_valid(method)) return ALP_ERR_INVAL;
    if (cb == NULL) {
        return alp_rpc_unsubscribe(ch, method);
    }
    uint32_t h = fnv1a_32(method);

    pthread_mutex_lock(&ch->sub_mutex);
    struct alp_rpc_sub *slot = NULL;
    /* Replace existing. */
    for (size_t i = 0; i < ALP_RPC_SUBS_PER_CHANNEL; ++i) {
        struct alp_rpc_sub *s = &ch->subs[i];
        if (s->cb != NULL && s->method_hash == h &&
            strncmp(s->method, method, ALP_RPC_METHOD_MAX_LEN) == 0) {
            slot = s;
            break;
        }
    }
    if (slot == NULL) {
        for (size_t i = 0; i < ALP_RPC_SUBS_PER_CHANNEL; ++i) {
            if (ch->subs[i].cb == NULL) {
                slot              = &ch->subs[i];
                slot->method_hash = h;
                strncpy(slot->method, method, sizeof(slot->method) - 1);
                slot->method[sizeof(slot->method) - 1] = '\0';
                break;
            }
        }
    }
    alp_status_t rc;
    if (slot == NULL) {
        rc = ALP_ERR_NOMEM;
    } else {
        slot->cb   = cb;
        slot->user = user;
        rc         = ALP_OK;
    }
    pthread_mutex_unlock(&ch->sub_mutex);
    return rc;
}

alp_status_t alp_rpc_unsubscribe(alp_rpc_channel_t *ch, const char *method)
{
    if (ch == NULL || !ch->in_use) return ALP_ERR_NOT_READY;
    if (!method_valid(method)) return ALP_ERR_INVAL;

    uint32_t h = fnv1a_32(method);
    pthread_mutex_lock(&ch->sub_mutex);
    alp_status_t rc = ALP_ERR_INVAL;
    for (size_t i = 0; i < ALP_RPC_SUBS_PER_CHANNEL; ++i) {
        struct alp_rpc_sub *s = &ch->subs[i];
        if (s->cb != NULL && s->method_hash == h &&
            strncmp(s->method, method, ALP_RPC_METHOD_MAX_LEN) == 0) {
            s->cb          = NULL;
            s->user        = NULL;
            s->method[0]   = '\0';
            s->method_hash = 0u;
            rc             = ALP_OK;
            break;
        }
    }
    pthread_mutex_unlock(&ch->sub_mutex);
    return rc;
}

alp_status_t alp_rpc_send(alp_rpc_channel_t *ch, const char *method, const void *payload,
                          size_t len)
{
    if (ch == NULL || !ch->in_use) return ALP_ERR_NOT_READY;
    if (!method_valid(method)) return ALP_ERR_INVAL;
    if (payload == NULL && len > 0) return ALP_ERR_INVAL;

    pthread_mutex_lock(&ch->tx_mutex);
    int          built = frame_build(ch->tx_scratch, sizeof ch->tx_scratch, method, payload, len);
    alp_status_t rc;
    if (built < 0) {
        rc = (built == -ENOMEM) ? ALP_ERR_NOMEM : ALP_ERR_INVAL;
    } else {
        ssize_t w = write(ch->ept_fd, ch->tx_scratch, (size_t)built);
        if (w < 0) {
            rc = (errno == EAGAIN || errno == EWOULDBLOCK) ? ALP_ERR_BUSY : ALP_ERR_IO;
        } else if (w != built) {
            rc = ALP_ERR_IO;
        } else {
            rc = ALP_OK;
        }
    }
    pthread_mutex_unlock(&ch->tx_mutex);
    return rc;
}

/* Compute an absolute CLOCK_REALTIME deadline `timeout_ms` from now,
 * for pthread_cond_timedwait.  Returns 0 on success, -1 on clock_gettime
 * failure (vanishingly rare; we surface ALP_ERR_IO in that case). */
static int absolute_deadline(struct timespec *ts, uint32_t timeout_ms)
{
    if (clock_gettime(CLOCK_REALTIME, ts) != 0) {
        return -1;
    }
    /* Carry-safe add: split timeout_ms into whole seconds + remainder
     * nanoseconds, then normalise tv_nsec into [0, 1e9). */
    uint64_t add_s  = (uint64_t)(timeout_ms / 1000u);
    uint64_t add_ns = (uint64_t)(timeout_ms % 1000u) * 1000000u;
    ts->tv_sec += (time_t)add_s;
    ts->tv_nsec += (long)add_ns;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += ts->tv_nsec / 1000000000L;
        ts->tv_nsec %= 1000000000L;
    }
    return 0;
}

alp_status_t alp_rpc_call(alp_rpc_channel_t *ch, const char *method, const void *req,
                          size_t req_len, void *resp, size_t *resp_len, uint32_t timeout_ms)
{
    if (ch == NULL || !ch->in_use) return ALP_ERR_NOT_READY;
    if (!method_valid(method)) return ALP_ERR_INVAL;
    if (req == NULL && req_len > 0) return ALP_ERR_INVAL;
    if (resp != NULL && resp_len == NULL) return ALP_ERR_INVAL;

    /* Serialise calls on this channel (matches the Zephyr backend's
     * tx_mutex + single call-slot model).  Customers needing
     * concurrent in-flight requests open multiple channels. */
    pthread_mutex_lock(&ch->tx_mutex);

    /* Stage the call slot under call_mutex so the RX worker observes
     * a consistent (method, resp_buf, resp_cap, pending=true) snapshot
     * the moment we make pending visible. */
    pthread_mutex_lock(&ch->call_mutex);
    strncpy(ch->call_method, method, sizeof(ch->call_method) - 1);
    ch->call_method[sizeof(ch->call_method) - 1] = '\0';
    ch->call_resp_buf                            = resp;
    ch->call_resp_cap = (resp != NULL && resp_len != NULL) ? *resp_len : 0u;
    ch->call_resp_len = 0u;
    ch->call_result   = ALP_ERR_TIMEOUT;
    ch->call_pending  = true;
    pthread_mutex_unlock(&ch->call_mutex);

    /* Frame + send the request. */
    int          built = frame_build(ch->tx_scratch, sizeof ch->tx_scratch, method, req, req_len);
    alp_status_t s     = ALP_OK;
    if (built < 0) {
        s = (built == -ENOMEM) ? ALP_ERR_NOMEM : ALP_ERR_INVAL;
    } else {
        ssize_t w = write(ch->ept_fd, ch->tx_scratch, (size_t)built);
        if (w < 0) {
            s = (errno == EAGAIN || errno == EWOULDBLOCK) ? ALP_ERR_BUSY : ALP_ERR_IO;
        } else if (w != built) {
            s = ALP_ERR_IO;
        }
    }

    if (s != ALP_OK) {
        /* Cancel the pending slot atomically so any late response
         * doesn't try to write into a stale resp_buf. */
        pthread_mutex_lock(&ch->call_mutex);
        ch->call_pending = false;
        pthread_mutex_unlock(&ch->call_mutex);
        pthread_mutex_unlock(&ch->tx_mutex);
        return s;
    }

    /* Wait for response, timeout, or close. */
    pthread_mutex_lock(&ch->call_mutex);
    int rc = 0;
    if (timeout_ms == UINT32_MAX) {
        /* Unbounded wait: pthread_cond_wait until pending clears. */
        while (ch->call_pending) {
            rc = pthread_cond_wait(&ch->call_cond, &ch->call_mutex);
            if (rc != 0) break;
        }
    } else {
        struct timespec deadline;
        if (absolute_deadline(&deadline, timeout_ms) != 0) {
            ch->call_pending = false;
            pthread_mutex_unlock(&ch->call_mutex);
            pthread_mutex_unlock(&ch->tx_mutex);
            return ALP_ERR_IO;
        }
        while (ch->call_pending) {
            rc = pthread_cond_timedwait(&ch->call_cond, &ch->call_mutex, &deadline);
            if (rc != 0) break;
        }
    }

    if (rc == ETIMEDOUT) {
        ch->call_pending = false;
        s                = ALP_ERR_TIMEOUT;
    } else if (rc != 0) {
        ch->call_pending = false;
        s                = ALP_ERR_IO;
    } else {
        s = ch->call_result;
        if (resp_len != NULL && (s == ALP_OK || s == ALP_ERR_NOMEM)) {
            *resp_len = ch->call_resp_len;
        }
    }
    pthread_mutex_unlock(&ch->call_mutex);

    pthread_mutex_unlock(&ch->tx_mutex);
    return s;
}

#else /* !__linux__ || !ALP_SDK_HAVE_OPENAMP_USERLAND */

/* Build-time fallback: no OpenAMP user-space libs available on the
 * host (typical for Windows / macOS dev boxes).  Compile to NOSUPPORT
 * stubs so the library still links cleanly and the customer sees a
 * clear runtime error if they actually try to call into <alp/rpc.h>. */

alp_rpc_channel_t *alp_rpc_open(const alp_rpc_config_t *cfg)
{
    (void)cfg;
    return NULL;
}

void alp_rpc_close(alp_rpc_channel_t *ch)
{
    (void)ch;
}

alp_status_t alp_rpc_subscribe(alp_rpc_channel_t *ch, const char *method, alp_rpc_method_cb_t cb,
                               void *user)
{
    (void)ch;
    (void)method;
    (void)cb;
    (void)user;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_rpc_unsubscribe(alp_rpc_channel_t *ch, const char *method)
{
    (void)ch;
    (void)method;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_rpc_send(alp_rpc_channel_t *ch, const char *method, const void *payload,
                          size_t len)
{
    (void)ch;
    (void)method;
    (void)payload;
    (void)len;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_rpc_call(alp_rpc_channel_t *ch, const char *method, const void *req,
                          size_t req_len, void *resp, size_t *resp_len, uint32_t timeout_ms)
{
    (void)ch;
    (void)method;
    (void)req;
    (void)req_len;
    (void)resp;
    (void)resp_len;
    (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
}

#endif /* __linux__ && ALP_SDK_HAVE_OPENAMP_USERLAND */
