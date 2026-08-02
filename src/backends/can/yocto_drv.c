/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Real Linux/Yocto can_* driver-class backend.  Binds the alp_can
 * dispatcher's ops vtable to SocketCAN: a PF_CAN raw socket
 * (CAN_RAW) bound to the canN network interface resolved via
 * ioctl(SIOCGIFINDEX).  Frames map alp_can_frame_t <-> struct
 * can_frame / struct canfd_frame.  Registered at priority 100 with
 * vendor "linux"; the sw_fallback backend (priority 0) still wins on
 * non-Linux native_sim builds where this TU compiles to an empty
 * object.
 *
 * Selected on any silicon (silicon_ref "*") because the SocketCAN
 * ABI is SoC-agnostic: the device-tree / kernel CAN driver decides
 * which physical controller backs canN.
 *
 * STATUS: real impl, Yocto-link + on-target run BENCH-UNVERIFIED (no
 *         sysroot / no real CAN device nodes in this environment).
 *
 * SocketCAN scope notes (NOT faked -- these are genuine ABI limits):
 *   - Bitrate / mode (classic vs FD) and bring-up of the interface are
 *     configured out-of-band via the netlink RTNL link layer ("ip link
 *     set canN type can bitrate ... [fd on] / set up").  A CAN_RAW
 *     socket cannot set the bitrate; cfg->bitrate_* are accepted and
 *     recorded but applied by the platform's network configuration, not
 *     here.  We DO request CAN_RAW_FD_FRAMES when the caller asks for FD
 *     so the socket can carry 64-byte canfd_frames.
 *   - start()/stop() correspond to interface up/down, which is an
 *     administrative netlink operation, not a raw-socket capability.
 *     We treat the socket as the bus and make start()/stop() no-ops
 *     (the interface is expected to be up); see y_start/y_stop.
 *   - Asynchronous RX dispatch uses a per-handle reader thread (pthread)
 *     that read()s frames and invokes the matching alp_can_rx_cb_t.
 *     Kernel-side filtering uses setsockopt(CAN_RAW_FILTER).
 *   - cfg->loopback ("local self-test mode") maps to the per-socket
 *     CAN_RAW_LOOPBACK + CAN_RAW_RECV_OWN_MSGS options: frames this
 *     handle sends are looped back to its own RX path so installed
 *     filters see them.  NOTE: unlike a controller loopback mode
 *     (Zephyr CAN_MODE_LOOPBACK), the frames still go out on the wire;
 *     bus-off isolation would need the netlink "ip link ... loopback on"
 *     controller mode, which a CAN_RAW socket cannot set (issue #246).
 *
 * @par Issue #756 -- RX-thread callback self-close
 * The RX reader thread used to invoke filter callbacks WHILE HOLDING
 * d->lock (see the old _dispatch_rx()), so a callback that called
 * alp_can_close()/alp_can_remove_filter() on its own handle deadlocked
 * re-locking that same mutex on the same thread -- and even past that,
 * the old single-phase y_close() would pthread_join() the RX thread,
 * which IS the calling thread on a self-close (guaranteed EDEADLK).
 * Both are now fixed together (the issue's own warning: fixing only
 * the mutex is insufficient):
 *   - _dispatch_rx() now snapshots matching (cb, user) pairs under
 *     d->lock, then invokes every callback AFTER releasing it, so a
 *     callback-triggered add/remove_filter or close() can freely
 *     re-take d->lock without self-deadlocking.
 *   - y_close() is split into y_shutdown()/y_destroy() (can_ops.h),
 *     mirroring the RPC backend's GHSA-xhm8-7f87-93q5 redesign:
 *     y_shutdown() detects self-vs-external via
 *     pthread_equal(pthread_self(), d->rx_thread) and reports which;
 *     an external close joins the RX thread synchronously (DONE); a
 *     self-close returns DEFERRED WITHOUT joining -- the RX thread's
 *     own epilogue (in _rx_loop(), once the read loop unwinds) detaches
 *     itself and calls alp_can_close_finalize() exactly once.
 *   - _rx_loop() now poll()s the socket fd alongside a wake-pipe
 *     (rx_wake_pipe) instead of blocking directly in read() -- an
 *     earlier version of this fix cancelled the blocking read() by
 *     close()ing d->fd from y_shutdown(), which ThreadSanitizer's
 *     Yocto coverage (this issue's own acceptance criteria) correctly
 *     flagged: close()ing an fd while ANOTHER thread may still be
 *     blocked reading that same fd number is a real hazard (a
 *     concurrently-opened, unrelated fd could reuse the number the
 *     instant it is closed), not just a benign warning.  The wake-pipe
 *     is the exact mechanism src/backends/rpc/yocto_drv.c already uses
 *     for the identical reason; y_shutdown() only ever writes to the
 *     wake-pipe end (never touches d->fd), and d->fd is closed exactly
 *     once, in y_destroy(), strictly after the RX thread has already
 *     exited (either joined -- external path -- or IS the thread
 *     calling this, about to return -- self-close path).
 */

#if defined(__linux__)

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <net/if.h>

#include <linux/can.h>
#include <linux/can/raw.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/can.h>

#include "can_ops.h"
#include "common/alp_errno.h"

#define ALP_Y_CAN_MAX_FILTERS 16

typedef struct {
	alp_can_rx_cb_t   cb;
	void             *user;
	struct can_filter kf; /* kernel filter installed for this slot */
	bool              in_use;
} y_can_filter_t;

/* Per-handle backend data: the open CAN raw socket plus the RX reader
 * thread + its filter table.  Boxed onto the heap so the void* be_data
 * slot in alp_can_backend_state_t owns it. */
typedef struct {
	int             fd;
	bool            fd_frames; /* CAN_RAW_FD_FRAMES enabled */
	pthread_t       rx_thread;
	bool            rx_running;
	pthread_mutex_t lock; /* guards filters[] */
	y_can_filter_t  filters[ALP_Y_CAN_MAX_FILTERS];

	/* Close-side wake notification for _rx_loop()'s poll() (issue
	 * #756) -- see this file's header comment for why this replaced a
	 * direct close(d->fd) as the cancellation mechanism. */
	int rx_wake_pipe[2];

	/* Issue #756: set by y_shutdown() (see its doc comment), read once
	 * by _rx_loop()'s epilogue to decide whether THIS handle's single
	 * close (the dispatcher guarantees at most one shutdown() call ever
	 * runs per handle -- see can_dispatch.c's alp_can_close()) was the
	 * self-close (DEFERRED) case.  Atomic: the external-close path
	 * writes this from a DIFFERENT thread than the RX thread that reads
	 * it in its epilogue. */
	bool close_from_worker;

	/* Dispatcher-owned back-pointer (alp_can_backend_state_t::owner,
	 * cached at y_open() time) -- see can_ops.h.  Used ONLY by
	 * _rx_loop()'s epilogue to call alp_can_close_finalize(owner)
	 * exactly once on the self-close (DEFERRED) path. */
	void *owner;
} y_can_data_t;

static inline void y_can_close_from_worker_store(y_can_data_t *d, bool from_worker)
{
	__atomic_store_n(&d->close_from_worker, from_worker, __ATOMIC_RELEASE);
}

static inline bool y_can_close_from_worker_load(y_can_data_t *d)
{
	return __atomic_load_n(&d->close_from_worker, __ATOMIC_ACQUIRE);
}

/*
 * CAN-specific overrides on top of the shared @ref
 * alp_status_from_posix_errno baseline (#630):
 *
 *   - EAGAIN -> ALP_ERR_TIMEOUT, not the baseline's ALP_ERR_BUSY.  This
 *     backend's socket()/ioctl()/setsockopt() calls are synchronous
 *     control-plane requests, not queue-full producer/consumer ops, so
 *     an EAGAIN here means the kernel couldn't complete the request in
 *     the current call and the caller should treat it like a deadline
 *     rather than an immediate bus-busy signal -- matching the existing
 *     ENOBUFS ("TX queue full") -> ALP_ERR_TIMEOUT special case a few
 *     lines below in y_send().
 *   - ENOPROTOOPT -> ALP_ERR_NOSUPPORT: a SocketCAN-only errno (rejected
 *     setsockopt level/option, e.g. CAN_RAW_FD_FRAMES on a kernel/iface
 *     without CAN-FD) with no equivalent in the shared baseline.
 */
static const alp_errno_override_t _can_errno_overrides[] = {
	{ EAGAIN, ALP_ERR_TIMEOUT },
	{ ENOPROTOOPT, ALP_ERR_NOSUPPORT },
};

static alp_status_t _errno_to_alp(int err)
{
	return alp_status_from_posix_errno_ex(
	    err, _can_errno_overrides, sizeof(_can_errno_overrides) / sizeof(_can_errno_overrides[0]));
}

/* Test-only setsockopt(CAN_RAW_FILTER) interception (default NULL: real
 * setsockopt()) -- dev-review follow-up on issue #756.  Lets
 * tests/yocto/can_add_filter_close_race.c drive the REAL y_add_filter()
 * (needed to reproduce its lazy RX-thread-spawn timing) over a
 * socketpair stand-in for the SocketCAN fd, which cannot accept a real
 * CAN_RAW_FILTER setsockopt().  Mirrors
 * src/yocto/peripheral_gpio.c's g_gpio_test_set_config_hook idiom. */
static int (*g_can_test_apply_filters_hook)(y_can_data_t            *d,
                                            const struct can_filter *set,
                                            size_t                   n) = NULL;

/* Reinstall the union of all active kernel filters on the socket.
 * SocketCAN's CAN_RAW_FILTER takes the whole array at once; we rebuild
 * it from the slot table after any add/remove.  Caller holds d->lock. */
static int _apply_filters_locked(y_can_data_t *d)
{
	struct can_filter set[ALP_Y_CAN_MAX_FILTERS];
	size_t            n = 0;
	for (size_t i = 0; i < ALP_Y_CAN_MAX_FILTERS; ++i) {
		if (d->filters[i].in_use) set[n++] = d->filters[i].kf;
	}
	if (g_can_test_apply_filters_hook != NULL) {
		return g_can_test_apply_filters_hook(d, n ? set : NULL, n);
	}
	/* n==0 leaves an empty filter set: the kernel then drops all RX,
     * which matches "no filters installed -> deliver nothing". */
	if (setsockopt(
	        d->fd, SOL_CAN_RAW, CAN_RAW_FILTER, n ? set : NULL, (socklen_t)(n * sizeof(set[0]))) <
	    0) {
		return errno;
	}
	return 0;
}

/* Decode a received frame buffer (can_frame or canfd_frame) into the
 * portable alp_can_frame_t and dispatch it to any matching slot cb.
 * nbytes is the number of bytes read() returned. */
static void _dispatch_rx(y_can_data_t *d, const void *buf, ssize_t nbytes)
{
	alp_can_frame_t out;
	memset(&out, 0, sizeof(out));

	if ((size_t)nbytes >= sizeof(struct canfd_frame) && d->fd_frames) {
		const struct canfd_frame *cf = (const struct canfd_frame *)buf;
		out.id                       = cf->can_id & CAN_EFF_MASK;
		out.ext_id                   = (cf->can_id & CAN_EFF_FLAG) != 0;
		out.rtr                      = false; /* canfd_frame has no RTR (FD forbids RTR) */
		out.fd                       = true;
		out.brs                      = (cf->flags & CANFD_BRS) != 0;
		out.payload_len              = cf->len;
		if (out.payload_len > sizeof(out.data)) out.payload_len = sizeof(out.data);
		memcpy(out.data, cf->data, out.payload_len);
	} else if ((size_t)nbytes >= sizeof(struct can_frame)) {
		const struct can_frame *cf = (const struct can_frame *)buf;
		out.id                     = cf->can_id & CAN_EFF_MASK;
		out.ext_id                 = (cf->can_id & CAN_EFF_FLAG) != 0;
		out.rtr                    = (cf->can_id & CAN_RTR_FLAG) != 0;
		out.fd                     = false;
		out.brs                    = false;
		out.payload_len            = cf->can_dlc;
		if (out.payload_len > ALP_CAN_MAX_PAYLOAD_BYTES_CLASSIC)
			out.payload_len = ALP_CAN_MAX_PAYLOAD_BYTES_CLASSIC;
		memcpy(out.data, cf->data, out.payload_len);
	} else {
		return; /* short / malformed read */
	}

	/* Kernel-side filtering already gated delivery; fan out to every
     * installed slot cb so software multiplexing matches the alp_can
     * "dispatch matching frames" contract.
     *
     * out.id carries CAN_EFF_FLAG stripped (CAN_EFF_MASK above), but the
     * stored kf.can_id/can_mask re-add CAN_EFF_FLAG for ext-ID slots (see
     * y_add_filter).  Re-apply the EFF flag to the comparison id for
     * extended frames so bit 31 lines up; otherwise every 29-bit slot
     * would mismatch on bit 31 and never fire. */
	canid_t cmp = out.id | (out.ext_id ? CAN_EFF_FLAG : 0u);

	/* Issue #756: snapshot the matching (cb, user) pairs UNDER d->lock,
     * then invoke every callback AFTER releasing it.  A filter cb may
     * call alp_can_close()/alp_can_remove_filter() on this very handle
     * (self-close/self-remove), each of which needs d->lock -- holding
     * it across the callback (the pre-#756 behaviour) deadlocked that
     * re-lock.  Value-copying cb+user here (not dereferencing `f` again
     * later) means a callback that removes/reuses a LATER slot in this
     * same snapshot cannot invalidate an entry already copied out. */
	struct {
		alp_can_rx_cb_t cb;
		void           *user;
	} matches[ALP_Y_CAN_MAX_FILTERS];
	size_t n = 0;

	pthread_mutex_lock(&d->lock);
	for (size_t i = 0; i < ALP_Y_CAN_MAX_FILTERS; ++i) {
		y_can_filter_t *f = &d->filters[i];
		if (!f->in_use || f->cb == NULL) continue;
		if ((cmp & f->kf.can_mask) == (f->kf.can_id & f->kf.can_mask)) {
			matches[n].cb   = f->cb;
			matches[n].user = f->user;
			++n;
		}
	}
	pthread_mutex_unlock(&d->lock);

	for (size_t i = 0; i < n; ++i) {
		matches[i].cb(&out, matches[i].user);
	}
}

/**
 * @brief RX reader thread: poll() the socket + wake-pipe until told to
 *        stop (issue #756 -- see this file's header comment for why
 *        this is poll()-driven rather than a directly-cancelled
 *        blocking read()).
 *
 * Capturing `fd`/`wake_fd` ONCE, into locals, before the loop (rather
 * than re-reading d->fd on every iteration) is safe here specifically
 * because neither is ever written again after y_open() publishes them
 * -- y_shutdown() only ever writes to the wake-pipe's WRITE end
 * (rx_wake_pipe[1]), never to d->fd or rx_wake_pipe[0] themselves, so
 * there is no concurrent writer for this thread's reads of these two
 * fields to race against (unlike the pre-redesign direct-close
 * approach, where d->fd itself was mutated from the closing thread).
 */
static void *_rx_loop(void *arg)
{
	y_can_data_t *d       = (y_can_data_t *)arg;
	int           fd      = d->fd;
	int           wake_fd = d->rx_wake_pipe[0];

	struct pollfd fds[2] = {
		{ .fd = fd, .events = POLLIN },
		{ .fd = wake_fd, .events = POLLIN },
	};

	/* A canfd_frame buffer is a superset of can_frame, so one buffer
     * serves both; read() returns the actual frame length. */
	struct canfd_frame frame;
	for (;;) {
		fds[0].revents = 0;
		fds[1].revents = 0;
		int rc         = poll(fds, 2, -1);
		if (rc < 0) {
			if (errno == EINTR) continue;
			break; /* fatal poll() error -> stop */
		}
		if (fds[1].revents & POLLIN) {
			break; /* close-side wake notification (issue #756) */
		}
		if (!(fds[0].revents & POLLIN)) {
			continue;
		}

		ssize_t n = read(fd, &frame, sizeof(frame));
		if (n < 0) {
			if (errno == EINTR) continue;
			break; /* fatal read() error -> stop */
		}
		if (n == 0) break;
		_dispatch_rx(d, &frame, n);
	}

	/* Issue #756: if a filter callback above closed THIS handle from
     * inside this very thread (self-close), y_shutdown() returned
     * ALP_CAN_SHUTDOWN_DEFERRED and set close_from_worker instead of
     * joining/destroying anything itself (the original bug:
     * pthread_join()-ing its own thread, guaranteed EDEADLK).  Finish
     * the deferred teardown now that the read loop has unwound: detach
     * first (the dispatcher's single-owner CAS guarantees no external
     * caller is racing us for ownership of this close, so nobody will
     * ever try to join this thread), then hand off to the dispatcher's
     * alp_can_close_finalize(), which calls y_destroy() exactly once --
     * active_ops was already drained by the alp_can_close() call that
     * got this DEFERRED result back, BEFORE it ever called y_shutdown()
     * (see can_dispatch.c's alp_can_close() comment), so no drain
     * happens here. */
	if (y_can_close_from_worker_load(d)) {
		pthread_detach(pthread_self());
		alp_can_close_finalize(d->owner);
	}
	return NULL;
}

/**
 * @brief Open a CAN_RAW socket bound to canN and stash it in the handle.
 *
 * Resolves the "can<bus_id>" interface ifindex via SIOCGIFINDEX and
 * binds an AF_CAN raw socket to it.  For ALP_CAN_MODE_FD the socket is
 * switched to CAN_RAW_FD_FRAMES so it can carry 64-byte canfd_frames;
 * if the kernel/interface lacks FD support the setsockopt fails and we
 * surface it as ALP_ERR_NOSUPPORT.  For cfg->loopback the socket opts
 * CAN_RAW_LOOPBACK + CAN_RAW_RECV_OWN_MSGS are BOTH enabled so this
 * handle's own TX frames come back through its RX filters (local
 * self-test; see the file-header scope note on wire semantics).
 * Bitrate is configured out-of-band (see file header) and is recorded
 * by the dispatcher snapshot, not applied here.  caps stay 0
 * (SocketCAN exposes no queryable cap bits).
 */
static alp_status_t
y_open(const alp_can_config_t *cfg, alp_can_backend_state_t *st, alp_capabilities_t *caps_out)
{
	if (cfg == NULL) return ALP_ERR_INVAL;

	char ifname[IFNAMSIZ];
	int  k = snprintf(ifname, sizeof(ifname), "can%u", (unsigned)cfg->bus_id);
	if (k < 0 || (size_t)k >= sizeof(ifname)) return ALP_ERR_INVAL;

	int fd = socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW);
	if (fd < 0) return _errno_to_alp(errno);

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	/* ifr_name is IFNAMSIZ; ifname already fits (checked above). */
	memcpy(ifr.ifr_name, ifname, (size_t)k + 1u);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
		int e = errno;
		close(fd);
		return _errno_to_alp(e);
	}

	bool want_fd = (cfg->mode == ALP_CAN_MODE_FD);
	if (want_fd) {
		int on = 1;
		if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &on, sizeof(on)) < 0) {
			int e = errno;
			close(fd);
			/* Interface / kernel without CAN-FD -> NOSUPPORT, honouring
             * the alp_can_open contract for "CAN-FD requested but
             * unsupported". */
			return _errno_to_alp(e);
		}
	}

	/* Local self-test mode: loop our own TX frames back into this
	 * socket's RX queue.  CAN_RAW_LOOPBACK (default on) makes sent
	 * frames visible to local sockets at all; CAN_RAW_RECV_OWN_MSGS
	 * (default off) additionally delivers them to the SENDING socket,
	 * which is what "the handle receives what it sends" needs.  Both
	 * are set explicitly so the mode does not depend on kernel
	 * defaults.  Silently ignoring cfg->loopback was issue #246. */
	if (cfg->loopback) {
		int on = 1;
		if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_LOOPBACK, &on, sizeof(on)) < 0 ||
		    setsockopt(fd, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &on, sizeof(on)) < 0) {
			int e = errno;
			close(fd);
			return _errno_to_alp(e);
		}
	}

	struct sockaddr_can addr;
	memset(&addr, 0, sizeof(addr));
	addr.can_family  = AF_CAN;
	addr.can_ifindex = ifr.ifr_ifindex;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		int e = errno;
		close(fd);
		return _errno_to_alp(e);
	}

	y_can_data_t *d = (y_can_data_t *)calloc(1, sizeof(*d));
	if (d == NULL) {
		close(fd);
		return ALP_ERR_NOMEM;
	}
	d->fd        = fd;
	d->fd_frames = want_fd;
	d->owner     = st->owner; /* issue #756: for alp_can_close_finalize() */
	if (pthread_mutex_init(&d->lock, NULL) != 0) {
		close(fd);
		free(d);
		return ALP_ERR_IO;
	}
	/* Close-side wake pipe for _rx_loop()'s poll() (issue #756) -- see
     * this file's header comment.  Created unconditionally at open()
     * time (cheap: two fds) even though the RX thread only spawns
     * lazily on the first add_filter(), matching
     * src/backends/rpc/yocto_drv.c's identical rx_wake_pipe. */
	if (pipe(d->rx_wake_pipe) != 0) {
		int e = errno;
		pthread_mutex_destroy(&d->lock);
		close(fd);
		free(d);
		return _errno_to_alp(e);
	}

	st->dev         = NULL;
	st->bus_id      = cfg->bus_id;
	st->be_data     = d;
	caps_out->flags = 0u;
	return ALP_OK;
}

/**
 * @brief Bring the bus into the active state.
 *
 * SocketCAN interface up/down is an administrative netlink (RTNL)
 * operation ("ip link set canN up"), not a CAN_RAW-socket capability.
 * The interface is expected to be up before open() bound to it, so a
 * bound socket already implies an active bus; start() is a no-op that
 * returns ALP_OK.  Per-handle async RX begins when the first filter is
 * installed (which spawns the reader thread).
 */
static alp_status_t y_start(alp_can_backend_state_t *st)
{
	y_can_data_t *d = (y_can_data_t *)st->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;
	return ALP_OK;
}

/**
 * @brief Bring the bus out of the active state.
 *
 * Mirror of @ref y_start: interface down is an out-of-band netlink
 * operation, so stop() is a no-op returning ALP_OK.  Frame delivery
 * stops when the handle is closed and the socket torn down.
 */
static alp_status_t y_stop(alp_can_backend_state_t *st)
{
	y_can_data_t *d = (y_can_data_t *)st->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;
	return ALP_OK;
}

/**
 * @brief Transmit a frame by write()ing a can_frame / canfd_frame.
 *
 * Maps alp_can_frame_t into the SocketCAN frame layout: the EFF/RTR
 * flags fold into can_id, the payload into data[].  A blocking write()
 * is used; @p timeout_ms is not separately enforced because SocketCAN
 * has no per-write timeout primitive on a blocking raw socket -- the
 * kernel TX queue accepts or returns ENOBUFS.  ENOBUFS maps to
 * ALP_ERR_TIMEOUT (TX queue full / no slot), matching the contract.
 */
static alp_status_t
y_send(alp_can_backend_state_t *st, const alp_can_frame_t *frame, uint32_t timeout_ms)
{
	(void)timeout_ms; /* see doxygen: no raw-socket per-write timeout */
	y_can_data_t *d = (y_can_data_t *)st->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;
	if (frame == NULL) return ALP_ERR_INVAL;

	canid_t can_id = frame->id & (frame->ext_id ? CAN_EFF_MASK : CAN_SFF_MASK);
	if (frame->ext_id) can_id |= CAN_EFF_FLAG;

	ssize_t wr;
	if (frame->fd) {
		if (!d->fd_frames) return ALP_ERR_NOSUPPORT;
		if (frame->payload_len > ALP_CAN_MAX_PAYLOAD_BYTES_FD) return ALP_ERR_INVAL;
		struct canfd_frame cf;
		memset(&cf, 0, sizeof(cf));
		cf.can_id = can_id; /* FD frames never carry RTR */
		cf.len    = frame->payload_len;
		cf.flags  = (uint8_t)(frame->brs ? CANFD_BRS : 0);
		memcpy(cf.data, frame->data, frame->payload_len);
		wr = write(d->fd, &cf, sizeof(cf));
		if (wr == (ssize_t)sizeof(cf)) return ALP_OK;
	} else {
		if (frame->payload_len > ALP_CAN_MAX_PAYLOAD_BYTES_CLASSIC) return ALP_ERR_INVAL;
		if (frame->rtr) can_id |= CAN_RTR_FLAG;
		struct can_frame cf;
		memset(&cf, 0, sizeof(cf));
		cf.can_id  = can_id;
		cf.can_dlc = frame->payload_len;
		memcpy(cf.data, frame->data, frame->payload_len);
		wr = write(d->fd, &cf, sizeof(cf));
		if (wr == (ssize_t)sizeof(cf)) return ALP_OK;
	}

	if (wr < 0) {
		if (errno == ENOBUFS) return ALP_ERR_TIMEOUT; /* TX queue full */
		return _errno_to_alp(errno);
	}
	return ALP_ERR_IO; /* short write */
}

/* Test-only hook (dev-review follow-up on issue #756): fires at the
 * very top of y_add_filter(), BEFORE it takes d->lock -- lets a test
 * deterministically stall the FIRST-EVER add_filter() call on a handle
 * (the one that lazily spawns the RX thread below) so a concurrent
 * alp_can_close() racing it can be driven through the exact window
 * dev-review flagged: an add_filter() already counted in the
 * dispatcher's active_ops, but not yet far enough along to have set
 * d->rx_running.  See tests/yocto/can_add_filter_close_race.c.  NULL
 * in production (matches g_y_call_test_late_staging_hook's existing
 * idiom in src/backends/rpc/yocto_drv.c). */
static void (*g_can_test_add_filter_prespawn_hook)(void) = NULL;

/**
 * @brief Install an RX filter and dispatch matching frames to @p cb.
 *
 * Reserves a slot, builds a struct can_filter (folding ext_id into the
 * EFF flag), reapplies the union of active filters via
 * setsockopt(CAN_RAW_FILTER), and -- on first filter -- spawns the
 * per-handle reader thread.  The slot index is the opaque filter id.
 */
static alp_status_t y_add_filter(alp_can_backend_state_t *st,
                                 const alp_can_filter_t  *filter,
                                 alp_can_rx_cb_t          cb,
                                 void                    *user,
                                 int32_t                 *filter_id_out)
{
	y_can_data_t *d = (y_can_data_t *)st->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;
	if (filter == NULL || cb == NULL) return ALP_ERR_INVAL;

	if (g_can_test_add_filter_prespawn_hook != NULL) {
		g_can_test_add_filter_prespawn_hook();
	}

	pthread_mutex_lock(&d->lock);

	int slot = -1;
	for (int i = 0; i < ALP_Y_CAN_MAX_FILTERS; ++i) {
		if (!d->filters[i].in_use) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		pthread_mutex_unlock(&d->lock);
		return ALP_ERR_NOMEM;
	}

	y_can_filter_t *f = &d->filters[slot];
	f->cb             = cb;
	f->user           = user;
	f->kf.can_id      = filter->id & (filter->ext_id ? CAN_EFF_MASK : CAN_SFF_MASK);
	f->kf.can_mask    = filter->mask;
	if (filter->ext_id) {
		f->kf.can_id |= CAN_EFF_FLAG;
		f->kf.can_mask |= CAN_EFF_FLAG; /* require EFF frames to match */
	}
	f->in_use = true;

	int e = _apply_filters_locked(d);
	if (e != 0) {
		*f = (y_can_filter_t){ 0 };
		pthread_mutex_unlock(&d->lock);
		return _errno_to_alp(e);
	}

	/* Lazily start the reader thread on the first installed filter. */
	if (!d->rx_running) {
		if (pthread_create(&d->rx_thread, NULL, _rx_loop, d) != 0) {
			*f = (y_can_filter_t){ 0 };
			(void)_apply_filters_locked(d);
			pthread_mutex_unlock(&d->lock);
			return ALP_ERR_IO;
		}
		d->rx_running = true;
	}

	pthread_mutex_unlock(&d->lock);
	if (filter_id_out != NULL) *filter_id_out = slot;
	return ALP_OK;
}

/**
 * @brief Remove a filter by its slot id and reapply the filter set.
 *
 * @return ALP_ERR_INVAL for an unknown / inactive id.
 */
static alp_status_t y_remove_filter(alp_can_backend_state_t *st, int32_t filter_id)
{
	y_can_data_t *d = (y_can_data_t *)st->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;
	if (filter_id < 0 || filter_id >= ALP_Y_CAN_MAX_FILTERS) {
		return ALP_ERR_INVAL;
	}

	pthread_mutex_lock(&d->lock);
	y_can_filter_t *f = &d->filters[filter_id];
	if (!f->in_use) {
		pthread_mutex_unlock(&d->lock);
		return ALP_ERR_INVAL;
	}
	*f    = (y_can_filter_t){ 0 };
	int e = _apply_filters_locked(d);
	pthread_mutex_unlock(&d->lock);
	return _errno_to_alp(e);
}

/**
 * @brief Wake the RX thread's poll() out of its blocking wait and
 *        report whether THIS call itself arrived from the handle's
 *        own RX thread (issue #756, GHSA-xhm8-7f87-93q5-style
 *        redesign).
 *
 * Writing to rx_wake_pipe[1] makes _rx_loop()'s poll() return on the
 * wake-pipe branch, breaking its loop -- see this file's header
 * comment for why this replaced closing d->fd directly (a real
 * ThreadSanitizer-flagged close()-vs-concurrent-read() hazard, not a
 * false positive).  An EXTERNAL close (a different thread) then joins
 * the RX thread synchronously and returns ALP_CAN_SHUTDOWN_DONE.  A
 * SELF-close (a filter callback, running ON d->rx_thread, calling
 * alp_can_close() on its own handle) must NOT join -- that thread IS
 * this call's own caller, so pthread_join() would be a guaranteed
 * EDEADLK -- so it returns ALP_CAN_SHUTDOWN_DEFERRED WITHOUT joining;
 * _rx_loop()'s epilogue (this same thread, once the loop above has
 * actually unwound) completes the deferred teardown via
 * alp_can_close_finalize().
 */
static alp_can_shutdown_result_t y_shutdown(alp_can_backend_state_t *st)
{
	y_can_data_t *d = (y_can_data_t *)st->be_data;
	if (d == NULL) return ALP_CAN_SHUTDOWN_DONE;

	bool      join = false;
	pthread_t th   = d->rx_thread;
	pthread_mutex_lock(&d->lock);
	bool from_worker    = d->rx_running && pthread_equal(pthread_self(), th);
	bool thread_existed = d->rx_running;
	if (d->rx_running) {
		d->rx_running = false;
		join          = !from_worker;
	}
	pthread_mutex_unlock(&d->lock);
	y_can_close_from_worker_store(d, from_worker);

	/* Wake _rx_loop()'s poll() -- the SOLE cross-thread cancellation
     * signal (d->fd itself is never touched here; it is closed exactly
     * once, in y_destroy(), strictly after the RX thread has already
     * exited). */
	if (thread_existed && d->rx_wake_pipe[1] >= 0) {
		char b = 1;
		(void)write(d->rx_wake_pipe[1], &b, 1);
	}

	if (from_worker) {
		return ALP_CAN_SHUTDOWN_DEFERRED;
	}
	if (join) {
		(void)pthread_join(th, NULL);
	}
	return ALP_CAN_SHUTDOWN_DONE;
}

/**
 * @brief Free the socket/pipe/mutex/handle box.
 *
 * Called exactly once by the dispatcher (src/can_dispatch.c), strictly
 * after y_shutdown() has run AND active_ops has drained to 0 -- either
 * synchronously from alp_can_close() (the DONE/external-close path) or
 * via alp_can_close_finalize() (the DEFERRED/self-close path, from
 * _rx_loop()'s epilogue).  Must not block -- by the time this runs the
 * RX thread is already joined-and-exited (external path) or IS the
 * very thread calling this (self path, about to return from
 * _rx_loop() anyway) -- so closing d->fd here, rather than from
 * y_shutdown(), can never race a still-in-flight read()/poll() on it
 * (issue #756).
 */
static void y_destroy(alp_can_backend_state_t *st)
{
	y_can_data_t *d = (y_can_data_t *)st->be_data;
	if (d == NULL) return;
	st->be_data = NULL;
	if (d->fd >= 0) close(d->fd);
	if (d->rx_wake_pipe[0] >= 0) close(d->rx_wake_pipe[0]);
	if (d->rx_wake_pipe[1] >= 0) close(d->rx_wake_pipe[1]);
	pthread_mutex_destroy(&d->lock);
	free(d);
}

static const alp_can_ops_t _ops = {
	.open          = y_open,
	.start         = y_start,
	.stop          = y_stop,
	.send          = y_send,
	.add_filter    = y_add_filter,
	.remove_filter = y_remove_filter,
	.shutdown      = y_shutdown,
	.destroy       = y_destroy,
};

ALP_BACKEND_REGISTER(can,
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
