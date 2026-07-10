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
 */

#if defined(__linux__)

#include <errno.h>
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
} y_can_data_t;

/** @brief Map a (positive) errno value to the closest alp_status_t. */
static alp_status_t _errno_to_alp(int err)
{
	switch (err) {
	case 0:
		return ALP_OK;
	case EINVAL:
		return ALP_ERR_INVAL;
	case EBUSY:
		return ALP_ERR_BUSY;
	case EAGAIN:
	case ETIMEDOUT:
		return ALP_ERR_TIMEOUT;
	case ENODEV:
	case ENXIO:
		return ALP_ERR_NOT_READY;
	case ENOMEM:
		return ALP_ERR_NOMEM;
	case EOPNOTSUPP:
	case ENOPROTOOPT:
	case ENOSYS:
		return ALP_ERR_NOSUPPORT;
	default:
		return ALP_ERR_IO;
	}
}

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
	pthread_mutex_lock(&d->lock);
	for (size_t i = 0; i < ALP_Y_CAN_MAX_FILTERS; ++i) {
		y_can_filter_t *f = &d->filters[i];
		if (!f->in_use || f->cb == NULL) continue;
		if ((cmp & f->kf.can_mask) == (f->kf.can_id & f->kf.can_mask)) {
			f->cb(&out, f->user);
		}
	}
	pthread_mutex_unlock(&d->lock);
}

/** @brief RX reader thread: blocking read() loop until the socket closes. */
static void *_rx_loop(void *arg)
{
	y_can_data_t *d = (y_can_data_t *)arg;
	/* A canfd_frame buffer is a superset of can_frame, so one buffer
     * serves both; read() returns the actual frame length. */
	struct canfd_frame frame;
	for (;;) {
		ssize_t n = read(d->fd, &frame, sizeof(frame));
		if (n < 0) {
			if (errno == EINTR) continue;
			break; /* socket closed (EBADF) or fatal error -> stop */
		}
		if (n == 0) break;
		_dispatch_rx(d, &frame, n);
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
	if (pthread_mutex_init(&d->lock, NULL) != 0) {
		close(fd);
		free(d);
		return ALP_ERR_IO;
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
 * @brief Tear down the reader thread + socket and free the handle box.
 *
 * close()ing the socket makes the blocking read() in the reader thread
 * fail, which ends the loop; the thread is then joined before the box
 * (which the thread dereferences) is freed.
 */
static void y_close(alp_can_backend_state_t *st)
{
	y_can_data_t *d = (y_can_data_t *)st->be_data;
	if (d == NULL) return;

	bool      join = false;
	pthread_t th   = d->rx_thread;
	pthread_mutex_lock(&d->lock);
	if (d->rx_running) {
		d->rx_running = false;
		join          = true;
	}
	pthread_mutex_unlock(&d->lock);

	/* Closing the fd unblocks read() in _rx_loop so it returns and the
     * thread exits; join before freeing the box it dereferences. */
	if (d->fd >= 0) {
		close(d->fd);
		d->fd = -1;
	}
	if (join) {
		(void)pthread_join(th, NULL);
	}
	pthread_mutex_destroy(&d->lock);
	free(d);
	st->be_data = NULL;
}

static const alp_can_ops_t _ops = {
	.open          = y_open,
	.start         = y_start,
	.stop          = y_stop,
	.send          = y_send,
	.add_filter    = y_add_filter,
	.remove_filter = y_remove_filter,
	.close         = y_close,
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
