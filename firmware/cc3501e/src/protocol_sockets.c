/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: TCP/UDP socket command-family handlers
 * (0x20..0x24).  Split out of protocol.c (issue #461); protocol_dispatch()
 * in protocol.c still owns the single command-family switch that routes
 * here.
 *
 * All five WORKER-ROUTE their (blocking) lwIP body off the SPI ISR:
 * every socket op is a tcpip_apimsg round-trip to the lwIP core thread
 * (connect/recv also wait on the network).  OPEN/SEND/RECV return reply
 * DATA (handle / byte-count / bytes) via the payload+reply seam;
 * CONNECT/CLOSE carry a payload only.  Socket payloads are validated
 * field-by-field here (the wire structs are naturally packed, but parse
 * defensively -- the request buffer alignment is transport-defined).
 * The raw req is forwarded to the worker, which re-parses the same
 * struct in the drain.
 */

#include "protocol_internal.h"

/* SOCK_OPEN (0x20): req = alp_cc3501e_sock_open_t { family | type | protocol |
 * reserved } = 4 B.  Reply DATA = alp_cc3501e_sock_handle_t (4 B). */
alp_cc3501e_resp_t handle_sock_open(const uint8_t *req,
                                    size_t         req_len,
                                    uint8_t       *reply_data,
                                    size_t         reply_cap,
                                    size_t        *reply_data_len)
{
	*reply_data_len = 0u;
	if (req_len != sizeof(alp_cc3501e_sock_open_t)) return ALP_CC3501E_RESP_ERR_INVALID;
	if (req[0] != (uint8_t)ALP_CC3501E_SOCK_FAMILY_IPV4) {
		return ALP_CC3501E_RESP_ERR_INVALID; /* v1 IP stack is IPv4-only */
	}
	return handle_worker_routed_payload_reply(ALP_CC3501E_CMD_SOCK_OPEN,
	                                          req,
	                                          req_len,
	                                          sizeof(alp_cc3501e_sock_handle_t),
	                                          reply_data,
	                                          reply_cap,
	                                          reply_data_len);
}

/* SOCK_CONNECT (0x21): req = alp_cc3501e_sock_connect_t = 24 B.  No reply data. */
alp_cc3501e_resp_t handle_sock_connect(const uint8_t *req,
                                       size_t         req_len,
                                       uint8_t       *reply_data,
                                       size_t         reply_cap,
                                       size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	if (req_len != sizeof(alp_cc3501e_sock_connect_t)) return ALP_CC3501E_RESP_ERR_INVALID;
	if (req[4] != (uint8_t)ALP_CC3501E_SOCK_FAMILY_IPV4) { /* peer.family */
		return ALP_CC3501E_RESP_ERR_INVALID;
	}
	return handle_worker_routed_payload(ALP_CC3501E_CMD_SOCK_CONNECT, req, req_len, reply_data_len);
}

/* SOCK_SEND (0x22): req = alp_cc3501e_sock_send_t (8 B) + data_len inline bytes.
 * Reply DATA = uint16_t LE queued-byte count. */
alp_cc3501e_resp_t handle_sock_send(const uint8_t *req,
                                    size_t         req_len,
                                    uint8_t       *reply_data,
                                    size_t         reply_cap,
                                    size_t        *reply_data_len)
{
	*reply_data_len = 0u;
	if (req_len < sizeof(alp_cc3501e_sock_send_t)) return ALP_CC3501E_RESP_ERR_INVALID;
	const uint16_t data_len = (uint16_t)req[4] | ((uint16_t)req[5] << 8);
	if (req_len != sizeof(alp_cc3501e_sock_send_t) + (size_t)data_len) {
		return ALP_CC3501E_RESP_ERR_INVALID; /* declared length must match the frame */
	}
	return handle_worker_routed_payload_reply(
	    ALP_CC3501E_CMD_SOCK_SEND, req, req_len, 2u, reply_data, reply_cap, reply_data_len);
}

/* SOCK_RECV (0x23): req = alp_cc3501e_sock_recv_t { handle | max_len } = 4 B.
 * Reply DATA = alp_cc3501e_sock_recv_resp_t (24 B) + received bytes inline. */
alp_cc3501e_resp_t handle_sock_recv(const uint8_t *req,
                                    size_t         req_len,
                                    uint8_t       *reply_data,
                                    size_t         reply_cap,
                                    size_t        *reply_data_len)
{
	*reply_data_len = 0u;
	if (req_len != sizeof(alp_cc3501e_sock_recv_t)) return ALP_CC3501E_RESP_ERR_INVALID;
	return handle_worker_routed_payload_reply(ALP_CC3501E_CMD_SOCK_RECV,
	                                          req,
	                                          req_len,
	                                          sizeof(alp_cc3501e_sock_recv_resp_t),
	                                          reply_data,
	                                          reply_cap,
	                                          reply_data_len);
}

/* SOCK_CLOSE (0x24): req = alp_cc3501e_sock_close_t { handle | reserved } = 4 B.
 * No reply data. */
alp_cc3501e_resp_t handle_sock_close(const uint8_t *req,
                                     size_t         req_len,
                                     uint8_t       *reply_data,
                                     size_t         reply_cap,
                                     size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	if (req_len != sizeof(alp_cc3501e_sock_close_t)) return ALP_CC3501E_RESP_ERR_INVALID;
	return handle_worker_routed_payload(ALP_CC3501E_CMD_SOCK_CLOSE, req, req_len, reply_data_len);
}
