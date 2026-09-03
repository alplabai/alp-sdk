/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sockets.h
 * @brief CC3501E TCP/UDP socket host helpers (opcodes 0x20..0x26).
 *
 * A minimal BSD-style socket API offloaded to the CC3501E's IP stack:
 * the host opens a handle, connects it, sends + receives bytes, then
 * closes it.  Each call is one worker-routed firmware op (the socket
 * bodies block on the lwIP core thread), so every wrapper is a
 * poll-by-repeat over the bridge like the Wi-Fi getters.  v1 is
 * IPv4-only; addresses are 4 octets in network (big-endian) order.
 *
 * SERVING (protocol v9).  @ref cc3501e_sock_bind + @ref cc3501e_sock_listen
 * turn a socket into a passive one so an application on the host can serve
 * over the module's own soft-AP -- an embedded web console on a product with
 * no Ethernet PHY, for instance.  There is deliberately NO accept call:
 * accept() blocks, and this bridge is strict request/reply lockstep, so a
 * blocking opcode would hold the firmware worker (and READY LOW, and the whole
 * link) for as long as no client connects.  Each inbound connection instead
 * arrives as an @ref ALP_CC3501E_EVT_SOCK_ACCEPTED entry on the polled event
 * queue, carrying a ready-to-use handle:
 *
 * @code
 * // once, at startup: AP up, then a listening socket.  The security byte is
 * // 1 = WPA2-PSK (see alp_cc3501e_wifi_connect_t::security), and it comes
 * // BEFORE the passphrase.
 * cc3501e_wifi_ap_start(ctx, "my-device", 1u, "secret", 10000);
 * cc3501e_sock_open(ctx, ALP_CC3501E_SOCK_FAMILY_IPV4,
 *                   ALP_CC3501E_SOCK_TYPE_STREAM, 0, &srv, 5000);
 * cc3501e_sock_bind(ctx, srv, NULL, 80, 5000);   // NULL ip = every interface
 * cc3501e_sock_listen(ctx, srv, 4, 5000);
 *
 * // in the event callback, for each accepted connection.  The signature is
 * // cc3501e_event_cb_t -- note @c size_t len, not uint8_t.
 * void on_event(uint8_t opcode, const uint8_t *payload, size_t len, void *user)
 * {
 *         alp_cc3501e_sock_accepted_evt_t ev;
 *         if (opcode != ALP_CC3501E_EVT_SOCK_ACCEPTED) return;
 *         if (cc3501e_sock_accepted_decode(payload, len, &ev) != ALP_OK) return;
 *         // ev.handle is a normal socket: recv the request, send the reply,
 *         // then cc3501e_sock_close() it.  The host owns it from here.
 * }
 * @endcode
 *
 * The host must be polling (@ref cc3501e_poll_events) for connections to be
 * delivered at all, and it OWNS every accepted handle -- the firmware never
 * closes one on the host's behalf, so a serve loop that forgets
 * @ref cc3501e_sock_close leaks firmware sockets until the IP stack runs out.
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
 * @brief Bind a socket to a local endpoint (SOCK_BIND, opcode 0x25).
 *
 * Assigns the local address and port a socket serves from, before
 * @ref cc3501e_sock_listen makes it passive.  Worker-routed poll-by-repeat.
 *
 * @param ctx         Initialised driver context.
 * @param handle      Socket handle from @ref cc3501e_sock_open.
 * @param ip          Local address, 4 octets in network order, or NULL for
 *                    INADDR_ANY (every interface).  NULL is the right choice
 *                    for a server on the soft-AP: the AP address does not
 *                    exist until the role is up.
 * @param port        Local port, host byte order.  Pass the port you serve
 *                    on; 0 asks the stack for an ephemeral port, which a
 *                    server has no way to publish.
 * @param timeout_ms  Upper bound on the poll-by-repeat budget.
 * @return ALP_OK once bound; ALP_ERR_NOT_READY if the firmware IP stack is
 *         unavailable (stub / no-Wi-Fi build); mapped error otherwise (a port
 *         already in use surfaces as the firmware's IO mapping).
 */
alp_status_t cc3501e_sock_bind(cc3501e_t    *ctx,
                               uint16_t      handle,
                               const uint8_t ip[4],
                               uint16_t      port,
                               uint32_t      timeout_ms);

/**
 * @brief Make a bound socket passive (SOCK_LISTEN, opcode 0x26).
 *
 * After this returns the firmware accepts inbound connections on the socket
 * and delivers each one as an @ref ALP_CC3501E_EVT_SOCK_ACCEPTED event on the
 * polled queue -- see the serve loop in this file's header comment.  This call
 * does NOT block waiting for a connection, and there is no accept counterpart.
 * Worker-routed poll-by-repeat.
 *
 * The firmware tracks a fixed number of listening sockets (4 in the current
 * build). Asking for one past that is a PERMANENT refusal, not a transient:
 * it surfaces as @c ALP_ERR_BUSY, which this driver treats as terminal and
 * does not retry — close a listener you no longer serve before opening
 * another.
 *
 * @param ctx         Initialised driver context.
 * @param handle      Bound STREAM socket (@ref cc3501e_sock_open then
 *                    @ref cc3501e_sock_bind).
 * @param backlog     Maximum queued, not-yet-accepted connections; 0 asks the
 *                    firmware for its default.
 * @param timeout_ms  Upper bound on the poll-by-repeat budget.
 * @return ALP_OK once listening; ALP_ERR_BUSY if the firmware has no free
 *         listening slot (terminal — see above, do not retry);
 *         ALP_ERR_NOT_READY on the stub build; mapped error otherwise.
 */
alp_status_t
cc3501e_sock_listen(cc3501e_t *ctx, uint16_t handle, uint8_t backlog, uint32_t timeout_ms);

/**
 * @brief Decode an EVT_SOCK_ACCEPTED event payload.
 *
 * Copies the packed wire bytes an event callback is handed into a properly
 * aligned @ref alp_cc3501e_sock_accepted_evt_t.  Use this rather than casting
 * the callback's @c payload pointer: it points into the driver's event buffer
 * at whatever offset the entry landed on, so the cast is unaligned.
 *
 * @param payload  Event payload bytes as delivered to the callback.
 * @param len      Payload length as delivered to the callback.
 * @param out      Receives the decoded event.
 * @return ALP_OK on success; ALP_ERR_INVAL if @p payload or @p out is NULL, or
 *         @p len is shorter than the event (a truncated entry -- do not use
 *         @p out in that case).
 */
alp_status_t cc3501e_sock_accepted_decode(const uint8_t                   *payload,
                                          size_t                           len,
                                          alp_cc3501e_sock_accepted_evt_t *out);

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
 *
 * @warning STACK COST.  This call places a full ALP_CC3501E_MAX_PAYLOAD reply
 *          buffer (4096 B as of protocol v5) on the CALLER's stack, and
 *          @ref cc3501e_sock_send does the same for its request.  A thread that
 *          calls either needs a stack sized for it: the bring-up example had to
 *          go from CONFIG_MAIN_STACK_SIZE 16384 to 32768 when MAX_PAYLOAD moved
 *          2048 -> 4096, and a 4096-byte main stack -- the default in most of the
 *          aen-cc3501e-* examples, none of which call the socket API -- overflows
 *          on the first call.  The failure is abrupt and gives no hint at the
 *          cause: `E: >>> ZEPHYR FATAL ERROR 2: Stack overflow on CPU 0` at the
 *          call site.  Examples that do not use sockets are unaffected and do not
 *          need the larger stack.
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
