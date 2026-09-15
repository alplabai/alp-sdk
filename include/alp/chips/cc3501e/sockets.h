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
 * @note **Handles carry the link epoch, and `timeout_ms` does not bound a
 *       recovery (issue #2126).**  A handle's upper byte is the link epoch it
 *       was opened under, so print it as `0x%04x` and mask with `& 0xFF`
 *       before comparing it against a firmware-side number.  After the driver
 *       warm-resets a wedged bridge, every handle from before that reset fails
 *       closed with `ALP_ERR_NOT_READY` instead of addressing whichever socket
 *       now holds that number; open new ones.  A call that fails with nothing
 *       decoded off the wire can also run ~12-18 s of probing plus ~3.5 s of
 *       reset past its own @p timeout_ms before returning -- see
 *       `CONFIG_ALP_SDK_CC3501E_AUTO_RECOVER`.
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
 *         if (cc3501e_sock_accepted_decode(payload, len, ctx->link_epoch, &ev) != ALP_OK) return;
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
 * @param epoch    The bridge ctx's CURRENT @c link_epoch (issue #2126) --
 *                 encoded into @p out's @c listen_handle and @c handle, the
 *                 same as every OTHER fresh firmware handle this driver
 *                 hands out (see cc3501e_sock_open()). Pass @c fw->link_epoch
 *                 (the ctx this event's companion was registered on); a
 *                 mismatched or stale value here would make cc3501e_sock_recv()
 *                 etc. refuse handles this call just minted.
 * @param out      Receives the decoded event.
 * @return ALP_OK on success; ALP_ERR_INVAL if @p payload or @p out is NULL, or
 *         @p len is shorter than the event (a truncated entry -- do not use
 *         @p out in that case).
 */
alp_status_t cc3501e_sock_accepted_decode(const uint8_t                   *payload,
                                          size_t                           len,
                                          uint8_t                          epoch,
                                          alp_cc3501e_sock_accepted_evt_t *out);

/**
 * @brief Send bytes on a socket (SOCK_SEND, opcode 0x22).
 *
 * Queues @p len bytes on the socket, re-issuing the remainder as its own
 * short transaction until every byte is queued or @p timeout_ms elapses --
 * the firmware's SOCK_SEND is non-blocking (MSG_DONTWAIT), so a full peer
 * receive buffer reports 0 bytes queued rather than blocking, and a short
 * queue is the normal outcome under backpressure, not an edge case. @p len is
 * bounded by one frame (<= ALP_CC3501E_MAX_PAYLOAD - 8, the send-header
 * size); larger buffers must be split by the caller.  Worker-routed
 * poll-by-repeat, looped.
 *
 * If a frame's own attempt times out while the firmware may genuinely have
 * accepted it and not finished, this function does not simply abandon it:
 * it re-polls that SAME frame for a short additional bounded grace to
 * collect the outcome before giving up (this grace is NOT entered for a
 * lock-acquire timeout on the very first attempt -- that means no frame
 * reached the bridge at all, an unambiguous @c ALP_ERR_BUSY returned
 * directly). Without the grace, the bridge's single worker slot could hand
 * an abandoned-but-later-completed job to the NEXT, unrelated call instead;
 * a bridge with cc3501e-bridge-firmware#107 (PR #134) discards such a job
 * once a different-seq request arrives, so a later call is never handed a
 * stale count, but that job's
 * bytes were still queued -- this grace is the only way left to learn how
 * many. If even the grace expires, @p sent_out is a LOWER BOUND, not an
 * exact count, and the socket's stream position from here on is UNKNOWN:
 * do not resume from a lower bound -- retrying assuming it is exact
 * duplicates bytes the bridge already queued. Close the socket instead. If
 * the grace instead collects a genuine, definitive non-OK status (e.g. a
 * decoded device-side error), that status is returned directly -- but
 * @p sent_out is NOT exact even then: the bridge's own lwIP stack can queue
 * bytes and still fail the send afterwards (tcp_write() succeeding, a later
 * tcp_output() returning an error), so a decoded device error on THIS frame
 * means @p sent_out only covers the earlier, already-collected frames, this
 * frame's own byte count is unknowable, and the stream position from here on
 * is just as unknown as the lower-bound case above -- close the socket
 * instead of resuming.
 *
 * @param ctx         Initialised driver context.
 * @param handle      Socket handle from @ref cc3501e_sock_open.
 * @param data        Payload bytes to send.
 * @param len         Number of bytes in @p data.
 * @param sent_out    Receives the TOTAL accepted byte count across every
 *                    iteration (may be NULL) -- @p len on ALP_OK; on
 *                    ALP_ERR_TIMEOUT, an EXACT partial count if the last
 *                    in-flight frame's outcome was collected (see above), or
 *                    a LOWER BOUND -- safe only to report, NOT to resume
 *                    from -- if even the collection grace expired; on a
 *                    decoded device error, covers only the earlier frames
 *                    already collected -- the failing frame's own count is
 *                    unknowable (the bridge's lwIP stack can queue bytes and
 *                    still fail the send), so treat it like the LOWER BOUND
 *                    case: safe to report, not to resume from.
 * @param timeout_ms  Upper bound on the total send budget, across every
 *                    iteration -- may be modestly exceeded by one bounded
 *                    collection grace (see above) to avoid leaving a job
 *                    uncollected.
 * @return ALP_OK only once all @p len bytes are queued; ALP_ERR_TIMEOUT with
 *         @p sent_out as described above if the budget (plus at most one
 *         grace) elapses first -- only resume sending from an EXACT
 *         @p sent_out; a LOWER BOUND means the stream position is unknown,
 *         so close the socket instead; ALP_ERR_BUSY if a transport-lock
 *         timeout on the very first attempt meant nothing was sent;
 *         ALP_ERR_IO if a decoded reply's queued-byte count is malformed;
 *         ALP_ERR_INVAL if @p len exceeds one frame; ALP_ERR_NOT_READY on
 *         the stub build; mapped error otherwise, with @p sent_out again only
 *         a LOWER BOUND -- close the socket, do not resume.
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
 * Requests up to @p cap bytes from the socket's receive queue into @p buf.
 * What a zero-length result (@p recv_len_out set to 0 with ALP_OK) MEANS
 * depends on which bridge is on the other end of the link:
 *   - Against v0.8.0 (`prebuilt/cc3501e-v0.8.0.bin`, what `prebuilt/`
 *     actually publishes today, predates cc3501e-bridge-firmware#140):
 *     zero is AMBIGUOUS -- no data was available within the firmware's
 *     receive window, or the peer closed the connection. The caller polls
 *     again to distinguish (or stops on a subsequent zero after a close).
 *   - Against a bridge carrying #140 (merged to `main`; cut as v0.9.0 but
 *     not yet released or bench-verified): EOF is sticky on the worker
 *     path and a reset is reported on the ring socket, so a zero-length
 *     result is UNAMBIGUOUS -- it means the peer actually closed. No
 *     poll-again dance is needed.
 * Worker-routed poll-by-repeat over the bridge.
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
