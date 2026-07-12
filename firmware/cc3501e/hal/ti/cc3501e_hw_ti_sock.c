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
#define CC3501E_SOCK_RCVTIMEO_MS 4000

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
