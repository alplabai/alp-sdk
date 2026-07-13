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
 * Close protocol (GHSA-xhm8-7f87-93q5) -- authoritative redesign.
 *   Two prior patch rounds each introduced a NEW deadlock (round 1: a
 *   callback self-close self-joined its own rx thread; round 2: a
 *   dispatcher-side CAS -> ops->close() -> drain ordering had no way
 *   for a backend whose close is invoked FROM its own rx thread to
 *   avoid either self-joining or leaving the dispatcher's drain
 *   spinning on an op that can only unblock via a close the dispatcher
 *   has already returned from).  The dispatcher
 *   (src/rpc_dispatch.c) now owns single-owner election (one atomic
 *   CAS) and the active-op drain; this file's half of the contract
 *   (see alp_rpc_ops_t in rpc_ops.h) is:
 *
 *     - y_shutdown(): makes every current AND future blocking y_call()
 *       terminate promptly via a STICKY `closing` flag checked under
 *       call_mutex (replacing the old one-shot cancel, which missed a
 *       call that staged itself AFTER the one-shot cancel had already
 *       run -- see y_call()'s doc comment).  Detects self-close via
 *       pthread_equal(pthread_self(), ch->rx_thread): external ->
 *       joins rx_thread (bounded: the wake-pipe write above guarantees
 *       rpc_rx_main()'s poll() returns promptly) and returns
 *       ALP_RPC_SHUTDOWN_DONE; self -> returns
 *       ALP_RPC_SHUTDOWN_DEFERRED WITHOUT joining (would be a
 *       guaranteed EDEADLK) or touching `ch` further.
 *     - rpc_rx_main()'s epilogue completes the deferred teardown for
 *       the self-close case, exactly once, once the callback that
 *       triggered it has returned and the poll loop has unwound --
 *       calling the dispatcher's alp_rpc_close_finalize(ch->owner).
 *     - y_destroy(): frees fds/mutexes/`ch` itself.  Called exactly
 *       once by the dispatcher (either synchronously from
 *       the public close entry point, for the external/DONE path, or via
 *       alp_rpc_close_finalize(), for the self/DEFERRED path) strictly
 *       after the dispatcher's own active-op count has drained to 0 --
 *       so this file no longer needs its OWN in-flight-op counter
 *       (the pre-redesign inflight_ops / rpc_be_op_enter() /
 *       rpc_be_op_leave() / rpc_be_drain_inflight() machinery, and the
 *       file-scope g_rpc_close_lock single-owner mutex that guarded
 *       it, are gone -- the dispatcher's single atomic CAS + refcount
 *       now provide both the single-owner guarantee and the "don't
 *       free `ch` while an op still holds it" guarantee, from ABOVE
 *       this file).
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
 *         sysroot / no real /dev/rpmsg* nodes in this environment) --
 *         this real close-protocol redesign has NOT been confirmed
 *         end-to-end against a real /dev/rpmsg* chardev on E1M
 *         hardware; only the socketpair-based unit tests in
 *         tests/yocto/rpc_yocto_self_close.c exercise it so far.
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
     * response can ever be in flight here.
     *
     * `closing` (GHSA-xhm8-7f87-93q5 redesign) is the sticky cancel
     * predicate: guarded by this SAME call_mutex (no atomics needed --
     * it lives entirely under its lock), set exactly once by
     * y_shutdown() and never cleared.  y_call() checks it BOTH before
     * staging (so a call that stages itself after y_shutdown() already
     * ran bails immediately instead of waiting out its own timeout
     * with nobody left to signal it -- the exact gap the OLD one-shot
     * cancel + re-cancel-in-drain-loop pattern had) and in its wait
     * loop's condition (so a call already waiting wakes promptly on
     * y_shutdown()'s broadcast). */
	pthread_mutex_t call_mutex;
	pthread_cond_t  call_cond;
	char            call_method[ALP_RPC_METHOD_MAX_LEN];
	void           *call_resp_buf;
	size_t          call_resp_cap;
	size_t          call_resp_len; /* actual response size on the wire */
	alp_status_t    call_result;
	bool            call_pending;
	bool            closing;

	/* Set once by y_shutdown() (see its doc comment), read once by
     * rpc_rx_main()'s epilogue to decide whether THIS channel's single
     * close (the dispatcher guarantees at most one call to
     * y_shutdown() ever runs per channel -- see rpc_dispatch.c's
     * _rpc_begin_close()) was the self-close (DEFERRED) case.  Real
     * release/acquire pair: for a self-close the write and the read
     * are on the very same thread/call-stack (sequenced-before is
     * already enough), but for an EXTERNAL close y_shutdown() runs on
     * a different thread than the one that (if a recv is concurrently
     * in flight on an unrelated message) might read this in
     * rpc_rx_main()'s epilogue -- keep the atomic pair for that case,
     * same as the pre-redesign version of this flag. */
	bool close_from_worker;

	/* Dispatcher-owned back-pointer (alp_rpc_backend_state_t::owner,
     * cached at y_open() time) -- see rpc_ops.h.  Used ONLY by
     * rpc_rx_main()'s epilogue to call alp_rpc_close_finalize(owner)
     * exactly once on the self-close (DEFERRED) path. */
	void *owner;
};

/* alp_rpc_backend_state_t::be_data (rpc_ops.h, dispatcher-owned) is
 * published once by y_open() and never reassigned until y_destroy()
 * frees `ch` -- see rpc_ops.h's alp_rpc_ops_t::destroy doc comment:
 * the dispatcher only ever calls destroy() once every op counted
 * before its close CAS has already left (its own drain, ABOVE this
 * file, in src/rpc_dispatch.c), so no op in this file can ever observe
 * `ch` mid-teardown or after free.  This atomic load/store pair still
 * carries the acquire/release semantics that make "everything y_open()
 * wrote is visible to the first op that reads be_data on another
 * thread" a real, ThreadSanitizer-clean synchronizes-with edge (the
 * generic pointer-publish idiom), even though -- unlike the
 * pre-redesign version of this file -- no op here needs to treat a
 * NULL read as a live, in-flight teardown-vs-op race any more. */
static inline struct rpc_be *rpc_be_data_load(alp_rpc_backend_state_t *st)
{
	return (struct rpc_be *)__atomic_load_n(&st->be_data, __ATOMIC_ACQUIRE);
}

static inline void rpc_be_data_store(alp_rpc_backend_state_t *st, struct rpc_be *ch)
{
	__atomic_store_n(&st->be_data, (void *)ch, __ATOMIC_RELEASE);
}

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
 * synchronisation primitives, and free the block.  Called from y_destroy()
 * -- see that function's doc comment for why this file no longer needs
 * its own in-flight-op drain here (GHSA-xhm8-7f87-93q5 redesign): the
 * dispatcher (src/rpc_dispatch.c) now owns that drain, ABOVE this file,
 * and guarantees destroy() (which routes here) is called exactly once,
 * strictly after every op counted before the close won is gone. */
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
             * THIS channel (self-close).  The dispatcher's single
             * atomic CAS elects it as the one closer, then
             * y_shutdown() detects the self-close via
             * pthread_equal(pthread_self(), ch->rx_thread), flips
             * rx_run to 0, and returns ALP_RPC_SHUTDOWN_DEFERRED
             * WITHOUT joining/freeing `ch` -- the physical teardown is
             * deferred to the epilogue below, which runs right here
             * once cb() returns and the loop unwinds.  `ch` stays
             * alive for the complete duration of this call. */
			cb(payload, payload_len, user);
		}
	}

	/* If this channel closed itself from inside a callback above,
     * y_shutdown() returned ALP_RPC_SHUTDOWN_DEFERRED and set
     * close_from_worker rather than joining/freeing anything itself
     * (the original bug: pthread_join()-ing its own thread, guaranteed
     * EDEADLK).  Finish the deferred teardown now that the poll loop
     * has unwound: no self-join needed -- we ARE the thread that would
     * have been joined -- so detach first (the dispatcher's single-
     * owner CAS, src/rpc_dispatch.c's _rpc_begin_close(), guarantees no
     * external caller is racing us for ownership of THIS close, so
     * nobody will ever try to join this thread) and then hand off to
     * the dispatcher's alp_rpc_close_finalize(), which drains the
     * dispatcher's own active-op count to 0 and calls y_destroy() ->
     * rpc_be_teardown() exactly once. */
	if (rpc_be_close_from_worker_load(ch)) {
		pthread_detach(pthread_self());
		alp_rpc_close_finalize(ch->owner);
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
	ch->closing           = false;
	ch->close_from_worker = false;
	ch->owner             = st->owner;
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
	if (!method_valid(method)) return ALP_ERR_INVAL;
	/* NULL cb == unsubscribe -- matches the documented behaviour and the
     * original direct-impl, which delegated to alp_rpc_unsubscribe. */
	if (cb == NULL) {
		return y_unsubscribe(st, method);
	}

	/* The dispatcher (src/rpc_dispatch.c) only ever calls into this op
     * while its own op-count gate is held, which is guaranteed to stay
     * valid (no concurrent destroy()) until this function returns --
     * see rpc_ops.h's alp_rpc_ops_t::destroy doc comment. */
	struct rpc_be *ch = rpc_be_data_load(st);
	if (ch == NULL) return ALP_ERR_NOT_READY;

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
	if (!method_valid(method)) return ALP_ERR_INVAL;

	struct rpc_be *ch = rpc_be_data_load(st);
	if (ch == NULL) return ALP_ERR_NOT_READY;

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
	if (!method_valid(method)) return ALP_ERR_INVAL;
	if (payload == NULL && len > 0) return ALP_ERR_INVAL;

	struct rpc_be *ch = rpc_be_data_load(st);
	if (ch == NULL) return ALP_ERR_NOT_READY;

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
 *
 * @par Sticky cancel (GHSA-xhm8-7f87-93q5 redesign)
 * `ch->closing` is checked under call_mutex BOTH before staging the
 * call slot and in the wait loop's own condition -- not just a
 * one-shot cancel run once by y_shutdown().  That closes the
 * "late-staging" gap the old one-shot-cancel design had: a call that
 * is delayed (e.g. preempted between the dispatcher's op-count
 * increment and reaching this staging step) until AFTER y_shutdown()
 * has already run and broadcast would, under the old design, stage
 * itself anyway and then wait out its own `timeout_ms` (or forever,
 * for UINT32_MAX) with nobody left to signal it -- the RX thread has
 * already unwound.  Rechecking the STICKY flag here, instead of
 * relying on a signal that already fired, closes that gap
 * unconditionally: `closing` is set exactly once and never cleared,
 * so a call that checks it at ANY point after y_shutdown() ran always
 * observes it.
 */

/* Test-only late-staging hook (default no-op; a single NULL-check
 * branch in production).  A test that #includes this .c file directly
 * (see tests/yocto/rpc_yocto_self_close.c) can point this at a
 * function that sleeps, to widen the window between "the dispatcher's
 * op-count already counts this call" and "this function actually
 * stages the call slot" enough to deterministically land a concurrent
 * close in between -- proving the sticky `closing` recheck below
 * catches it where the old one-shot cancel did not. */
static void (*g_y_call_test_late_staging_hook)(void) = NULL;

static alp_status_t y_call(alp_rpc_backend_state_t *st,
                           const char              *method,
                           const void              *req,
                           size_t                   req_len,
                           void                    *resp,
                           size_t                  *resp_len,
                           uint32_t                 timeout_ms)
{
	if (!method_valid(method)) return ALP_ERR_INVAL;
	if (req == NULL && req_len > 0) return ALP_ERR_INVAL;
	if (resp != NULL && resp_len == NULL) return ALP_ERR_INVAL;

	struct rpc_be *ch = rpc_be_data_load(st);
	if (ch == NULL) return ALP_ERR_NOT_READY;

	if (g_y_call_test_late_staging_hook != NULL) {
		g_y_call_test_late_staging_hook();
	}

	/* Serialise calls on this channel (matches the Zephyr backend's
     * tx_mutex + single call-slot model).  Customers needing
     * concurrent in-flight requests open multiple channels. */
	pthread_mutex_lock(&ch->tx_mutex);

	/* Check-then-stage as ONE call_mutex critical section: a call that
     * sees `closing` bails immediately, before ever setting
     * call_pending, so y_shutdown()'s single cancel-and-broadcast pass
     * never needs to run again for a call staged after it. */
	pthread_mutex_lock(&ch->call_mutex);
	if (ch->closing) {
		pthread_mutex_unlock(&ch->call_mutex);
		pthread_mutex_unlock(&ch->tx_mutex);
		return ALP_ERR_NOT_READY;
	}
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
		/* Unbounded wait: pthread_cond_wait until pending clears or the
         * channel starts closing. */
		while (ch->call_pending && !ch->closing) {
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
		while (ch->call_pending && !ch->closing) {
			rc = pthread_cond_timedwait(&ch->call_cond, &ch->call_mutex, &deadline);
			if (rc != 0) break;
		}
	}

	if (ch->closing) {
		/* Sticky cancel wins regardless of how we got here (a normal
         * wake from y_shutdown()'s broadcast, or the late-staging case
         * where `closing` was already true the moment we looked) --
         * see this function's doc comment. */
		ch->call_pending = false;
		s                = ALP_ERR_NOT_READY;
	} else if (rc == ETIMEDOUT) {
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
 * @brief Make every blocking wait on this channel terminate promptly,
 *        and report whether THIS call itself arrived from the
 *        channel's own rx thread.
 *
 * Sets the sticky `closing` predicate (see y_call()'s doc comment),
 * cancels any pending call, and wakes the RX worker via the wake pipe.
 * Then either joins the RX worker synchronously (the external-caller
 * path -- bounded: rx_run=0 plus the wake-pipe write above guarantee
 * rpc_rx_main()'s poll() returns promptly) and returns
 * @ref ALP_RPC_SHUTDOWN_DONE, or -- when @b this call itself arrived
 * from a subscriber callback running on the channel's OWN rx thread --
 * returns @ref ALP_RPC_SHUTDOWN_DEFERRED WITHOUT joining or touching
 * `ch` further.
 *
 * @par Self-close detection (GHSA-xhm8-7f87-93q5)
 * A subscriber callback (see @ref alp_rpc_method_cb_t) runs ON this
 * channel's rx thread and MAY call @ref alp_rpc_close on its own
 * channel -- this is the scenario GHSA-xhm8-7f87-93q5 fixes.  Two
 * prior patch rounds each got this wrong (round 1: `pthread_join()`ed
 * its own thread -- guaranteed `EDEADLK`, silently ignored -- then
 * freed `ch` anyway while `rpc_rx_main()` kept dereferencing it after
 * the callback returned, a deterministic heap use-after-free; round 2:
 * moved the whole cancel+join+teardown sequence to the DISPATCHER,
 * which then had no way to avoid EITHER self-joining OR spinning
 * forever draining an op -- a blocked synchronous-call op -- that only this
 * very shutdown() call can unblock).
 *
 * `pthread_equal(pthread_self(), ch->rx_thread)` detects the
 * self-close.  Single-owner-ness (so it is safe to dereference `ch` at
 * all here, and so this function runs AT MOST ONCE per channel) is now
 * guaranteed one layer up, by the dispatcher's single atomic CAS
 * (src/rpc_dispatch.c's `_rpc_begin_close()`) -- this file no longer
 * needs its own g_rpc_close_lock/ownership race at all.  The self-close
 * case returns DEFERRED without joining or freeing -- `ch` stays fully
 * alive for the remainder of the callback invocation and the loop
 * unwind; `rpc_rx_main()`'s epilogue (this SAME thread, once the
 * callback returns and the poll loop exits) detaches itself (nothing
 * else may ever join it -- the dispatcher's CAS guarantees no external
 * caller raced it for ownership of THIS close) and hands off to
 * alp_rpc_close_finalize(), which completes the teardown exactly once.
 */
static alp_rpc_shutdown_result_t y_shutdown(alp_rpc_backend_state_t *st)
{
	struct rpc_be *ch = rpc_be_data_load(st);
	if (ch == NULL) {
		return ALP_RPC_SHUTDOWN_DONE; /* nothing to shut down */
	}

	bool from_worker = pthread_equal(pthread_self(), ch->rx_thread);
	rpc_be_close_from_worker_store(ch, from_worker);

	/* Sticky cancel: set `closing` and wake any pending call under
     * call_mutex (mandatory: synchronises with a caller about to enter
     * pthread_cond_timedwait, and with y_call()'s own check-then-stage
     * critical section -- see y_call()'s doc comment). */
	pthread_mutex_lock(&ch->call_mutex);
	ch->closing = true;
	if (ch->call_pending) {
		ch->call_result   = ALP_ERR_NOT_READY;
		ch->call_resp_len = 0;
		ch->call_pending  = false;
	}
	pthread_cond_broadcast(&ch->call_cond);
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
         * itself.  Do NOT join (would deadlock: EDEADLK).  Every
         * blocking wait is already unblocked above; the rx-epilogue in
         * rpc_rx_main() completes the deferred teardown once the loop
         * exits -- see this function's doc comment. */
		return ALP_RPC_SHUTDOWN_DEFERRED;
	}

	/* External close: join the RX worker synchronously before
     * returning DONE -- the dispatcher's contract (see
     * alp_rpc_shutdown_result_t in rpc_ops.h) requires that no backend
     * thread/callback can still touch dispatcher-visible state once
     * DONE is returned. */
	int jrc = pthread_join(ch->rx_thread, NULL);
	if (jrc == EDEADLK) {
		/* Structurally unreachable -- `from_worker` above already
         * returned early whenever pthread_self() is ch->rx_thread.  If
         * this ever fires, the thread-identity check misidentified the
         * caller; do NOT repeat the original bug by proceeding to
         * destroy `ch` while a thread that may still be running
         * references it.  Surface the condition loudly and report
         * DEFERRED so the dispatcher does NOT drain/destroy -- this
         * leaks the channel rather than risking a use-after-free. */
		fprintf(stderr,
		        "alp_rpc: BUG: y_shutdown() self-join (EDEADLK) despite the "
		        "from_worker guard; leaking channel %p to avoid a UAF\n",
		        (void *)ch);
		return ALP_RPC_SHUTDOWN_DEFERRED;
	}
	return ALP_RPC_SHUTDOWN_DONE;
}

/**
 * @brief Free the RX worker's chardev fds, sync primitives, and the
 *        channel box itself.
 *
 * Called exactly once by the dispatcher (src/rpc_dispatch.c), strictly
 * after y_shutdown() has run AND every op counted before the close won
 * has left (the dispatcher's own drain) -- either synchronously from
 * the public close entry point (the ALP_RPC_SHUTDOWN_DONE / external-close path) or
 * via alp_rpc_close_finalize() (the ALP_RPC_SHUTDOWN_DEFERRED /
 * self-close path, from rpc_rx_main()'s epilogue).  Must not block --
 * see alp_rpc_ops_t::destroy's doc comment in rpc_ops.h -- and indeed
 * cannot: by the time this runs, the RX worker is already
 * joined-and-exited (external path) or IS the very thread calling this
 * (self path, where it is about to return from rpc_rx_main() anyway).
 */
static void y_destroy(alp_rpc_backend_state_t *st)
{
	struct rpc_be *ch = rpc_be_data_load(st);
	if (ch == NULL) {
		return;
	}
	rpc_be_data_store(st, NULL);
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

/** @brief NOSUPPORT shutdown() -- no OpenAMP user-space libraries linked;
 *         no rx thread exists, so a self-close is impossible -- always
 *         DONE. */
static alp_rpc_shutdown_result_t y_shutdown(alp_rpc_backend_state_t *st)
{
	(void)st;
	return ALP_RPC_SHUTDOWN_DONE;
}

/** @brief NOSUPPORT destroy() -- no OpenAMP user-space libraries linked. */
static void y_destroy(alp_rpc_backend_state_t *st)
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
	.shutdown    = y_shutdown,
	.destroy     = y_destroy,
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
