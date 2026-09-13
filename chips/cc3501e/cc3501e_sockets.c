/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * CC3501E TCP/UDP socket host helpers (opcodes 0x20..0x26).  See
 * <alp/chips/cc3501e/sockets.h> for the public API.
 *
 * Each wraps cc3501e_request over the packed wire structs in
 * <alp/protocol/cc3501e.h>.  The firmware worker-routes every socket
 * op (the lwIP bodies block), so each is a poll-by-repeat that re-
 * issues the SAME frame while the firmware reports RESP_ERR_BUSY (op
 * in flight) or the bridge reads IO (down mid-op), until it resolves.
 * v1 IPv4-only; addresses are 4 octets in network order.
 *
 * CMD_SOCK_SEND carries a retry seq (proto v7) in what used to be an
 * always-zero reserved byte, because a re-issued poll is otherwise
 * indistinguishable from a brand-new request: once the firmware's worker
 * completes a send and frees its job slot, the host's next byte-identical
 * poll reads as a NEW request and the payload is transmitted AGAIN
 * (alp-sdk#1746, root-caused in cc3501e-bridge-firmware#88).  See
 * cc3501e_sock_send()'s seq assignment below for the host half; the
 * firmware caches (seq, reply) and serves a matching retry without
 * re-submitting.
 *
 * cc3501e_sock_send() additionally loops over its OWN remaining-bytes budget
 * (cc3501e-bridge-firmware#107): with the firmware's SOCK_SEND now
 * non-blocking (MSG_DONTWAIT), a short queue is the normal outcome under
 * backpressure, not the near-impossible case it used to be.  That loop is
 * layered on top of, not instead of, the poll-by-repeat retry described
 * above -- see the seq comment inside cc3501e_sock_send() for how the two
 * stay disambiguated.
 */

#include <string.h>
#include <stdint.h>

#include "cc3501e_internal.h"

/* Wire header size of alp_cc3501e_sock_send_t (handle | flags | seq |
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

alp_status_t cc3501e_sock_bind(cc3501e_t    *ctx,
                               uint16_t      handle,
                               const uint8_t ip[4],
                               uint16_t      port,
                               uint32_t      timeout_ms)
{
	/* SOCK_BIND (0x25) wire = alp_cc3501e_sock_bind_t: handle(LE16) |
	 * reserved(2) | local sock_addr { family | reserved | port(LE16) | addr[16] }
	 * -- byte-for-byte the SOCK_CONNECT layout, only the endpoint's meaning
	 * differs.  ip == NULL means INADDR_ANY (bind every interface), which is
	 * what a server on the soft-AP wants: the AP address only exists once the
	 * role is up, and binding it explicitly would race the role-up. */
	uint8_t p[24];
	memset(p, 0, sizeof(p));
	p[0] = (uint8_t)(handle & 0xFFu);
	p[1] = (uint8_t)((handle >> 8) & 0xFFu);
	p[4] = (uint8_t)ALP_CC3501E_SOCK_FAMILY_IPV4; /* local.family */
	p[6] = (uint8_t)(port & 0xFFu);               /* local.port (LE16, host order) */
	p[7] = (uint8_t)((port >> 8) & 0xFFu);
	if (ip != NULL) memcpy(&p[8], ip, 4); /* local.addr[0..3]; [4..15] stay zero */
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_SOCK_BIND, p, sizeof(p), NULL, 0, NULL, timeout_ms);
}

alp_status_t
cc3501e_sock_listen(cc3501e_t *ctx, uint16_t handle, uint8_t backlog, uint32_t timeout_ms)
{
	/* SOCK_LISTEN (0x26) wire = alp_cc3501e_sock_listen_t { handle(LE16) |
	 * backlog | reserved }.  There is no accept call to pair with this: each
	 * inbound connection is delivered as an EVT_SOCK_ACCEPTED entry on the
	 * polled event queue (cc3501e_poll_events), carrying a ready-to-use handle.
	 * See <alp/chips/cc3501e/sockets.h> for the serve loop that implies. */
	uint8_t p[4] = { (uint8_t)(handle & 0xFFu), (uint8_t)((handle >> 8) & 0xFFu), backlog, 0u };
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_SOCK_LISTEN, p, sizeof(p), NULL, 0, NULL, timeout_ms);
}

/* Zero-progress back-off for the remainder loop below (cc3501e-bridge-firmware
 * #107): once the firmware's SOCK_SEND moved from a blocking lwip_send() to
 * MSG_DONTWAIT, a full peer receive buffer now reports RESP_OK with 0 bytes
 * queued instead of blocking.  Retrying that instantly would hammer the SPI
 * link on a peer that simply isn't reading; a short, budget-bounded sleep
 * between zero-progress attempts keeps this a poll, not a hot loop. */
#define CC3501E_SOCK_SEND_BACKOFF_MS 20u

alp_status_t cc3501e_sock_send(cc3501e_t     *ctx,
                               uint16_t       handle,
                               const uint8_t *data,
                               size_t         len,
                               size_t        *sent_out,
                               uint32_t       timeout_ms)
{
	if (data == NULL && len > 0u) return ALP_ERR_INVAL;
	/* #2035: leave room for the MAJOR-4 CRC trailer cc3501e_request() appends
	 * once this ctx has negotiated it -- see cc3501e_ble_gatt_register()'s
	 * comment in cc3501e_ble.c. */
	if (len > (size_t)(ALP_CC3501E_MAX_PAYLOAD - CC3501E_SOCK_SEND_HDR - ALP_CC3501E_CRC_BYTES))
		return ALP_ERR_INVAL;
	if (sent_out != NULL) *sent_out = 0u;

	/* SOCK_SEND (0x22) wire = alp_cc3501e_sock_send_t (8 B) + inline data; reply
	 * DATA = uint16_t LE queued-byte count.
	 *
	 * cc3501e-bridge-firmware#107: the firmware's SOCK_SEND handler moved from
	 * a blocking lwip_send() to MSG_DONTWAIT, because the blocking form parked
	 * the worker -- and READY along with it -- for as long as a peer declined
	 * to read, wedging every opcode on the bridge for that whole span.
	 * MSG_DONTWAIT never blocks: a full send buffer now reports 0 bytes
	 * queued, ALP_OK, immediately, where the old firmware would eventually
	 * have blocked until it could report the full count. Callers that never
	 * checked *sent_out (e.g.
	 * src/zephyr/console/alp_console_companion_sock.c) relied on a short
	 * queue being effectively impossible; it is now the normal case under
	 * backpressure. So THIS function absorbs it: it keeps issuing the
	 * remainder as its own short transaction -- never parking the firmware --
	 * until every byte of @p len is queued or @p timeout_ms elapses, which
	 * keeps every existing caller's assumption true without touching them. */
	/* Per-context scratch, NOT a 4 KB stack frame.  This was
	 * `uint8_t p[ALP_CC3501E_MAX_PAYLOAD]` -- 4096 bytes on the caller's stack.
	 * The Zephyr shell thread is CONFIG_SHELL_STACK_SIZE=2048, so
	 * `alp companion sock tcp-get` overflowed it deterministically and the
	 * application took a USAGE FAULT.  Issue #740 moved the scan/event decode
	 * buffers off the stack for this exact reason; the socket path was missed. */
	if (ctx->sock_busy) return ALP_ERR_BUSY;
	ctx->sock_busy = true;

	const uint8_t *remaining     = data;
	size_t         remaining_len = len;
	size_t         total_sent    = 0u;
	/* Real elapsed time, not a declared per-attempt cost -- the same #2035 bug
	 * already fixed once in cc3501e_wifi_connect()'s status-poll loop (see its
	 * `elapsed_ms` comment): a ledger debited by a fixed per-attempt estimate
	 * drains faster than the clock whenever an attempt actually costs less
	 * than its estimate. Here the deadline is read ONCE off the same
	 * monotonic clock poll_by_repeat() itself uses, and every iteration below
	 * budgets whatever is genuinely left of it. */
	const uint64_t deadline_ms = alp_uptime_ms() + (uint64_t)timeout_ms;
	alp_status_t   s;

	for (;;) {
		uint8_t *p = ctx->sock_buf;
		p[0]       = (uint8_t)(handle & 0xFFu);
		p[1]       = (uint8_t)((handle >> 8) & 0xFFu);
		p[2]       = 0u; /* flags (MORE bit unused here) */
		/* p[3] is alp_cc3501e_sock_send_t.seq (formerly reserved, always 0
		 * through v6) -- a retry seq (proto v7, alp-sdk#1746 /
		 * cc3501e-bridge-firmware#88). ONE seq per ITERATION of this loop, not
		 * per call: each iteration carries different remaining bytes, so it is
		 * a NEW logical send and must get a NEW seq -- reusing the previous
		 * iteration's seq for different data would make the firmware serve its
		 * stale cached reply instead of submitting the new bytes, silently
		 * dropping them. poll_by_repeat() below re-sends THIS EXACT buffer,
		 * unmodified, on every one of its OWN internal BUSY/IO retries of one
		 * iteration -- that keeps the seq constant across THOSE retries
		 * automatically, which is what lets the firmware serve a repeated poll
		 * of the SAME iteration from its cache instead of re-submitting (and
		 * re-transmitting) the payload. Pre-increment, exactly like spi1_seq:
		 * a fresh ctx's first send is seq 1, and the counter free-runs from
		 * there.
		 *
		 * WRAP: sock_send_seq is a uint8_t, so it wraps 255 -> 0 after 256
		 * increments (defined unsigned overflow, not UB). That cannot collide
		 * with the firmware's cache: the cache is a SINGLE entry holding only
		 * the immediately-preceding completed send's seq, never anything from
		 * 256 increments ago, so a wrapped-around repeat is never mistaken for
		 * a retry of an old send. */
		p[3] = ++ctx->sock_send_seq;
		p[4] = (uint8_t)(remaining_len & 0xFFu);
		p[5] = (uint8_t)((remaining_len >> 8) & 0xFFu);
		p[6] = 0u;
		p[7] = 0u;
		if (remaining_len > 0u) memcpy(&p[CC3501E_SOCK_SEND_HDR], remaining, remaining_len);

		uint64_t now    = alp_uptime_ms();
		uint32_t budget = (now >= deadline_ms) ? 0u : (uint32_t)(deadline_ms - now);

		uint8_t reply[2] = { 0 };
		size_t  got      = 0;
		s                = poll_by_repeat(ctx,
		                                  ALP_CC3501E_CMD_SOCK_SEND,
		                                  p,
		                                  CC3501E_SOCK_SEND_HDR + remaining_len,
		                                  reply,
		                                  sizeof(reply),
		                                  &got,
		                                  budget);
		if (s != ALP_OK) break; /* transport/firmware error -- return it now, as today */

		size_t queued = (got >= 2u) ? (size_t)((uint16_t)reply[0] | ((uint16_t)reply[1] << 8)) : 0u;
		if (queued > remaining_len) queued = remaining_len; /* defensive clamp */
		total_sent += queued;
		remaining_len -= queued;
		if (remaining_len == 0u) break; /* every byte of len is now queued */
		if (queued > 0u) remaining += queued;

		now = alp_uptime_ms();
		if (now >= deadline_ms) {
			s = ALP_ERR_TIMEOUT;
			break;
		}
		if (queued == 0u) {
			/* Zero progress this iteration: back off before the next one,
			 * bounded to whatever budget remains so the sleep itself cannot
			 * blow the caller's timeout_ms. */
			uint64_t left    = deadline_ms - now;
			uint32_t backoff = (left < (uint64_t)CC3501E_SOCK_SEND_BACKOFF_MS)
			                       ? (uint32_t)left
			                       : CC3501E_SOCK_SEND_BACKOFF_MS;
			alp_delay_ms(backoff);
			if (alp_uptime_ms() >= deadline_ms) {
				s = ALP_ERR_TIMEOUT;
				break;
			}
		}
		/* Partial progress: loop again immediately -- no back-off needed, the
		 * peer just demonstrated it is reading. */
	}

	ctx->sock_busy = false;
	if (sent_out != NULL) *sent_out = total_sent;
	return s;
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
	size_t want = cap;
	/* Fill the frame: MAX_PAYLOAD - CC3501E_SOCK_RECV_RESP_HDR - 1 = 487 data
	 * bytes.
	 *
	 * This was pinned at 256 because larger replies desynced the bridge totally
	 * -- silicon-measured 2026-08-24, streaming a 262144 B HTTP body the server
	 * demonstrably delivered: cap 128 -> 12779 B/s, cap 256 -> 25480 B/s, cap
	 * 400/486/487 -> 0 B/s with the socket layer left unusable.  The failure was
	 * always a bad reply HEADER on the FOLLOWING transaction (hdr_bad=669,
	 * xfer_fail=0, busy=0), which is why it looked like a hard size limit.
	 *
	 * It was not a size limit.  That comment named the READY line as "the leading
	 * remaining suspect", and it was right: READY (CC35 GPIO17 -> Alif P2_6) read
	 * 0 only because the Alif pad's INPUT BUFFER was never enabled -- an
	 * input-enable pinctrl group turns it on (see the board overlay).  With READY
	 * actually readable and cc3501e_reply_gate() waiting for the drop-then-rise
	 * EDGE, the host stops clocking into an un-armed slave and 487 works:
	 * 262405 B in 883 ms = 297174 B/s, over a link running ping_fail=0.
	 *
	 * The cap therefore belongs to the frame, not to a magic number.  A board
	 * with no readable READY line still falls back to fixed settle gaps, where
	 * the old 256 limit would apply -- re-measure on silicon before trusting
	 * this on such a board. */
	const size_t want_max = (size_t)ALP_CC3501E_MAX_PAYLOAD - CC3501E_SOCK_RECV_RESP_HDR - 1u;
	if (want > want_max) want = want_max;

	/* SOCK_RECV (0x23) wire = alp_cc3501e_sock_recv_t { handle(LE16) | max_len(LE16) }. */
	uint8_t p[4] = { (uint8_t)(handle & 0xFFu),
		             (uint8_t)((handle >> 8) & 0xFFu),
		             (uint8_t)(want & 0xFFu),
		             (uint8_t)((want >> 8) & 0xFFu) };

	/* Per-context scratch, NOT a 4 KB stack frame -- see the note in
	 * cc3501e_sock_send() above and cc3501e_t's sock_buf comment. */
	if (ctx->sock_busy) return ALP_ERR_BUSY;
	ctx->sock_busy     = true;
	uint8_t     *reply = ctx->sock_buf;
	size_t       got   = 0;
	alp_status_t s     = poll_by_repeat(ctx,
	                                    ALP_CC3501E_CMD_SOCK_RECV,
	                                    p,
	                                    sizeof(p),
	                                    reply,
	                                    sizeof(ctx->sock_buf),
	                                    &got,
	                                    timeout_ms);
	ctx->sock_busy     = false;
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

alp_status_t cc3501e_sock_accepted_decode(const uint8_t                   *payload,
                                          size_t                           len,
                                          alp_cc3501e_sock_accepted_evt_t *out)
{
	if (payload == NULL || out == NULL) return ALP_ERR_INVAL;
	if (len < sizeof(*out)) return ALP_ERR_INVAL; /* truncated entry -- out stays untouched */
	/* Field-by-field off the packed wire bytes, NOT a struct copy: the callback's
	 * payload pointer aims into the driver's event buffer at whatever offset this
	 * entry landed on, so it carries no alignment guarantee at all. */
	out->listen_handle = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
	out->handle        = (uint16_t)payload[2] | ((uint16_t)payload[3] << 8);
	out->peer_port     = (uint16_t)payload[4] | ((uint16_t)payload[5] << 8);
	out->peer_family   = payload[6];
	out->reserved      = payload[7];
	memcpy(out->peer_addr, &payload[8], sizeof(out->peer_addr));
	return ALP_OK;
}

alp_status_t cc3501e_sock_close(cc3501e_t *ctx, uint16_t handle, uint32_t timeout_ms)
{
	/* SOCK_CLOSE (0x24) wire = alp_cc3501e_sock_close_t { handle(LE16) | reserved }. */
	uint8_t p[4] = { (uint8_t)(handle & 0xFFu), (uint8_t)((handle >> 8) & 0xFFu), 0u, 0u };
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_SOCK_CLOSE, p, sizeof(p), NULL, 0, NULL, timeout_ms);
}
