/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Real Linux/Yocto rpc_* driver-class backend.  Binds the alp_rpc
 * dispatcher's ops vtable to the kernel RPMsg chardev surface
 * (/dev/rpmsg_ctrlN + /dev/rpmsgN) via RPMSG_CREATE_EPT_IOCTL +
 * read/write/poll on the resulting endpoint fd -- framed RPC over
 * OpenAMP / RPMsg in userland.  Registered at priority 100 with
 * vendor "linux"; the sw_fallback backend (priority 0) still wins on
 * non-Linux native_sim builds where this TU compiles to an empty
 * object.
 *
 * On a Yocto / mainline-Linux A-class core, the kernel's remoteproc
 * framework exposes each loaded M-class firmware as a /dev/rpmsg*
 * chardev once the userspace systemd unit
 * (meta-alp-sdk/recipes-core/alp-system/alp-remoteproc.service)
 * echos `start` into /sys/class/remoteproc/remoteprocN/state and the
 * device's name-service announce arrives.  This backend plumbs the
 * public <alp/rpc.h> surface onto those chardev nodes.
 *
 * Per-channel backend state (struct rpc_be, reached through
 * state->be_data):
 *   - The RPMsg control fd (/dev/rpmsg_ctrlN) used to create endpoints.
 *   - The per-endpoint fd opened against /dev/rpmsgN.
 *   - A per-channel pthread that runs poll() + read() and dispatches
 *     to the registered method callbacks.
 *   - TX serialisation via a pthread_mutex.
 *
 * The OpenAMP / RPMsg vendor calls are preserved verbatim from the
 * original direct-impl (src/yocto/rpc_yocto.c); only the wrapping into
 * the ops vtable + registration changed in the re-home.
 *
 * Header conventions: this file uses the same fnv1a + frame_build +
 * frame_parse helpers as src/backends/rpc/zephyr_drv.c so the two
 * backends stay byte-compatible on the wire without depending on each
 * other.
 *
 * alp_rpc_call -- correlation strategy.
 *   Matches the Zephyr backend byte-for-byte: no correlation ID
 *   prepended to the frame, the peer is expected to reply on the same
 *   channel with the same method name, and concurrent calls on a
 *   single channel are serialised by the channel's tx_mutex.
 *   Applications needing concurrent in-flight requests open multiple
 *   channels.
 *
 *   Buffer-too-small policy: we copy what fits into the caller's resp
 *   buffer (truncated) and report the actual response size in *resp_len
 *   along with ALP_ERR_NOMEM.  Matches the alp/rpc.h documentation.
 *
 * Callback reentrancy / close semantics (GHSA-xhm8-7f87-93q5).
 *   A subscriber callback runs ON the channel's own rx thread and MAY
 *   close its own channel from inside that callback.  y_close()
 *   detects this via pthread_equal(pthread_self(), ch->rx_thread) and,
 *   in that case, never self-joins and never frees the channel out
 *   from under the still-running callback/RX-loop -- it defers the fd/
 *   mutex/free teardown to rpc_rx_main()'s epilogue, which runs on that
 *   same thread once the callback returns and the poll loop unwinds.
 *   A single file-scope mutex (g_rpc_close_lock) makes this race-free
 *   against a concurrent EXTERNAL alp_rpc_close on the same handle:
 *   whichever caller claims ownership first performs the exactly-once
 *   teardown; external close keeps the original join-based path. See
 *   y_close()'s doc comment for the full design.
 *
 * Linking: src/yocto/CMakeLists.txt gates this file behind
 * find_package(Threads) + pkg_check_modules(libmetal librpmsg) and
 * the ALP_SDK_HAVE_OPENAMP_USERLAND define.  When the host doesn't
 * have the OpenAMP user-space libraries (e.g. a macOS / Windows dev
 * box, or a host-only syntax check) the ops compile to NOSUPPORT via
 * the #else branch below, so the TU still builds + links cleanly and
 * the dispatcher falls through to the SW fallback.
 *
 * STATUS: real impl, Yocto-link + on-target run BENCH-UNVERIFIED (no
 *         sysroot / no real /dev/rpmsg* nodes in this environment).
 */

#if defined(__linux__)

#include "alp/rpc.h"

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "rpc_ops.h"

#if defined(ALP_SDK_HAVE_OPENAMP_USERLAND)

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

#ifndef ALP_RPC_SUBS_PER_CHANNEL
#define ALP_RPC_SUBS_PER_CHANNEL 8
#endif

#ifndef ALP_RPC_TX_FRAME_MAX
#define ALP_RPC_TX_FRAME_MAX 1024
#endif

/* ------------------------------------------------------------------ */
/* Backend-owned per-channel state (reached via state->be_data)        */
/* ------------------------------------------------------------------ */

struct alp_rpc_sub {
	uint32_t            method_hash;
	char                method[ALP_RPC_METHOD_MAX_LEN];
	alp_rpc_method_cb_t cb;
	void               *user;
};

/* Per-channel backend block.  Boxed onto the heap so the void* be_data
 * slot in alp_rpc_backend_state_t owns it; the dispatcher's
 * alp_rpc_channel_t pool owns the handle itself. */
struct rpc_be {
	char     name[ALP_RPC_METHOD_MAX_LEN];
	uint32_t src_ept;
	uint32_t dst_ept;
	uint32_t mbox_ch;
	bool     cacheable;

	int ept_fd;  /* /dev/rpmsgN */
	int ctrl_fd; /* /dev/rpmsg_ctrlN (kept for close) */

	pthread_t  rx_thread;
	atomic_int rx_run;
	int        rx_wake_pipe[2]; /* close-side notification */

	pthread_mutex_t    tx_mutex;
	pthread_mutex_t    sub_mutex;
	struct alp_rpc_sub subs[ALP_RPC_SUBS_PER_CHANNEL];

	uint8_t tx_scratch[ALP_RPC_TX_FRAME_MAX];

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

	/* Close-side race protection (GHSA-xhm8-7f87-93q5): records whether
     * the winning y_close() call (see g_rpc_close_lock below) was this
     * channel's own rx_thread closing itself from inside a callback, in
     * which case the physical teardown (fds/mutexes/free) is deferred
     * to rpc_rx_main()'s epilogue -- which runs on that same thread once
     * the callback returns and the poll loop unwinds -- instead of being
     * joined-and-freed synchronously inside y_close().  See y_close()
     * and rpc_rx_main() for the two teardown paths this selects between. */
	bool close_from_worker;
};

/* Single-owner gate for y_close() (GHSA-xhm8-7f87-93q5): a subscriber
 * callback that closes its OWN channel (running on rx_thread) races an
 * external alp_rpc_close on the same handle.  A per-channel mutex
 * doesn't work here -- the mutex itself lives inside `ch`, so locking
 * it is only safe if `ch` hasn't already been freed, which is exactly
 * the property we need to establish.  This one file-scope mutex is
 * never freed, so serialising "read st->be_data, decide who owns the
 * close, and (if won) null st->be_data out" as a single critical
 * section under it is always safe: whichever caller gets the lock
 * first is the owner and unpublishes the channel before releasing it;
 * anyone else (racing in concurrently, or arriving afterwards) that
 * takes the lock next is guaranteed to see st->be_data already NULL. A
 * quick, lock-free pre-check lets every OTHER op (subscribe/send/call)
 * and a plain non-racing close skip the lock entirely. Held only
 * across that tiny decision -- never across the wake/join/teardown
 * work below -- so it cannot deadlock or serialise the rare close path
 * against anything long-running. */
static pthread_mutex_t g_rpc_close_lock = PTHREAD_MUTEX_INITIALIZER;

/* alp_rpc_backend_state_t::be_data (rpc_ops.h, dispatcher-owned) is
 * read from every op in this file (y_open/y_subscribe/y_unsubscribe/
 * y_send/y_call/y_close) and can race a close arriving on another
 * thread -- an external alp_rpc_close versus this channel's own rx
 * thread closing itself from inside a callback (GHSA-xhm8-7f87-93q5).
 * Every access here goes through this load/store pair so the "close
 * nulls it out" / "every op reads it" relationship is a real
 * synchronizes-with edge (a plain, unsynchronised `void *` read/write
 * pair racing a concurrent close is technically undefined behaviour
 * even though it happens to be benign on every architecture this SDK
 * targets today -- ThreadSanitizer flags it as a data race). */
static inline struct rpc_be *rpc_be_data_load(alp_rpc_backend_state_t *st)
{
	return (struct rpc_be *)__atomic_load_n(&st->be_data, __ATOMIC_ACQUIRE);
}

static inline void rpc_be_data_store(alp_rpc_backend_state_t *st, struct rpc_be *ch)
{
	__atomic_store_n(&st->be_data, (void *)ch, __ATOMIC_RELEASE);
}

/* ch->close_from_worker (set once by whichever caller wins y_close()'s
 * single-owner race, read once by rpc_rx_main()'s epilogue) needs the
 * same treatment: the epilogue's read must happen-after the owner's
 * write even when the owner is a DIFFERENT thread (an external close).
 * The rx loop's wake-pipe path breaks out directly on `poll()` seeing
 * POLLIN on the notification fd WITHOUT looping back to re-evaluate
 * `atomic_load(&ch->rx_run)` (see rpc_rx_main()), so that atomic does
 * NOT reliably stand in as the synchronizing edge here -- a plain bool
 * read/write pair left this a genuine (if narrow-window and
 * architecture-benign) data race under ThreadSanitizer.  A real
 * release/acquire pair on this one flag is simpler and cheaper than
 * restructuring the wake-pipe path. */
static inline void rpc_be_close_from_worker_store(struct rpc_be *ch, bool from_worker)
{
	__atomic_store_n(&ch->close_from_worker, from_worker, __ATOMIC_RELEASE);
}

static inline bool rpc_be_close_from_worker_load(struct rpc_be *ch)
{
	return __atomic_load_n(&ch->close_from_worker, __ATOMIC_ACQUIRE);
}

/* ------------------------------------------------------------------ */
/* Helpers (intentionally mirrors src/backends/rpc/zephyr_drv.c)       */
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

static int
frame_build(uint8_t *out, size_t cap, const char *method, const void *payload, size_t payload_len)
{
	size_t method_len = strnlen(method, ALP_RPC_METHOD_MAX_LEN);
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

/* Physical teardown of a channel: close descriptors, destroy the
 * synchronisation primitives, and free the block.  Runs EXACTLY ONCE
 * per channel, from exactly one of two call sites (never both):
 *   - y_close(), synchronously, for an external (non-self) close; or
 *   - rpc_rx_main()'s epilogue, for a channel that closed itself from
 *     inside one of its own subscriber callbacks (GHSA-xhm8-7f87-93q5).
 * Both call sites reach this only after y_close() has already won
 * g_rpc_close_lock's single-owner decision (see y_close()) -- no other
 * thread can still be holding a reference to `ch` at that point, so
 * nothing else can be touching `ch` here. */
static void rpc_be_teardown(struct rpc_be *ch)
{
	if (ch->rx_wake_pipe[0] >= 0) close(ch->rx_wake_pipe[0]);
	if (ch->rx_wake_pipe[1] >= 0) close(ch->rx_wake_pipe[1]);
	if (ch->ept_fd >= 0) close(ch->ept_fd);
	if (ch->ctrl_fd >= 0) close(ch->ctrl_fd);

	pthread_mutex_destroy(&ch->tx_mutex);
	pthread_mutex_destroy(&ch->sub_mutex);
	pthread_cond_destroy(&ch->call_cond);
	pthread_mutex_destroy(&ch->call_mutex);
	free(ch);
}

/* RX worker: poll() the endpoint fd; on inbound frame, parse + dispatch. */
static void *rpc_rx_main(void *arg)
{
	struct rpc_be *ch = (struct rpc_be *)arg;
	uint8_t        buf[ALP_RPC_TX_FRAME_MAX];

	struct pollfd fds[2] = {
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
			/* GHSA-xhm8-7f87-93q5: cb() may call alp_rpc_close on
             * THIS channel (self-close).  y_close() detects that via
             * pthread_equal(pthread_self(), ch->rx_thread), wins single
             * ownership under g_rpc_close_lock, flips rx_run to 0, and
             * returns WITHOUT joining/freeing `ch` -- it defers the
             * physical teardown to the epilogue below, which runs right
             * here once cb() returns and the loop unwinds.  `ch` stays
             * alive for the complete duration of this call. */
			cb(payload, payload_len, user);
		}
	}

	/* If this channel closed itself from inside a callback above,
     * y_close() deferred the physical teardown to here rather than
     * freeing `ch` out from under this very call stack (the original
     * bug).  Finish it now that the poll loop has unwound: no self-join
     * needed -- we ARE the thread that would have been joined -- so
     * detach first (y_close()'s g_rpc_close_lock-guarded single-owner
     * decision guarantees no external caller is racing us for
     * ownership, so nobody will ever try to join this thread) and only
     * then run the shared teardown. */
	if (rpc_be_close_from_worker_load(ch)) {
		pthread_detach(pthread_self());
		rpc_be_teardown(ch);
	}
	return NULL;
}

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

/* ================================================================== */
/* Ops                                                                 */
/* ================================================================== */

/**
 * @brief Open the RPMsg control device, create the named endpoint, and
 *        spawn the per-channel RX worker.
 *
 * Opens /dev/rpmsg_ctrl0 (override via ALP_RPMSG_CTRL_DEV), creates the
 * endpoint with RPMSG_CREATE_EPT_IOCTL, opens the matching /dev/rpmsgN
 * (override via ALP_RPMSG_EPT_DEV), and starts the poll()/read() worker.
 * The RPMsg chardev ABI exposes no queryable capability surface, so
 * caps stay 0.  The OpenAMP / RPMsg calls are preserved verbatim from
 * the original direct-impl.
 */
static alp_status_t
y_open(const alp_rpc_config_t *cfg, alp_rpc_backend_state_t *st, alp_capabilities_t *caps_out)
{
	if (caps_out != NULL) caps_out->flags = 0u;
	if (cfg == NULL || cfg->name == NULL || cfg->name[0] == '\0') {
		return ALP_ERR_INVAL;
	}
	if (strnlen(cfg->name, ALP_RPC_METHOD_MAX_LEN) == ALP_RPC_METHOD_MAX_LEN) {
		return ALP_ERR_INVAL;
	}

	struct rpc_be *ch = (struct rpc_be *)calloc(1, sizeof(*ch));
	if (ch == NULL) {
		return ALP_ERR_NOMEM;
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
	ch->call_pending      = false;
	ch->close_from_worker = false;
	ch->ept_fd            = -1;
	ch->ctrl_fd           = -1;
	ch->rx_wake_pipe[0]   = -1;
	ch->rx_wake_pipe[1]   = -1;

	/* Open the control device + create our endpoint via ioctl. */
	ch->ctrl_fd = open(rpmsg_ctrl_path(), O_RDWR);
	if (ch->ctrl_fd < 0) {
		fprintf(stderr, "alp_rpc: open(%s) failed: %s\n", rpmsg_ctrl_path(), strerror(errno));
		free(ch);
		return ALP_ERR_NOT_READY;
	}

	struct rpmsg_endpoint_info eptinfo;
	memset(&eptinfo, 0, sizeof eptinfo);
	strncpy(eptinfo.name, ch->name, sizeof(eptinfo.name) - 1);
	eptinfo.src = ch->src_ept;
	eptinfo.dst = ch->dst_ept;

	if (ioctl(ch->ctrl_fd, RPMSG_CREATE_EPT_IOCTL, &eptinfo) < 0) {
		fprintf(
		    stderr, "alp_rpc: RPMSG_CREATE_EPT_IOCTL(%s) failed: %s\n", ch->name, strerror(errno));
		close(ch->ctrl_fd);
		free(ch);
		return ALP_ERR_NOT_READY;
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
		free(ch);
		return ALP_ERR_NOT_READY;
	}

	if (pipe(ch->rx_wake_pipe) < 0) {
		fprintf(stderr, "alp_rpc: pipe() failed: %s\n", strerror(errno));
		close(ch->ept_fd);
		close(ch->ctrl_fd);
		free(ch);
		return ALP_ERR_IO;
	}

	atomic_store(&ch->rx_run, 1);
	if (pthread_create(&ch->rx_thread, NULL, rpc_rx_main, ch) != 0) {
		fprintf(stderr, "alp_rpc: pthread_create failed\n");
		close(ch->rx_wake_pipe[0]);
		close(ch->rx_wake_pipe[1]);
		close(ch->ept_fd);
		close(ch->ctrl_fd);
		free(ch);
		return ALP_ERR_IO;
	}

	rpc_be_data_store(st, ch);
	return ALP_OK;
}

/**
 * @brief Subscribe a callback to a named method on this channel.
 *
 * Replaces any prior registration for the same (channel, method) pair;
 * a NULL @p cb removes the registration.  The subscribe table is
 * guarded by the channel's sub_mutex against the RX worker.
 */
static alp_status_t y_unsubscribe(alp_rpc_backend_state_t *st, const char *method);

static alp_status_t
y_subscribe(alp_rpc_backend_state_t *st, const char *method, alp_rpc_method_cb_t cb, void *user)
{
	struct rpc_be *ch = rpc_be_data_load(st);
	if (ch == NULL) return ALP_ERR_NOT_READY;
	if (!method_valid(method)) return ALP_ERR_INVAL;
	/* NULL cb == unsubscribe -- matches the documented behaviour and the
     * original direct-impl, which delegated to alp_rpc_unsubscribe. */
	if (cb == NULL) {
		return y_unsubscribe(st, method);
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

/**
 * @brief Remove a prior @ref y_subscribe registration.
 *
 * @return ALP_ERR_INVAL when no registration matched.
 */
static alp_status_t y_unsubscribe(alp_rpc_backend_state_t *st, const char *method)
{
	struct rpc_be *ch = rpc_be_data_load(st);
	if (ch == NULL) return ALP_ERR_NOT_READY;
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

/**
 * @brief Fire-and-forget send: frame the (method, payload) pair and
 *        write() it to the endpoint fd.
 *
 * TX is serialised by the channel's tx_mutex.  A non-blocking write
 * that would block maps EAGAIN/EWOULDBLOCK -> ALP_ERR_BUSY.
 */
static alp_status_t
y_send(alp_rpc_backend_state_t *st, const char *method, const void *payload, size_t len)
{
	struct rpc_be *ch = rpc_be_data_load(st);
	if (ch == NULL) return ALP_ERR_NOT_READY;
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

/**
 * @brief Synchronous request/response.
 *
 * Stages the per-channel call slot, sends the request frame, then waits
 * on call_cond (pthread_cond_timedwait) until the RX worker routes a
 * matching response, the timeout elapses, or the channel closes.
 * Concurrent calls on one channel are serialised by tx_mutex.
 */
static alp_status_t y_call(alp_rpc_backend_state_t *st,
                           const char              *method,
                           const void              *req,
                           size_t                   req_len,
                           void                    *resp,
                           size_t                  *resp_len,
                           uint32_t                 timeout_ms)
{
	struct rpc_be *ch = rpc_be_data_load(st);
	if (ch == NULL) return ALP_ERR_NOT_READY;
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

/**
 * @brief Tear down the RX worker + chardev fds and free the channel box.
 *
 * Wakes any pending alp_rpc_call with ALP_ERR_NOT_READY, signals the RX
 * worker via the wake pipe, then either joins it synchronously (the
 * normal, external-caller path) or -- when @b this call itself arrived
 * from a subscriber callback running on the channel's OWN rx thread --
 * defers the descriptor/mutex/free teardown to that thread's own
 * `rpc_rx_main()` epilogue instead.
 *
 * @par Callback reentrancy, unsubscribe, and close semantics
 * A subscriber callback (see @ref alp_rpc_method_cb_t) runs ON this
 * channel's rx thread.  It MAY call @ref alp_rpc_unsubscribe /
 * @ref alp_rpc_subscribe on its own channel (sub_mutex already excludes
 * the RX dispatch loop while a callback runs) and it MAY call
 * @ref alp_rpc_close on its own channel -- this is the scenario
 * GHSA-xhm8-7f87-93q5 fixes.  Previously that self-close called
 * `pthread_join()` on its own thread (guaranteed `EDEADLK`, silently
 * ignored), then freed `ch` anyway while `rpc_rx_main()` kept
 * dereferencing it after the callback returned -- a deterministic
 * heap use-after-free.
 *
 * The fix: `pthread_equal(pthread_self(), ch->rx_thread)` detects the
 * self-close.  An external close racing a callback self-close (or two
 * racing calls of any kind) is made single-shot by g_rpc_close_lock (a
 * file-scope mutex, declared next to `struct rpc_be` -- see its own
 * comment for why it can't live inside the per-channel struct): every
 * caller re-reads `st->be_data` while HOLDING that lock and, if it is
 * still non-NULL, claims ownership and nulls it out before releasing
 * the lock, all as one indivisible step.  This is what makes it safe
 * for a caller to dereference `ch` (e.g. `ch->rx_thread` below) at
 * all: an earlier, per-channel-mutex-only version of this fix let a
 * racing "loser" read `ch` fields with no guarantee `ch` hadn't
 * already been freed by a faster winner -- a per-channel mutex can't
 * fix that (locking a mutex that lives INSIDE the block you're
 * checking is only safe if that block is still allocated); a lock
 * that outlives every channel can. ThreadSanitizer caught the gap as a
 * real (if narrow-window) use-after-free. A lock-free pre-check lets
 * an uncontested close (and every other op) skip the lock entirely.
 * The self-close owner never joins or frees -- it returns immediately,
 * leaving `ch` fully alive for the remainder of the callback invocation
 * and the loop unwind; `rpc_rx_main()`'s epilogue (this SAME thread,
 * once the callback returns and the poll loop exits) detaches itself
 * (nothing else may join it -- the single-owner gate guarantees no
 * external caller raced it for ownership) and completes the teardown.
 * An external-owner close is unchanged: wake, signal, `pthread_join()`,
 * then close fds / destroy sync objects / free, all synchronously
 * inside this call.
 */
static void y_close(alp_rpc_backend_state_t *st)
{
	/* Lock-free fast path: a channel that is already fully closed (or
     * was never opened) needs no synchronisation at all to observe
     * NULL here -- rpc_be_data_load()/store() are a real
     * release/acquire pair (see their own comment). */
	if (rpc_be_data_load(st) == NULL) {
		return;
	}

	/* Re-read st->be_data and decide ownership as ONE step under
     * g_rpc_close_lock (see its declaration for why this must be a
     * lock that outlives every channel, not one stored in `ch`).
     * Whichever caller gets the lock first is the single owner: it
     * claims `ch`, records from_worker, and nulls st->be_data out
     * before releasing the lock -- so a second caller that takes the
     * lock next is guaranteed to see st->be_data already NULL and
     * never touches `ch` again. */
	pthread_mutex_lock(&g_rpc_close_lock);
	struct rpc_be *ch = rpc_be_data_load(st);
	if (ch == NULL) {
		pthread_mutex_unlock(&g_rpc_close_lock);
		return; /* a racing caller already claimed + unpublished it */
	}
	bool from_worker = pthread_equal(pthread_self(), ch->rx_thread);
	rpc_be_close_from_worker_store(ch, from_worker);
	rpc_be_data_store(st, NULL);
	pthread_mutex_unlock(&g_rpc_close_lock);

	/* We are the single owner from here on. */

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

	if (from_worker) {
		/* GHSA-xhm8-7f87-93q5: this call arrived from a subscriber
         * callback running on ch->rx_thread -- the channel is closing
         * itself.  Do NOT join (would deadlock: EDEADLK) and do NOT
         * free `ch` here -- the callback that called us is still
         * running on top of this exact memory and `rpc_rx_main()` will
         * keep touching `ch` (the loop-condition check) until it
         * unwinds.  Leave everything alive and return; the epilogue in
         * rpc_rx_main() completes the teardown once the loop exits. */
		return;
	}

	/* External close: preserve the original join-based teardown. */
	int jrc = pthread_join(ch->rx_thread, NULL);
	if (jrc == EDEADLK) {
		/* Structurally unreachable -- `from_worker` above already
         * returned early whenever pthread_self() is ch->rx_thread.  If
         * this ever fires, the thread-identity check misidentified the
         * caller; do NOT repeat the original bug by freeing `ch` out
         * from under a thread that may still be running.  Surface the
         * condition loudly and leak the channel rather than risk a
         * use-after-free. */
		fprintf(stderr,
		        "alp_rpc: BUG: y_close() self-join (EDEADLK) despite the "
		        "from_worker guard; leaking channel %p to avoid a UAF\n",
		        (void *)ch);
		return;
	}
	rpc_be_teardown(ch);
}

#else /* !ALP_SDK_HAVE_OPENAMP_USERLAND */

/* Build-time fallback: no OpenAMP user-space libs available on the
 * host (typical for Windows / macOS dev boxes, or a host-only syntax
 * check).  Compile the ops to NOSUPPORT so the TU still links cleanly
 * and the dispatcher falls through to the SW fallback; a customer who
 * forces this backend still sees a clear runtime error. */

/** @brief NOSUPPORT open() -- no OpenAMP user-space libraries linked. */
static alp_status_t
y_open(const alp_rpc_config_t *cfg, alp_rpc_backend_state_t *st, alp_capabilities_t *caps_out)
{
	(void)cfg;
	(void)st;
	if (caps_out != NULL) caps_out->flags = 0u;
	return ALP_ERR_NOSUPPORT;
}

/** @brief NOSUPPORT subscribe() -- no OpenAMP user-space libraries linked. */
static alp_status_t
y_subscribe(alp_rpc_backend_state_t *st, const char *method, alp_rpc_method_cb_t cb, void *user)
{
	(void)st;
	(void)method;
	(void)cb;
	(void)user;
	return ALP_ERR_NOSUPPORT;
}

/** @brief NOSUPPORT unsubscribe() -- no OpenAMP user-space libraries linked. */
static alp_status_t y_unsubscribe(alp_rpc_backend_state_t *st, const char *method)
{
	(void)st;
	(void)method;
	return ALP_ERR_NOSUPPORT;
}

/** @brief NOSUPPORT send() -- no OpenAMP user-space libraries linked. */
static alp_status_t
y_send(alp_rpc_backend_state_t *st, const char *method, const void *payload, size_t len)
{
	(void)st;
	(void)method;
	(void)payload;
	(void)len;
	return ALP_ERR_NOSUPPORT;
}

/** @brief NOSUPPORT call() -- no OpenAMP user-space libraries linked. */
static alp_status_t y_call(alp_rpc_backend_state_t *st,
                           const char              *method,
                           const void              *req,
                           size_t                   req_len,
                           void                    *resp,
                           size_t                  *resp_len,
                           uint32_t                 timeout_ms)
{
	(void)st;
	(void)method;
	(void)req;
	(void)req_len;
	(void)resp;
	(void)resp_len;
	(void)timeout_ms;
	return ALP_ERR_NOSUPPORT;
}

/** @brief NOSUPPORT close() -- no OpenAMP user-space libraries linked. */
static void y_close(alp_rpc_backend_state_t *st)
{
	(void)st;
}

#endif /* ALP_SDK_HAVE_OPENAMP_USERLAND */

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const alp_rpc_ops_t _ops = {
	.open        = y_open,
	.subscribe   = y_subscribe,
	.unsubscribe = y_unsubscribe,
	.send        = y_send,
	.call        = y_call,
	.close       = y_close,
};

ALP_BACKEND_REGISTER(rpc,
                     yocto_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "linux",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

#endif /* __linux__ */
