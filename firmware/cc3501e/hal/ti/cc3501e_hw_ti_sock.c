/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- TCP/UDP sockets (v0.5, lwIP BSD socket
 * path).
 *
 * Split by hardware subsystem out of cc3501e_hw_ti.c (issue #703, #461
 * Phase B).  cc3501e_hw_ti.c keeps platform lifecycle + the deferred-reboot
 * latch; see cc3501e_hw_ti_internal.h for the cross-TU seam.
 *
 * Built ONLY for CC3501E_HAL_BACKEND=ti (the bench build), against TI's
 * SimpleLink CC35xx SDK.  CI builds the stub backend instead, so this file
 * is never on the SDK-free path.
 */

#include <stdint.h>
#include <string.h>

#ifdef CC3501E_WIFI
/* lwIP BSD socket API for the TCP/UDP data path (CMD_SOCK_* 0x20..0x24): the
 * osi lwipopts enable LWIP_SOCKET + LWIP_COMPAT_SOCKETS + LWIP_TCP/UDP and the
 * prebuilt lwip.a carries sockets.c.  lwip_socket / lwip_connect / lwip_send /
 * lwip_recvfrom / lwip_close + struct sockaddr_in / SO_RCVTIMEO live here. */
#include <lwip/sockets.h>
/* Wi-Fi console UART logger (network_terminal demo adaptation/uart_term.c, linked
 * in the --wifi build): Report() surfaces the real reason a socket op failed on the
 * bench console -- the only diagnostic channel this headless bridge has. */
#include <uart_term.h>
/* FreeRTOS heap accounting (resolves at link time; declared here so the socket
 * failure path can report free heap without pulling the kernel headers). */
extern size_t xPortGetFreeHeapSize(void);
#endif

#include "alp/protocol/cc3501e.h"

#include "../cc3501e_hw.h"

/* --------------------------------------------------------------- */
/* TCP/UDP sockets (v0.5) -- lwIP BSD socket path.                   */
/*                                                                   */
/* CMD_SOCK_* (0x20..0x24) route here through the async worker: every */
/* lwip_* body below BLOCKS (a tcpip_apimsg round-trip to the lwIP   */
/* core thread; connect/recv also wait on the network), so -- like   */
/* the Wlan_* ops -- they MUST run in worker_run_pending, never the  */
/* SPI ISR.  The handle handed to the host is the lwIP fd + 1 so the */
/* protocol's "0 = invalid handle" contract holds (lwIP fds start at */
/* 0).  IPv4 only this rev (the osi lwipopts bring up an IPv4 stack). */
/* Under !CC3501E_WIFI (no lwIP) every body is NOTIMPL -> NOT_READY.  */
/* --------------------------------------------------------------- */
#ifdef CC3501E_WIFI
/* Bounded receive timeout so a worker RECV job can never wedge the drain on a
 * silent/half-open peer: after this window lwip_recv returns EWOULDBLOCK, which
 * the recv body maps to "0 bytes available" (OK) per the non-blocking wire
 * contract.  The host re-issues CMD_SOCK_RECV to poll for more. */
/* WAS 4000.  A blocking receive stalls the WHOLE WORKER for its duration, and
 * worker_run_pending() holds READY LOW across the whole job -- so no bridge
 * frame of ANY opcode is served while it waits.  A 4 s empty read therefore
 * blacked the bridge out for 4 s per poll, capping socket streaming at a
 * fraction of a frame per second and inverting against any host timeout
 * shorter than 4 s (the host gave up before the firmware could answer "0 bytes
 * available").
 *
 * cc3501e_hw_sock_recv() now passes MSG_DONTWAIT and does not rely on this at
 * all; it is kept as the socket's default so any OTHER blocking operation on
 * the handle is bounded to something short rather than to lwIP's default. */
/* Blocking timeout on the prefetch socket.  cc3501e_hw_sock_pump() runs once
 * per task tick, so EVERY momentarily-empty poll stalls the whole task for
 * this long while the bridge keeps draining the ring -- which is why replies
 * came back short.  Was 4000, then 50; 2 ms keeps the stall an order of
 * magnitude below a bridge transaction.  Do NOT go to 0/MSG_DONTWAIT: that
 * returned 0 bytes for 81 s on a connection the server had already fed
 * 256 KiB (bench-measured, reverted). */
#define CC3501E_SOCK_RCVTIMEO_MS 2

int cc3501e_hw_sock_open(uint8_t family, uint8_t type, uint8_t protocol, uint16_t *handle_out)
{
	if (handle_out == 0) {
		return CC3501E_HW_ERR_INVAL;
	}
	*handle_out = 0u;
	if (family != (uint8_t)ALP_CC3501E_SOCK_FAMILY_IPV4) {
		return CC3501E_HW_ERR_INVAL; /* v1 IP stack is IPv4-only */
	}
	const int st = (type == (uint8_t)ALP_CC3501E_SOCK_TYPE_DGRAM) ? SOCK_DGRAM : SOCK_STREAM;
	const int fd = lwip_socket(AF_INET, st, (int)protocol);
	if (fd < 0) {
		/* netconn allocation failed -- typically FreeRTOS-heap exhaustion for the
		 * recvmbox/sem, or MEMP_NUM_NETCONN starvation.  UNMASK the real reason on the
		 * bench console (errno + free heap), then FAIL FAST: return NOTIMPL, which the
		 * protocol layer maps to RESP_ERR_NOT_READY -- a NON-retryable host error.  (IO
		 * would map to RESP_ERR_RADIO -> host ALP_ERR_IO, which poll_by_repeat retries
		 * for the whole budget and masks as a -4 timeout.)  NOT_READY == "the IP stack
		 * cannot serve a socket right now", which is exactly this condition. */
		Report("\n\rcc3501e sock_open: lwip_socket failed errno=%d freeHeap=%u\n\r",
		       errno,
		       (unsigned)xPortGetFreeHeapSize());
		return CC3501E_HW_ERR_NOTIMPL;
	}
	/* lwIP fds are small non-negative ints; +1 keeps host handle 0 = invalid.  A
	 * full u16 table is unnecessary -- lwIP validates the fd (EBADF) on each op. */
	if (fd >= 0xFFFF) {
		(void)lwip_close(fd);
		return CC3501E_HW_ERR_IO;
	}
	struct timeval tv = { .tv_sec  = CC3501E_SOCK_RCVTIMEO_MS / 1000,
		                  .tv_usec = (CC3501E_SOCK_RCVTIMEO_MS % 1000) * 1000 };
	(void)lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	*handle_out = (uint16_t)(fd + 1);
	return CC3501E_HW_OK;
}

int cc3501e_hw_sock_connect(uint16_t handle, uint8_t family, uint16_t port, const uint8_t addr[4])
{
	if (handle == 0u || addr == 0) {
		return CC3501E_HW_ERR_INVAL;
	}
	if (family != (uint8_t)ALP_CC3501E_SOCK_FAMILY_IPV4) {
		return CC3501E_HW_ERR_INVAL;
	}
	const int          fd = (int)handle - 1;
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port   = lwip_htons(port); /* host-order port -> network order */
	/* addr[0..3] are already big-endian (network order); s_addr is a network-order
	 * u32, so a straight copy lands the octets in the right byte positions. */
	memcpy(&sa.sin_addr.s_addr, addr, 4);
	if (lwip_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		return CC3501E_HW_ERR_IO;
	}
	/* A connected STREAM socket is the bulk-receive case -- start prefetching so
	 * CMD_SOCK_RECV can be answered synchronously from the dispatch. */
	cc3501e_hw_sock_prefetch(handle, true);
	return CC3501E_HW_OK;
}

int cc3501e_hw_sock_send(uint16_t       handle,
                         uint8_t        flags,
                         const uint8_t *data,
                         uint16_t       data_len,
                         uint16_t      *sent_out)
{
	(void)flags; /* MORE hint is advisory; lwip_send has no matching flag here */
	if (sent_out != 0) *sent_out = 0u;
	if (handle == 0u || (data == 0 && data_len > 0u)) {
		return CC3501E_HW_ERR_INVAL;
	}
	const int     fd = (int)handle - 1;
	const ssize_t n  = lwip_send(fd, data, data_len, 0);
	if (n < 0) {
		return CC3501E_HW_ERR_IO;
	}
	if (sent_out != 0) *sent_out = (uint16_t)n;
	return CC3501E_HW_OK;
}

/* ================= SOCKET RX PREFETCH RING =================
 *
 * WHY: protocol_dispatch() runs in the SPI transfer-complete callback (SWI/HWI
 * context) and cannot call lwIP, so CMD_SOCK_RECV had to be worker-routed --
 * handle_worker_routed_payload_reply always answers BUSY to the submit and the
 * host must come back for the answer.  That is TWO bridge transactions plus a
 * wait for the worker loop per frame, and it is the dominant per-frame cost of a
 * socket stream.
 *
 * The OTA write path already solved this shape: cc3501e_hw_ota_write is
 * SYNCHRONOUS because it only memcpy's into a staged window.  Do the same here.
 * The TASK side (cc3501e_hw_tick -> cc3501e_hw_sock_pump) does the lwIP work and
 * fills this ring; the DISPATCH side only memcpy's out of it, which is ISR-safe.
 * CMD_SOCK_RECV then costs ONE transaction and no worker round trip.
 *
 * Single stream socket by design: this serves the bulk-receive case, and one
 * ring keeps the ISR-vs-task handshake to a single producer and a single
 * consumer (head written by the task only, tail by the dispatch only). */
#define CC3501E_SOCK_RING_BYTES 8192u
/* Max lwip_recv() calls per pump tick; see cc3501e_hw_sock_pump(). */
#define CC3501E_SOCK_PUMP_PASSES 8u

static struct {
	uint8_t           buf[CC3501E_SOCK_RING_BYTES];
	volatile uint32_t head;     /* task writes   */
	volatile uint32_t tail;     /* dispatch reads */
	volatile uint16_t fd_plus1; /* socket being prefetched, 0 = none */
	volatile bool     peer_closed;
} rx_ring;

static uint32_t ring_used(void)
{
	return rx_ring.head - rx_ring.tail; /* free-running; unsigned wrap is correct */
}

/* TASK CONTEXT ONLY -- called from cc3501e_hw_tick().  Does the lwIP read. */
void cc3501e_hw_sock_pump(void)
{
	const uint16_t h = rx_ring.fd_plus1;
	if (h == 0u || rx_ring.peer_closed) {
		return;
	}
	/* Drain what lwIP already has, not one segment per tick.  The bridge takes up
	 * to ALP_CC3501E_MAX_PAYLOAD per transaction while a single lwip_recv()
	 * returns about one TCP segment, so a one-shot pump left the ring
	 * under-filled and every reply came back short.  Safe only because
	 * CC3501E_SOCK_RCVTIMEO_MS is small -- at the old 50 ms an extra pass cost
	 * more than a transaction. */
	for (uint32_t pass = 0u; pass < CC3501E_SOCK_PUMP_PASSES; ++pass) {
		uint32_t used = ring_used();
		if (used > CC3501E_SOCK_RING_BYTES - (uint32_t)ALP_CC3501E_MAX_PAYLOAD) {
			return; /* keep at least one max frame of headroom */
		}
		const uint32_t idx   = rx_ring.head % CC3501E_SOCK_RING_BYTES;
		uint32_t       chunk = CC3501E_SOCK_RING_BYTES - idx; /* to the wrap only */
		const uint32_t space = CC3501E_SOCK_RING_BYTES - used;
		if (chunk > space) chunk = space;
		if (chunk == 0u) {
			return;
		}
		const ssize_t n = lwip_recv((int)h - 1, &rx_ring.buf[idx], (size_t)chunk, 0);
		if (n > 0) {
			rx_ring.head += (uint32_t)n;
			if ((uint32_t)n < chunk) {
				return; /* socket drained -- don't spend a timeout on the next pass */
			}
			continue;
		}
		if (n == 0) {
			rx_ring.peer_closed = true; /* orderly close */
		}
		/* n < 0 with EAGAIN/EWOULDBLOCK is just "nothing yet" -- next tick. */
		return;
	}
}

/* Arm/disarm prefetch for a handle.  Called from the socket open/close paths. */
void cc3501e_hw_sock_prefetch(uint16_t handle, bool on)
{
	if (on) {
		rx_ring.head = rx_ring.tail = 0u;
		rx_ring.peer_closed         = false;
		rx_ring.fd_plus1            = handle;
	} else if (rx_ring.fd_plus1 == handle) {
		rx_ring.fd_plus1 = 0u;
	}
}

/* DISPATCH CONTEXT (SWI/HWI) -- memcpy only, never lwIP.  Returns bytes taken,
 * or -1 when this handle is not the prefetched one so the caller can fall back
 * to the worker path. */
int cc3501e_hw_sock_recv_ring(uint16_t handle, uint8_t *buf, uint16_t cap, uint16_t *out_len)
{
	if (out_len != 0) *out_len = 0u;
	if (rx_ring.fd_plus1 != handle || handle == 0u || buf == 0) {
		return -1;
	}
	uint32_t used = ring_used();
	if (used == 0u) {
		/* Armed but EMPTY -> report "not handled" so the caller falls through to
		 * the worker path.  Answering OK with 0 bytes looked harmless but broke
		 * single-shot callers: cc3501e_sock_recv goes through poll_by_repeat,
		 * which treats ALP_OK as final, so one recv on a socket whose data had not
		 * landed yet returned 0 bytes and gave up (measured: `NET recv -> 0 (0 B)`
		 * on a connection that was about to deliver 389 B).  The fast path is an
		 * OPTIMISATION for when data is already staged; it must never take a
		 * request it cannot satisfy. */
		return -1;
	}
	uint32_t       n     = (used < cap) ? used : cap;
	const uint32_t idx   = rx_ring.tail % CC3501E_SOCK_RING_BYTES;
	uint32_t       first = CC3501E_SOCK_RING_BYTES - idx;
	if (first > n) first = n;
	memcpy(buf, &rx_ring.buf[idx], first);
	if (n > first) {
		memcpy(&buf[first], &rx_ring.buf[0], n - first);
	}
	rx_ring.tail += n;
	if (out_len != 0) *out_len = (uint16_t)n;
	return (int)n;
}

int cc3501e_hw_sock_recv(uint16_t  handle,
                         uint16_t  max_len,
                         uint8_t  *buf,
                         uint16_t  cap,
                         uint16_t *recv_len_out,
                         uint8_t   from_addr[4],
                         uint16_t *from_port_out)
{
	if (recv_len_out != 0) *recv_len_out = 0u;
	if (from_addr != 0) {
		memset(from_addr, 0, 4);
	}
	if (from_port_out != 0) *from_port_out = 0u;
	if (handle == 0u || buf == 0) {
		return CC3501E_HW_ERR_INVAL;
	}
	const int fd   = (int)handle - 1;
	uint16_t  want = (max_len < cap) ? max_len : cap;

	struct sockaddr_in from;
	socklen_t          fromlen = sizeof(from);
	memset(&from, 0, sizeof(from));
	/* BLOCKING, bounded by the socket's SO_RCVTIMEO (CC3501E_SOCK_RCVTIMEO_MS).
	 *
	 * MSG_DONTWAIT was tried here and is WRONG on this stack -- silicon-measured
	 * 2026-08-24: with it, a 256 KiB HTTP body that the server demonstrably
	 * delivered (two `GET /speed.bin HTTP/1.0` 200 hits logged from the device's
	 * own IP) produced `NET recv -> 0 (0 B)` on EVERY call for 81 s, i.e. 0 B/s.
	 * The non-blocking path returns EWOULDBLOCK before lwIP has moved anything
	 * into the socket, so the host never drains the connection at all.  A short
	 * blocking read does return data.
	 *
	 * The reason the timeout must stay SHORT is unchanged: this runs on the
	 * worker, and worker_run_pending() holds READY LOW across the whole job, so
	 * no bridge frame of ANY opcode is served while it waits.  That is why 4000
	 * became 50 -- not why it should become zero. */
	const ssize_t n = lwip_recvfrom(fd, buf, want, 0, (struct sockaddr *)&from, &fromlen);
	if (n < 0) {
		/* SO_RCVTIMEO expiry (EAGAIN / EWOULDBLOCK) is NOT an error at the wire: it
		 * means "no data yet" -- report OK with 0 bytes so the host re-polls.  Any
		 * other errno is a real socket failure (bad fd / reset) -> IO.  The ticlang
		 * C <errno.h> defines EAGAIN but not always EWOULDBLOCK, so guard the latter
		 * (lwIP treats the two as equal on this platform). */
		if (errno == EAGAIN
#ifdef EWOULDBLOCK
		    || errno == EWOULDBLOCK
#endif
		) {
			return CC3501E_HW_OK;
		}
		return CC3501E_HW_ERR_IO;
	}
	/* n == 0 on a STREAM socket means the peer closed -- still OK, 0 bytes. */
	if (recv_len_out != 0) *recv_len_out = (uint16_t)n;
	if (from.sin_family == AF_INET) {
		if (from_addr != 0) memcpy(from_addr, &from.sin_addr.s_addr, 4);
		if (from_port_out != 0) *from_port_out = lwip_ntohs(from.sin_port);
	}
	return CC3501E_HW_OK;
}

int cc3501e_hw_sock_close(uint16_t handle)
{
	cc3501e_hw_sock_prefetch(handle, false);
	if (handle == 0u) {
		return CC3501E_HW_ERR_INVAL;
	}
	if (lwip_close((int)handle - 1) != 0) {
		return CC3501E_HW_ERR_IO;
	}
	return CC3501E_HW_OK;
}
#else  /* !CC3501E_WIFI -- no lwIP: report NOTIMPL (-> RESP_ERR_NOT_READY) */
int cc3501e_hw_sock_open(uint8_t family, uint8_t type, uint8_t protocol, uint16_t *handle_out)
{
	(void)family;
	(void)type;
	(void)protocol;
	if (handle_out != 0) *handle_out = 0u;
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_sock_connect(uint16_t handle, uint8_t family, uint16_t port, const uint8_t addr[4])
{
	(void)handle;
	(void)family;
	(void)port;
	(void)addr;
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_sock_send(uint16_t       handle,
                         uint8_t        flags,
                         const uint8_t *data,
                         uint16_t       data_len,
                         uint16_t      *sent_out)
{
	(void)handle;
	(void)flags;
	(void)data;
	(void)data_len;
	if (sent_out != 0) *sent_out = 0u;
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_sock_recv(uint16_t  handle,
                         uint16_t  max_len,
                         uint8_t  *buf,
                         uint16_t  cap,
                         uint16_t *recv_len_out,
                         uint8_t   from_addr[4],
                         uint16_t *from_port_out)
{
	(void)handle;
	(void)max_len;
	(void)buf;
	(void)cap;
	if (recv_len_out != 0) *recv_len_out = 0u;
	if (from_addr != 0) memset(from_addr, 0, 4);
	if (from_port_out != 0) *from_port_out = 0u;
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_sock_close(uint16_t handle)
{
	(void)handle;
	return CC3501E_HW_ERR_NOTIMPL;
}
#endif /* CC3501E_WIFI */
