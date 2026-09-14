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
 * between zero-progress attempts keeps this a poll, not a hot loop.
 *
 * Also reused below as the MINIMUM remaining budget worth starting a new
 * iteration over (alp-sdk#2035): less than one back-off's worth of time left
 * is not enough to plausibly get a fresh frame answered before the deadline
 * anyway, so starting one would just walk straight into the collection-grace
 * path below for no benefit -- stopping a little earlier is strictly better.
 * Checked at the TOP of every iteration but the first (which always runs
 * regardless of budget, matching poll_by_repeat()'s own "at least one
 * attempt" contract) -- specifically INCLUDING right after this back-off's
 * own sleep, not just before it: checking only beforehand and then sleeping
 * the full amount unconditionally could hand the next iteration a sliver as
 * small as zero (a tick landed exactly on the deadline) or a few ms (this
 * back-off ran right up against it), and firmware answers BUSY to any new
 * frame's first poll -- so that sliver would routinely time out into the
 * grace window below, making an overrun of timeout_ms the NORMAL ending
 * under backpressure instead of a rare edge. */
#define CC3501E_SOCK_SEND_BACKOFF_MS 20u

/* Bounded grace window used ONLY to COLLECT an already-submitted SOCK_SEND
 * frame's outcome after this function's own per-iteration poll_by_repeat()
 * call reports ALP_ERR_TIMEOUT (alp-sdk#2035): that status alone does not
 * mean nothing happened on the wire. A shrinking per-iteration budget can
 * hit a window where the firmware has already accepted this exact frame
 * (answered RESP_ERR_BUSY -- a genuinely retryable "still running", not a
 * reject) and, thanks to #107's now-non-blocking send, would resolve it
 * almost immediately -- but this function's OWN per-iteration deadline gives
 * up first. A bridge with cc3501e-bridge-firmware#107 (PR #134,
 * merged, not yet in a released blob) bounds, but
 * does not close, what
 * abandoning that frame here would risk: it discards a finished-but-
 * uncollected job outright once a request with a DIFFERENT seq arrives, so a
 * stale completion is never handed to the wrong caller as if it were that
 * caller's own reply -- but the abandoned frame's bytes were still
 * genuinely queued to the peer, and once the firmware discards that job the
 * count is gone for good. This grace is the only way left to learn it.
 *
 * NOT triggered by ALP_ERR_BUSY (alp-sdk#2035 review follow-up): a
 * transport-lock timeout on poll_by_repeat()'s FIRST attempt
 * (cc3501e_lock_acquire(), cc3501e_core.c) means no frame reached the
 * bridge at all -- an unambiguous BUSY, correct to return directly with no
 * grace. A lock timeout on a LATER attempt is retried within
 * poll_by_repeat() itself instead, since an earlier attempt already reached
 * the bridge by then; that retry can still end in ALP_ERR_TIMEOUT, which
 * DOES reach this grace like any other exhausted retry.
 *
 * Re-issuing the SAME frame (same seq, same remaining bytes -- see the seq
 * comment inside the loop below) for this bounded grace either (a) collects
 * the real completion the earlier BUSY promised, or (b) if nothing was ever
 * actually submitted (e.g. the whole per-iteration budget was consumed by a
 * transient link-down IO instead of a genuine BUSY), simply becomes the real
 * first submission -- both outcomes are correct, and reusing the identical
 * seq cannot duplicate or drop anything either way (see #1746 / #88's cache
 * mechanism this whole file already depends on).
 *
 * 250 ms = 5 x CC3501E_POLL_GAP_MS (the 50 ms steady-state poll cadence
 * poll_by_repeat() itself settles into, cc3501e_core.c): a worker job that
 * has ALREADY acknowledged BUSY and, under #107, has nothing left to do but
 * report, needs only to be polled a handful more times to be collected --
 * this does not need to cover a fresh worst-case op, only the tail of one
 * already in flight. If even this expires, cc3501e_sock_send() returns
 * ALP_ERR_TIMEOUT with *sent_out as a LOWER BOUND, not an exact count -- the
 * stream position from here is unknown, so the caller should close the
 * socket rather than resume (see <alp/chips/cc3501e/sockets.h>). If it
 * instead collects a genuine, definitive non-OK status (e.g. a decoded
 * device-side error), that status is returned directly and *sent_out is
 * exact, not a lower bound. */
#define CC3501E_SOCK_SEND_COLLECT_GRACE_MS 250u

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
	bool           first_iteration = true;

	for (;;) {
		if (!first_iteration) {
			/* alp-sdk#2035: do not start a new iteration once less than one
			 * back-off's worth of budget remains -- see
			 * CC3501E_SOCK_SEND_BACKOFF_MS's comment above. Checked here, at
			 * the TOP of every iteration but the first, so it re-reads the
			 * clock AFTER this iteration's own back-off sleep (below) too,
			 * not just before it. */
			uint64_t now_top  = alp_uptime_ms();
			uint64_t left_top = (now_top >= deadline_ms) ? 0u : (deadline_ms - now_top);
			if (left_top < (uint64_t)CC3501E_SOCK_SEND_BACKOFF_MS) {
				s = ALP_ERR_TIMEOUT;
				break;
			}
		}
		first_iteration = false;

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
		 * increments (defined unsigned overflow, not UB). That CAN alias a
		 * stale cache entry -- contrary to what this comment used to claim.
		 * (Firmware without #107 at all has no seq-vs-cache check either, so
		 * it can answer a brand-new send with a previous send's cached
		 * count on far less than a full wrap.) A bridge with
		 * cc3501e-bridge-firmware#107 (PR #134, merged, not yet in a
		 * released blob) invalidates its reply cache
		 * the instant a DIFFERENT seq is DISPATCHED, which narrows the
		 * window to 255 consecutive frames whose seq increments but which
		 * never reach that dispatch at all -- e.g. this driver reporting
		 * NOT_READY, a cc3501e_lock_acquire() timeout (nothing sent), or the
		 * link being down the whole time -- not any 255 frames. It does NOT
		 * close the window: if that happens, the 256th attempt's seq wraps
		 * back to a value the cache still holds, and that new send is
		 * answered with the stale cached count instead of executing. */
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

		if (s == ALP_ERR_TIMEOUT) {
			/* alp-sdk#2035: do not abandon a frame this attempt may have
			 * already gotten a genuine RESP_ERR_BUSY for -- collect it first.
			 * See CC3501E_SOCK_SEND_COLLECT_GRACE_MS's comment above for the
			 * full why. NOT entered on ALP_ERR_BUSY -- see that same comment
			 * for why a first-attempt lock timeout is unambiguous and
			 * returned directly below instead, with no grace needed. Re-sends
			 * the IDENTICAL `p` buffer untouched, so the seq (p[3]) and
			 * remaining_len are unchanged -- this is a retry of THIS
			 * iteration, not a new one. */
			uint8_t      grace_reply[2] = { 0 };
			size_t       grace_got      = 0;
			alp_status_t grace_s        = poll_by_repeat(ctx,
			                                             ALP_CC3501E_CMD_SOCK_SEND,
			                                             p,
			                                             CC3501E_SOCK_SEND_HDR + remaining_len,
			                                             grace_reply,
			                                             sizeof(grace_reply),
			                                             &grace_got,
			                                             CC3501E_SOCK_SEND_COLLECT_GRACE_MS);
			if (grace_s == ALP_OK) {
				if (grace_got < 2u) {
					/* Malformed reply, not a queue count -- see the identical
					 * got<2u handling below for the non-grace path. */
					s = ALP_ERR_IO;
				} else {
					size_t grace_queued =
					    (size_t)((uint16_t)grace_reply[0] | ((uint16_t)grace_reply[1] << 8));
					if (grace_queued > remaining_len) grace_queued = remaining_len;
					total_sent += grace_queued;
					remaining_len -= grace_queued;
					/* Collected -- the caller's timeout_ms already elapsed to get
					 * here, so ALP_OK is only warranted if that collection
					 * happened to finish the whole send; otherwise this is still
					 * a (now exact, not a lower bound -- the collected count is
					 * final) TIMEOUT. */
					s = (remaining_len == 0u) ? ALP_OK : ALP_ERR_TIMEOUT;
				}
			} else if (grace_s != ALP_ERR_TIMEOUT && grace_s != ALP_ERR_BUSY) {
				/* Grace decoded a genuine, definitive non-OK status -- e.g.
				 * ALP_ERR_IO for a real device-side error such as a peer
				 * reset. This frame is DONE, not merely timed out: surface
				 * the real status to the caller instead of masking it as
				 * ALP_ERR_TIMEOUT, and *sent_out (below) is exact, not a
				 * lower bound -- this frame will never resolve differently. */
				s = grace_s;
			} else {
				/* Grace itself timed out, or hit its OWN first-attempt lock
				 * timeout, or a decoded terminal reject -- none of those
				 * distinguish "the original frame is still pending" from
				 * "the grace poll never even reached the bridge" from here,
				 * and none of them resolve THIS frame either way. Report the
				 * caller's declared budget as exceeded; *sent_out (below) is
				 * a LOWER BOUND -- the job may still complete and later be
				 * discarded (not collected) once a different-seq request
				 * arrives. See <alp/chips/cc3501e/sockets.h>. */
				s = ALP_ERR_TIMEOUT;
			}
			break;
		}
		if (s != ALP_OK)
			break; /* terminal transport/firmware error, or a lock timeout
		                          * this attempt could not even take -- return it now,
		                          * as today (see CC3501E_SOCK_SEND_COLLECT_GRACE_MS's
		                          * comment for why ALP_ERR_BUSY specifically does not
		                          * enter the grace above). */

		if (got < 2u) {
			/* A decoded ALP_OK with fewer than 2 data bytes is not a valid
			 * queued-byte count -- a firmware/wire gap, not backpressure.
			 * Treating it as "0 queued" would fold it into the zero-progress
			 * back-off path below and spin against a malformed reply forever;
			 * report it plainly instead, same as cc3501e_sock_open() does for
			 * an equivalently short reply above. */
			s = ALP_ERR_IO;
			break;
		}
		size_t queued = (size_t)((uint16_t)reply[0] | ((uint16_t)reply[1] << 8));
		if (queued > remaining_len) queued = remaining_len; /* defensive clamp */
		total_sent += queued;
		remaining_len -= queued;
		if (remaining_len == 0u) break; /* every byte of len is now queued */
		if (queued > 0u) remaining += queued;

		if (queued == 0u) {
			/* Zero progress this iteration: back off before the next one --
			 * but only if that back-off itself fits the remaining budget.
			 * Sleeping the full CC3501E_SOCK_SEND_BACKOFF_MS unconditionally
			 * (alp-sdk#2035 review follow-up) could overshoot a small
			 * timeout_ms outright -- a 4 ms budget returning after 23 ms,
			 * not ~4 -- since this decision runs even on the very FIRST
			 * iteration, before the top-of-loop check above has ever had a
			 * chance to run. Checked here instead of clamping the sleep to
			 * whatever is left: a clamped, shorter-than-BACKOFF_MS sleep
			 * would still land back at the top of the loop and, per the
			 * top-of-loop check, immediately stop anyway -- returning
			 * TIMEOUT here directly is the same outcome without the
			 * pointless partial sleep. */
			uint64_t now2  = alp_uptime_ms();
			uint64_t left2 = (now2 >= deadline_ms) ? 0u : (deadline_ms - now2);
			if (left2 <= (uint64_t)CC3501E_SOCK_SEND_BACKOFF_MS) {
				s = ALP_ERR_TIMEOUT;
				break;
			}
			alp_delay_ms(CC3501E_SOCK_SEND_BACKOFF_MS);
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
	ctx->sock_busy = true;

	/* alp-sdk#2108: SOCK_RECV allocates its retry seq from its OWN
	 * ctx->sock_recv_seq counter, NOT the shared ctx->req_seq every other
	 * worker-routed opcode draws from via plain poll_by_repeat() -- see
	 * ctx->sock_recv_seq's comment in <alp/chips/cc3501e/core.h> for the full
	 * story (cc3501e-bridge-firmware#138 keys its receive replay caches on
	 * this seq + the socket handle; this counter is the groundwork that
	 * keeps that key unambiguous). ONE counter for the whole ctx is shared
	 * across every handle, so recovery is guaranteed only for an immediate
	 * retry on the SAME handle: a failed recv on one handle followed by a recv
	 * on another before the retry can still lose the first handle's bytes
	 * (cc3501e-bridge-firmware#136 tracks per-handle state).
	 *
	 * CANDIDATE, not yet committed: this is the value THIS call will use on
	 * the wire, but ctx->sock_recv_seq itself is only updated to it once
	 * this call reaches ALP_OK below with the reply actually decoded. A
	 * caller whose call instead returns ALP_ERR_TIMEOUT / ALP_ERR_IO / a
	 * short header (any early return past this point) leaves
	 * ctx->sock_recv_seq untouched, so the NEXT cc3501e_sock_recv() call --
	 * on ANY handle -- computes this SAME candidate again.
	 *
	 *   - Retried on the SAME handle next: the bridge (once
	 *     fix/sock-recv-retry-safe ships) sees the identical (seq, handle)
	 *     it already answered and replays that exact chunk instead of
	 *     committing a new one -- recovering bytes a lost/corrupted reply
	 *     would otherwise have dropped, with no grace loop needed here.
	 *   - A DIFFERENT handle is called first instead: that call also gets
	 *     this same candidate value, but with its OWN handle, so the
	 *     bridge's single-entry key does not match the failed call's entry
	 *     either way -- it commits fresh, exactly as any new request should.
	 *     Once THAT call succeeds and commits, the bridge's key has moved on
	 *     to the different handle, so a LATER retry of the original handle
	 *     gets a genuinely new seq and can no longer recover the original
	 *     lost chunk -- recovery is guaranteed only for an IMMEDIATE retry
	 *     of the same handle, not indefinitely. */
	const uint8_t candidate_seq =
	    (ctx->sock_recv_seq >= ALP_CC3501E_REQ_SEQ_LAST) ? 1u : (uint8_t)(ctx->sock_recv_seq + 1u);

	uint8_t     *reply = ctx->sock_buf;
	size_t       got   = 0;
	alp_status_t s     = poll_by_repeat_seq(ctx,
	                                        ALP_CC3501E_CMD_SOCK_RECV,
	                                        p,
	                                        sizeof(p),
	                                        reply,
	                                        sizeof(ctx->sock_buf),
	                                        &got,
	                                        timeout_ms,
	                                        candidate_seq);
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
	/* Commit now, not before: everything above this line succeeded, so this
	 * call's candidate is genuinely spent -- see the comment at its
	 * computation above for why an early return past this point must NOT
	 * reach here. */
	ctx->sock_recv_seq = candidate_seq;
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
