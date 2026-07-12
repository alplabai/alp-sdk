/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * CC3501E TCP/UDP socket host helpers (opcodes 0x20..0x24).  See
 * <alp/chips/cc3501e/sockets.h> for the public API.
 *
 * Each wraps cc3501e_request over the packed wire structs in
 * <alp/protocol/cc3501e.h>.  The firmware worker-routes every socket
 * op (the lwIP bodies block), so each is a poll-by-repeat that re-
 * issues the SAME frame while the firmware reports RESP_ERR_BUSY (op
 * in flight) or the bridge reads IO (down mid-op), until it resolves.
 * v1 IPv4-only; addresses are 4 octets in network order.
 */

#include <string.h>
#include <stdint.h>

#include "cc3501e_internal.h"

/* Wire header size of alp_cc3501e_sock_send_t (handle | flags | reserved |
 * data_len | reserved2), and of the alp_cc3501e_sock_recv_resp_t reply header
 * (from sock_addr(20) | data_len | reserved).  Fixed by the protocol header. */
#define CC3501E_SOCK_SEND_HDR      8u
#define CC3501E_SOCK_RECV_RESP_HDR 24u

alp_status_t cc3501e_sock_open(cc3501e_t *ctx,
                               uint8_t    family,
                               uint8_t    type,
                               uint8_t    protocol,
                               uint16_t  *handle_out,
                               uint32_t   timeout_ms)
{
	if (handle_out == NULL) return ALP_ERR_INVAL;
	*handle_out = 0u;
	/* SOCK_OPEN (0x20) wire = alp_cc3501e_sock_open_t { family | type | protocol |
	 * reserved }; reply DATA = alp_cc3501e_sock_handle_t { handle(LE16) | rsvd }. */
	uint8_t      payload[4] = { family, type, protocol, 0u };
	uint8_t      reply[4]   = { 0 };
	size_t       got        = 0;
	alp_status_t s          = poll_by_repeat(ctx,
	                                         ALP_CC3501E_CMD_SOCK_OPEN,
	                                         payload,
	                                         sizeof(payload),
	                                         reply,
	                                         sizeof(reply),
	                                         &got,
	                                         timeout_ms);
	if (s != ALP_OK) return s;
	if (got < 2u) return ALP_ERR_IO; /* short reply -- firmware/wire gap */
	*handle_out = (uint16_t)reply[0] | ((uint16_t)reply[1] << 8);
	return ALP_OK;
}

alp_status_t cc3501e_sock_connect(cc3501e_t    *ctx,
                                  uint16_t      handle,
                                  const uint8_t ip[4],
                                  uint16_t      port,
                                  uint32_t      timeout_ms)
{
	if (ip == NULL) return ALP_ERR_INVAL;
	/* SOCK_CONNECT (0x21) wire = alp_cc3501e_sock_connect_t: handle(LE16) |
	 * reserved(2) | peer sock_addr { family | reserved | port(LE16) | addr[16] }. */
	uint8_t p[24];
	memset(p, 0, sizeof(p));
	p[0] = (uint8_t)(handle & 0xFFu);
	p[1] = (uint8_t)((handle >> 8) & 0xFFu);
	p[4] = (uint8_t)ALP_CC3501E_SOCK_FAMILY_IPV4; /* peer.family */
	p[6] = (uint8_t)(port & 0xFFu);               /* peer.port (LE16, host order) */
	p[7] = (uint8_t)((port >> 8) & 0xFFu);
	memcpy(&p[8], ip, 4); /* peer.addr[0..3]; addr[4..15] stay zero (IPv4) */
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_SOCK_CONNECT, p, sizeof(p), NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_sock_send(cc3501e_t     *ctx,
                               uint16_t       handle,
                               const uint8_t *data,
                               size_t         len,
                               size_t        *sent_out,
                               uint32_t       timeout_ms)
{
	if (data == NULL && len > 0u) return ALP_ERR_INVAL;
	if (len > (size_t)(ALP_CC3501E_MAX_PAYLOAD - CC3501E_SOCK_SEND_HDR)) return ALP_ERR_INVAL;
	if (sent_out != NULL) *sent_out = 0u;

	/* SOCK_SEND (0x22) wire = alp_cc3501e_sock_send_t (8 B) + inline data; reply
	 * DATA = uint16_t LE queued-byte count. */
	uint8_t p[ALP_CC3501E_MAX_PAYLOAD];
	p[0] = (uint8_t)(handle & 0xFFu);
	p[1] = (uint8_t)((handle >> 8) & 0xFFu);
	p[2] = 0u; /* flags (MORE bit unused here) */
	p[3] = 0u;
	p[4] = (uint8_t)(len & 0xFFu);
	p[5] = (uint8_t)((len >> 8) & 0xFFu);
	p[6] = 0u;
	p[7] = 0u;
	if (len > 0u) memcpy(&p[CC3501E_SOCK_SEND_HDR], data, len);

	uint8_t      reply[2] = { 0 };
	size_t       got      = 0;
	alp_status_t s        = poll_by_repeat(ctx,
	                                       ALP_CC3501E_CMD_SOCK_SEND,
	                                       p,
	                                       CC3501E_SOCK_SEND_HDR + len,
	                                       reply,
	                                       sizeof(reply),
	                                       &got,
	                                       timeout_ms);
	if (s != ALP_OK) return s;
	if (sent_out != NULL && got >= 2u) {
		*sent_out = (size_t)((uint16_t)reply[0] | ((uint16_t)reply[1] << 8));
	}
	return ALP_OK;
}

alp_status_t cc3501e_sock_recv(cc3501e_t *ctx,
                               uint16_t   handle,
                               uint8_t   *buf,
                               size_t     cap,
                               size_t    *recv_len_out,
                               uint32_t   timeout_ms)
{
	if (buf == NULL && cap > 0u) return ALP_ERR_INVAL;
	if (recv_len_out != NULL) *recv_len_out = 0u;

	/* Bound the requested count so the reply (recv_resp header + data + status)
	 * fits one frame. */
	size_t       want     = cap;
	const size_t want_max = (size_t)(ALP_CC3501E_MAX_PAYLOAD - CC3501E_SOCK_RECV_RESP_HDR - 1u);
	if (want > want_max) want = want_max;

	/* SOCK_RECV (0x23) wire = alp_cc3501e_sock_recv_t { handle(LE16) | max_len(LE16) }. */
	uint8_t p[4] = { (uint8_t)(handle & 0xFFu),
		             (uint8_t)((handle >> 8) & 0xFFu),
		             (uint8_t)(want & 0xFFu),
		             (uint8_t)((want >> 8) & 0xFFu) };

	uint8_t      reply[ALP_CC3501E_MAX_PAYLOAD];
	size_t       got = 0;
	alp_status_t s   = poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_SOCK_RECV, p, sizeof(p), reply, sizeof(reply), &got, timeout_ms);
	if (s != ALP_OK) return s;
	if (got < CC3501E_SOCK_RECV_RESP_HDR) return ALP_ERR_IO; /* short reply header */

	/* recv_resp header: from sock_addr(20) | data_len(LE16 @20) | reserved(@22).
	 * The received bytes follow inline at offset 24. */
	size_t data_len = (size_t)((uint16_t)reply[20] | ((uint16_t)reply[21] << 8));
	if (CC3501E_SOCK_RECV_RESP_HDR + data_len > got) {
		data_len = got - CC3501E_SOCK_RECV_RESP_HDR; /* truncated -- clamp to captured */
	}
	size_t copy = (data_len > cap) ? cap : data_len;
	if (copy > 0u) memcpy(buf, &reply[CC3501E_SOCK_RECV_RESP_HDR], copy);
	if (recv_len_out != NULL) *recv_len_out = copy;
	return ALP_OK;
}

alp_status_t cc3501e_sock_close(cc3501e_t *ctx, uint16_t handle, uint32_t timeout_ms)
{
	/* SOCK_CLOSE (0x24) wire = alp_cc3501e_sock_close_t { handle(LE16) | reserved }. */
	uint8_t p[4] = { (uint8_t)(handle & 0xFFu), (uint8_t)((handle >> 8) & 0xFFu), 0u, 0u };
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_SOCK_CLOSE, p, sizeof(p), NULL, 0, NULL, timeout_ms);
}
