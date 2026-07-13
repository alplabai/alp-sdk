/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sockets.h
 * @brief CC3501E TCP/UDP socket host helpers (opcodes 0x20..0x24).
 *
 * A minimal BSD-style socket API offloaded to the CC3501E's IP stack:
 * the host opens a handle, connects it, sends + receives bytes, then
 * closes it.  Each call is one worker-routed firmware op (the socket
 * bodies block on the lwIP core thread), so every wrapper is a
 * poll-by-repeat over the bridge like the Wi-Fi getters.  v1 is
 * IPv4-only; addresses are 4 octets in network (big-endian) order.
 */

#ifndef ALP_CHIPS_CC3501E_SOCKETS_H
#define ALP_CHIPS_CC3501E_SOCKETS_H

#include <stdint.h>
#include <stddef.h>

#include "alp/chips/cc3501e/core.h"
#include "alp/protocol/cc3501e.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open a socket on the CC3501E IP stack (SOCK_OPEN, opcode 0x20).
 *
 * Allocates a socket in the firmware IP stack and returns its handle.  The
 * handle is opaque and non-zero (0 is the invalid handle); pass it to every
 * later socket call for this socket.  Worker-routed poll-by-repeat: re-issued
 * while the firmware reports RESP_ERR_BUSY until the socket is allocated.
 *
 * @param ctx         Initialised driver context.
 * @param family      Address family (@ref ALP_CC3501E_SOCK_FAMILY_IPV4; IPv6
 *                    reserved in v1).
 * @param type        @ref ALP_CC3501E_SOCK_TYPE_STREAM (TCP) or
 *                    @ref ALP_CC3501E_SOCK_TYPE_DGRAM (UDP).
 * @param protocol    IP protocol number, or 0 for the type's default
 *                    (TCP for STREAM, UDP for DGRAM).
 * @param handle_out  Receives the socket handle on success (must not be NULL).
 * @param timeout_ms  Upper bound on the poll-by-repeat budget.
 * @return ALP_OK with @p handle_out set; ALP_ERR_NOT_READY if the firmware IP
 *         stack is unavailable (stub / no-Wi-Fi build); mapped error otherwise.
 */
alp_status_t cc3501e_sock_open(cc3501e_t *ctx,
                               uint8_t    family,
                               uint8_t    type,
                               uint8_t    protocol,
                               uint16_t  *handle_out,
                               uint32_t   timeout_ms);

/**
 * @brief Connect a socket to a peer (SOCK_CONNECT, opcode 0x21).
 *
 * For STREAM sockets this runs the TCP handshake to @p ip : @p port; for DGRAM
 * sockets it sets the default peer for later @ref cc3501e_sock_send calls.  The
 * firmware body blocks on the handshake, so this is a worker-routed
 * poll-by-repeat until it resolves.
 *
 * @param ctx         Initialised driver context.
 * @param handle      Socket handle from @ref cc3501e_sock_open.
 * @param ip          Destination IPv4 address, 4 octets in network order
 *                    (@c ip[0] is the most significant octet, a.b.c.d).
 * @param port        Destination TCP/UDP port, host byte order (the firmware
 *                    converts to network order on the wire).
 * @param timeout_ms  Upper bound on the connect poll budget.
 * @return ALP_OK once connected; ALP_ERR_NOT_READY on the stub build; mapped
 *         error (e.g. ALP_ERR_IO on a refused/timed-out handshake) otherwise.
 */
alp_status_t cc3501e_sock_connect(cc3501e_t    *ctx,
                                  uint16_t      handle,
                                  const uint8_t ip[4],
                                  uint16_t      port,
                                  uint32_t      timeout_ms);

/**
 * @brief Send bytes on a socket (SOCK_SEND, opcode 0x22).
 *
 * Queues @p len bytes on the socket and reports how many the stack accepted in
 * @p sent_out.  @p len is bounded by one frame
 * (<= ALP_CC3501E_MAX_PAYLOAD - 8, the send-header size); larger buffers must be
 * split by the caller.  Worker-routed poll-by-repeat.
 *
 * @param ctx         Initialised driver context.
 * @param handle      Socket handle from @ref cc3501e_sock_open.
 * @param data        Payload bytes to send.
 * @param len         Number of bytes in @p data.
 * @param sent_out    Receives the accepted byte count (may be NULL).
 * @param timeout_ms  Upper bound on the send poll budget.
 * @return ALP_OK once queued; ALP_ERR_INVAL if @p len exceeds one frame;
 *         ALP_ERR_NOT_READY on the stub build; mapped error otherwise.
 */
alp_status_t cc3501e_sock_send(cc3501e_t     *ctx,
                               uint16_t       handle,
                               const uint8_t *data,
                               size_t         len,
                               size_t        *sent_out,
                               uint32_t       timeout_ms);

/**
 * @brief Receive bytes from a socket (SOCK_RECV, opcode 0x23).
 *
 * Requests up to @p cap bytes from the socket's receive queue into @p buf.  A
 * zero-length result (@p recv_len_out set to 0 with ALP_OK) means no data was
 * available within the firmware's receive window, or the peer closed the
 * connection -- the caller polls again to distinguish (or stops on a subsequent
 * zero after a close).  Worker-routed poll-by-repeat over the bridge.
 *
 * @param ctx           Initialised driver context.
 * @param handle        Socket handle from @ref cc3501e_sock_open.
 * @param buf           Destination buffer for received bytes.
 * @param cap           Capacity of @p buf (also bounds the firmware request).
 * @param recv_len_out  Receives the number of bytes written to @p buf (may be
 *                      NULL).
 * @param timeout_ms    Upper bound on the receive poll budget.
 * @return ALP_OK with @p recv_len_out set (possibly 0); ALP_ERR_NOT_READY on the
 *         stub build; mapped error otherwise.
 */
alp_status_t cc3501e_sock_recv(cc3501e_t *ctx,
                               uint16_t   handle,
                               uint8_t   *buf,
                               size_t     cap,
                               size_t    *recv_len_out,
                               uint32_t   timeout_ms);

/**
 * @brief Close a socket (SOCK_CLOSE, opcode 0x24).
 *
 * Releases the firmware-side socket and, for STREAM sockets, issues the TCP
 * teardown.  The handle is invalid afterwards and the firmware may reuse its
 * value.  Worker-routed poll-by-repeat.
 *
 * @param ctx         Initialised driver context.
 * @param handle      Socket handle from @ref cc3501e_sock_open.
 * @param timeout_ms  Upper bound on the close poll budget.
 * @return ALP_OK once closed; ALP_ERR_NOT_READY on the stub build; mapped error
 *         otherwise.
 */
alp_status_t cc3501e_sock_close(cc3501e_t *ctx, uint16_t handle, uint32_t timeout_ms);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_CC3501E_SOCKETS_H */
