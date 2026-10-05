/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-side driver for the GD32G553 supervisor MCU bridge on V2N.
 *
 * The wire protocol is in docs/gd32-bridge-protocol.md.  The firmware
 * counterpart that implements the same command-handler table lives
 * under gd32-bridge-firmware:.
 *
 * This file is deliberately Zephyr-agnostic: all bus access goes
 * through <alp/peripheral.h> so the same source compiles into either
 * the Zephyr backend or the future baremetal backend.  The chip
 * driver does not introduce any platform timing primitives -- the
 * tens-of-microseconds gap between alp_spi_write and alp_spi_read
 * naturally falls out of the host-side function-call overhead and
 * is documented as the firmware's reply-staging window in
 * docs/gd32-bridge-protocol.md §4.1.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "alp/chips/gd32g553.h"
#include "alp/protocol/crc16.h"

/* ----------------------------------------------------------------- */
/* CRC-16 / CCITT-FALSE  (poly 0x1021, init 0xFFFF, non-reflected,    */
/* xor-out 0x0000).  Reference vector: "123456789" -> 0x29B1.         */
/* Matches Zephyr's `crc16_itu_t(0xFFFF, ...)` and the matching       */
/* gd32-bridge-firmware:src/protocol.c implementation byte-for-byte.  */
/* alp_crc16_ccitt_false() (<alp/protocol/crc16.h>) is this exact     */
/* bitwise algorithm, header-only and stddef/stdint-only, so pulling  */
/* it in here does not add a Zephyr dependency to this deliberately   */
/* Zephyr-agnostic file.                                              */
/* ----------------------------------------------------------------- */

/* ----------------------------------------------------------------- */
/* Status-byte translation: wire encoding (unsigned) -> alp_status_t  */
/* (signed, negative-numbered).  See docs/gd32-bridge-protocol.md §6. */
/* ----------------------------------------------------------------- */

static alp_status_t status_from_wire(uint8_t s)
{
	switch (s) {
	case 0x00u:
		return ALP_OK;
	case 0x01u:
		return ALP_ERR_INVAL;
	case 0x02u:
		return ALP_ERR_NOT_READY;
	case 0x03u:
		return ALP_ERR_BUSY;
	case 0x04u:
		return ALP_ERR_TIMEOUT;
	case 0x05u:
		return ALP_ERR_IO;
	case 0x06u:
		return ALP_ERR_NOSUPPORT;
	case 0x07u:
		return ALP_ERR_NOMEM;
	case 0x08u:
		return ALP_ERR_OUT_OF_RANGE;
	case 0x80u: /* I2C: no pending command since last START */
		return ALP_ERR_NOT_READY;
	default:
		return ALP_ERR_IO;
	}
}

/* Little-endian uint32 read / write helpers. */
static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)((v >> 8) & 0xFFu);
	p[2] = (uint8_t)((v >> 16) & 0xFFu);
	p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ----------------------------------------------------------------- */
/* Envelope sizes                                                     */
/* ----------------------------------------------------------------- */

/* Maximum wire-side payload either direction.  Driven by the largest
 * of (a) ADC_STREAM_READ which can reply up to
 * `1 + GD32G553_BRIDGE_ADC_STREAM_READ_MAX * 2` = 65 bytes (count
 * byte + u16 samples), and (b) ADC_DSP_STAGE_PUSH whose request
 * envelope carries a 7-byte chunk header plus up to
 * `GD32G553_BRIDGE_ADC_DSP_MAX_CHUNK_BYTES` = 58 bytes of stage
 * payload = 65 bytes total.  Matches the firmware-side
 * `GD32_BRIDGE_MAX_PAYLOAD_BYTES` so host + firmware serialise the
 * same wire envelopes; legacy small opcodes (ADC_READ at 17 bytes,
 * GET_BUILD_ID at 20 bytes, etc.) sit comfortably below. */
#define GD32G553_MAX_PAYLOAD_BYTES (1u + (GD32G553_BRIDGE_ADC_STREAM_READ_MAX * 2u))

/* The SPI frame buffers live in the context (BIG_FRAME frames reach
 * 256 B); the I2C buffers below stay at the 65 B base envelope.  I2C
 * frames: CMD + payload + CRC. */
#define GD32G553_MAX_I2C_WRITE_BYTES \
	(1u /* reg=0x00 */ + 1u /* CMD */ + GD32G553_MAX_PAYLOAD_BYTES + 2u /* CRC */)

#define GD32G553_MAX_I2C_READ_BYTES (1u /* STATUS */ + GD32G553_MAX_PAYLOAD_BYTES + 2u /* CRC */)

/* ----------------------------------------------------------------- */
/* Transport: SPI                                                     */
/* ----------------------------------------------------------------- */

/* Reply re-read schedule.  The slave stages the reply inside the
 * REQUEST transaction's CS-rising handler; for slow handlers (ADC
 * conversion ~7 us/sample, TRNG conditioning, OTA FMC programming
 * ~70-140 us/chunk) the host's first reply read can land before the
 * reply is armed and clock idle/stale bytes instead.  Such a miss
 * does NOT consume the staged reply -- the slave's drain path rewinds
 * its staging cursor and re-arms the TX DMA on every CS cycle
 * (firmware >= v0.2.1; silicon-validated 2026-06-04) -- so re-READING
 * is always safe (no re-dispatch, no side effects).
 *
 * The schedule has two parts:
 *
 * 1. A fixed STAGING GAP before the first read.  The slave's CS-rising
 *    handler re-initialises its SPI peripheral (RCU flush), decodes,
 *    dispatches and re-arms both DMA channels before the reply exists
 *    -- a floor of a few tens of microseconds even for trivial
 *    handlers (bench 2026-06-04: at a ~15 us effective gap even
 *    GET_VERSION missed its first read every time, and each miss
 *    costs a wasted drain transaction plus a ladder wait, which is
 *    strictly worse than waiting out the floor).  35 us covers the
 *    rising path + trivial handlers; commands with real work fall
 *    through to the ladder.
 *
 * 2. A short-first backoff ladder between re-reads: cheap early
 *    retries keep ADC-burst-class handlers near the wire floor; the
 *    geometric tail covers OTA flash programming, bounding the total
 *    wait at ~3.2 ms. */
#define GD32G553_REPLY_STAGING_GAP_US 35u
static const uint16_t gd32g553_reply_retry_us[] = { 25u, 50u, 100u, 200u, 400u, 800u, 1600u };
#define GD32G553_REPLY_READ_TRIES \
	(1u + (sizeof(gd32g553_reply_retry_us) / sizeof(gd32g553_reply_retry_us[0])))

/* A reply whose payload length depends on its own bytes (READ2's `got`,
 * BATCH's per-op entries) supplies a decoder: given the payload bytes and
 * how many were clocked, return the payload length, or REPLY_LEN_INVALID
 * when the bytes are not a legal reply.  The CRC then sits right after
 * that many bytes. */
typedef size_t (*reply_len_fn)(const uint8_t *payload, size_t avail, const void *arg);
#define REPLY_LEN_INVALID ((size_t)-1)

/* Staging gap for one command.  ADC conversions run inside the request's
 * CS-rising handler at ~18-20 us per SAMPLE on top of the base rising
 * path (bench-bracketed 2026-06-04 from the miss boundary at two sample
 * counts and three gap sizes) -- too slow for the host to wait out in
 * full (chasing it with a sized-to-cover gap measured WORSE than letting
 * the ladder's first rung catch the tail, because the oversized gap is
 * paid even when conversions finish early).  The 8 us/sample partial
 * cover is the measured optimum: it keeps the first read out of the
 * conversion burst's COLLISION window (a read racing the burst can be
 * swallowed whole by coalesced CS edges, turning one miss into two:
 * 356 us avg vs 196 us) while the ladder absorbs the remainder.  The
 * real fix for ADC reply latency is slave-side (sampling-time config),
 * tracked for the next firmware rev.
 *
 * v0.15 big frames (BATCH / READ2) add ~45 ns per request+reply byte for
 * the longer frame handling in the CS-rising handler (protocol 0.15 §3.4,
 * an estimate; the ladder absorbs any remainder).  Legacy opcodes are
 * untouched so a v0.14-shaped exchange keeps its measured timing.  With
 * ATTN active none of this runs: the host waits for the edge instead. */
static uint32_t reply_staging_gap_us(uint8_t        cmd,
                                     const uint8_t *req_payload,
                                     size_t         req_payload_len,
                                     size_t         reply_len)
{
	uint32_t gap = GD32G553_REPLY_STAGING_GAP_US;

	if (cmd == GD32G553_CMD_ADC_READ && req_payload != NULL && req_payload_len >= 2u) {
		gap += 8u * (uint32_t)req_payload[1];
	}
	if (cmd == GD32G553_CMD_BATCH || cmd == GD32G553_CMD_ADC_STREAM_READ2) {
		gap += (uint32_t)((req_payload_len + reply_len) * 45u) / 1000u;
	}
	return gap;
}

/* Effective SPI payload ceiling.  A zeroed or hand-built context (tests,
 * pre-init) never negotiated anything, so it is the 65 B base envelope. */
static size_t link_max_payload(const gd32g553_t *ctx)
{
	return (ctx->max_payload > GD32G553_MAX_PAYLOAD_BYTES) ? ctx->max_payload
	                                                       : GD32G553_MAX_PAYLOAD_BYTES;
}

/* ----------------------------------------------------------------- */
/* ATTN bookkeeping (protocol 0.15 §4.5 / §4.6)                        */
/* ----------------------------------------------------------------- */

/* Consecutive lost edges / stuck-high readings / empty idle edges before
 * ATTN is withdrawn (§4.6). */
#define GD32G553_ATTN_FAULT_LIMIT      3u
#define GD32G553_ATTN_IDLE_EMPTY_LIMIT 8u

/* Stop awaiting edges at once (so the rest of this command, e.g. its
 * resync PING, does not pay another 10 ms) and ask the NEXT command to
 * renegotiate ATTN off.  Never renegotiate from inside the command that
 * tripped the limit: ctx->spi_reply still holds that command's reply, and
 * another frame would overwrite it before the caller has parsed it. */
static void attn_fault(gd32g553_t *ctx)
{
	ctx->attn_active        = false;
	ctx->attn_fault_pending = true;
}

/* Before each request: the stuck-high check.
 *
 * The line must be LOW here unless a watermark stream has events pending
 * (the GD32 drops it at every CS fall and only raises it after arming a
 * reply or on an event).  The spec's "high when CS releases on a >= 20 us
 * request" cannot be sampled through the portable SPI API (it does not
 * expose the CS release instant), so the host samples the idle level
 * instead; the 3-in-a-row rule absorbs a single unlucky reading. */
static void attn_pre_request(gd32g553_t *ctx)
{
	if (!ctx->attn_active) return;
	if (ctx->stream2_armed == 0u && !ctx->attn_event_edge) {
		bool high = false;
		if (ctx->attn.read_level(ctx->attn.user, &high) == ALP_OK && high) {
			ctx->attn_stuck++;
			if (++ctx->attn_stuck_run >= GD32G553_ATTN_FAULT_LIMIT) {
				attn_fault(ctx);
				return;
			}
		} else {
			ctx->attn_stuck_run = 0u;
		}
	}
}

/* Wait for the reply edge of the request that started at @p t_start (the
 * hook's clock, read just before the first byte was clocked).  Two filters:
 *
 *   - time: an edge stamped before t_start is stale (an event edge that was
 *     still latched).  Wrap-safe, but a wrap-safe compare cannot tell "2^31+
 *     ticks old" from "in the future", so the latch is drained right before
 *     t_start is read (see the caller): nothing older than the drain exists;
 *   - level: the firmware drives ATTN LOW at CS falling and never raises it
 *     while CS is low, so a stale RISE inside the window between t_start and our
 *     own CS falling is always followed by a LOW before this wait begins (the
 *     wait starts after the request write returned), whereas a genuine reply
 *     leaves ATTN HIGH.  An edge is accepted only if its stamp is not older than
 *     t_start AND the line reads HIGH now; otherwise keep waiting (bounded).
 *
 * Returns true when a genuine edge arrived.  A miss is not an error: the
 * caller falls back to the 0.14 staging gap + ladder for this command. */
static bool attn_await_reply(gd32g553_t *ctx, uint32_t t_start)
{
	if (!ctx->attn_active) return false;
	/* Each stale edge costs one more wait; a latch holds at most one. */
	for (unsigned tries = 0u; tries < 3u; ++tries) {
		uint32_t           t_edge = 0u;
		const alp_status_t w =
		    ctx->attn.wait(ctx->attn.user, GD32G553_BRIDGE_REPLY_TIMEOUT_MS, &t_edge);
		if (w != ALP_OK) break;
		bool high = false;
		if ((int32_t)(t_edge - t_start) >= 0 &&
		    ctx->attn.read_level(ctx->attn.user, &high) == ALP_OK && high) {
			ctx->attn_edges++;
			ctx->attn_timeout_run = 0u;
			return true;
		}
		ctx->attn_stale_edges++;
	}
	ctx->attn_timeouts++;
	if (++ctx->attn_timeout_run >= GD32G553_ATTN_FAULT_LIMIT) attn_fault(ctx);
	return false;
}

/* Forward declarations for the failed-command resync epilogue below. */
static alp_status_t spi_xfer(gd32g553_t    *ctx,
                             uint8_t        cmd,
                             const uint8_t *req_payload,
                             size_t         req_payload_len,
                             uint8_t       *reply_payload,
                             size_t         reply_payload_len);
static void         spi_negotiate(gd32g553_t *ctx);

/* Drop every negotiated item on the SPI link (a reset, OTA commit/rollback
 * or deinit returned the slave to the un-negotiated 0.14 framing). */
static void spi_drop_negotiated(gd32g553_t *ctx)
{
	ctx->seq_enabled        = false;
	ctx->seq_last           = 0u;
	ctx->granted            = 0u;
	ctx->supported          = 0u;
	ctx->max_payload        = GD32G553_MAX_PAYLOAD_BYTES;
	ctx->attn_active        = false;
	ctx->attn_fault_pending = false;
	ctx->attn_event_edge    = false;
	ctx->attn_last_edge     = false;
	ctx->attn_timeout_run   = 0u;
	ctx->attn_stuck_run     = 0u;
	ctx->attn_idle_run      = 0u;
	ctx->stream2_armed      = 0u;
	ctx->stream2_seen       = 0u;
}

/* STATUS_SEQ verdict for one CRC-valid SPI reply stamp, against the last
 * accepted stamp.
 *   FRESH -- the slave decoded our request (stamp advanced).
 *   STALE -- stamp did not advance: the slave re-served its old staged
 *            reply, i.e. it (almost certainly) never decoded the request.
 *   RESET -- the slave's link feature is off again: a GD32 reset (OTA
 *            commit/rollback, watchdog) reverts every reply to stamp 0.
 *            Stamp 0 after a non-zero baseline is that signature (a
 *            legitimate mod-16 wrap 0xF -> 0 is the one exception).  A
 *            stamp-0 "stale" (baseline already 0) is also RESET: after a
 *            reset it is indistinguishable from a stale re-serve, and the
 *            request may HAVE executed, so it must never be re-sent. */
enum spi_seq_verdict { SPI_SEQ_FRESH, SPI_SEQ_STALE, SPI_SEQ_RESET };

static enum spi_seq_verdict spi_seq_classify(const gd32g553_t *ctx, uint8_t stamp)
{
	if (stamp == 0u && ctx->seq_last != 0x0Fu) return SPI_SEQ_RESET;
	return (stamp == ctx->seq_last) ? SPI_SEQ_STALE : SPI_SEQ_FRESH;
}

/* Reset signature seen: drop EVERY negotiated item (sequence, BIG_FRAME,
 * ATTN, STREAM2, BATCH, stream handles), re-read the version (the reset
 * may have booted a different image) and re-run the init negotiation
 * (§8 step 5), then fail THIS call.  The request is deliberately NOT
 * re-sent -- whether the slave executed it before/while resetting is
 * unknowable, and re-sending a non-idempotent opcode would run it twice.
 * The ATTN hook stays registered and the pin stays an input; the reset
 * returned PA14 to SWCLK, so ATTN is requested afresh. */
static alp_status_t spi_seq_reset_recover(gd32g553_t *ctx)
{
	spi_drop_negotiated(ctx);
	ctx->attn_unusable = false;
	uint8_t v[3];
	if (spi_xfer(ctx, GD32G553_CMD_GET_VERSION, NULL, 0u, v, sizeof(v)) == ALP_OK) {
		ctx->version.major  = v[0];
		ctx->version.minor  = v[1];
		ctx->version.patch  = v[2];
		ctx->version_cached = true;
	}
	spi_negotiate(ctx);
	return ALP_ERR_IO;
}

/* Resync after a failed command.  The slave clears its staged reply
 * ONLY when it decodes a fresh request -- drains re-arm it (the
 * documented re-read semantics).  So when a command gives up (ladder
 * exhausted, or a short error envelope decoded), its stale staged
 * reply stays armed, and if the NEXT command's request is ever lost
 * to edge coalescing, that command reads the leftover -- at best a
 * CRC mismatch, at worst a perfectly valid error envelope attributed
 * to the WRONG command (observed 2026-06-04: GET_VERSION "returned"
 * a lingering TRNG BUSY).  One throwaway PING here forces a decode,
 * replacing the leftover with PING's reply and consuming it --
 * bounding staleness to the command that already failed.  Recursion
 * is depth-1 by construction: the epilogue never runs for PING. */
static void spi_resync_after_failure(gd32g553_t *ctx)
{
	(void)spi_xfer(ctx, 0x00u /* CMD_PING */, NULL, 0u, NULL, 0u);
}

/* One SPI command.  `reply_max` is the payload length the host clocks:
 * exact for a fixed-length reply (decode == NULL), the worst case for a
 * variable one (decode != NULL; the real length comes from decode()).  On
 * success a non-NULL `reply_payload` receives the payload and
 * `*reply_len_out` (when non-NULL) its length; either may be NULL, in
 * which case the caller reads the payload straight out of
 * ctx->spi_reply[2..] (valid until the next transaction). */
static alp_status_t spi_xfer_core(gd32g553_t    *ctx,
                                  uint8_t        cmd,
                                  const uint8_t *req_payload,
                                  size_t         req_payload_len,
                                  uint8_t       *reply_payload,
                                  size_t         reply_max,
                                  reply_len_fn   decode,
                                  const void    *decode_arg,
                                  size_t        *reply_len_out)
{
	if (ctx->spi == NULL) return ALP_ERR_NOSUPPORT;

	/* Only BATCH (request + reply) and READ2 (reply) may exceed the 65 B
	 * base envelope, and only up to what BIG_FRAME granted; clocking more
	 * than one 256 B frame in a CS window overflows the slave RX FIFO. */
	const bool   big_op = (cmd == GD32G553_CMD_BATCH || cmd == GD32G553_CMD_ADC_STREAM_READ2);
	const size_t req_cap =
	    (cmd == GD32G553_CMD_BATCH) ? link_max_payload(ctx) : (size_t)GD32G553_MAX_PAYLOAD_BYTES;
	const size_t rep_cap = big_op ? link_max_payload(ctx) : (size_t)GD32G553_MAX_PAYLOAD_BYTES;
	if (req_payload_len > req_cap || reply_max > rep_cap) return ALP_ERR_INVAL;

	/* Request envelope: SOF | CMD | PAYLOAD | CRC(SOF..PAYLOAD).  The
	 * buffer lives in the context so a 256 B frame never sits on the
	 * caller's thread stack. */
	uint8_t *req = ctx->spi_req;
	req[0]       = GD32G553_BRIDGE_SOF;
	req[1]       = cmd;
	/* gd32g553_batch() assembles its payload in place. */
	if (req_payload_len > 0u && req_payload != NULL && req_payload != &req[2]) {
		memcpy(&req[2], req_payload, req_payload_len);
	}
	const size_t   crc_covered = 2u + req_payload_len;
	const uint16_t crc         = alp_crc16_ccitt_false(req, crc_covered);
	req[crc_covered]           = (uint8_t)(crc & 0xFFu);
	req[crc_covered + 1u]      = (uint8_t)((crc >> 8) & 0xFFu);

	/* v0.7 STATUS_SEQ outer loop: one RE-SEND when a CRC-valid reply
	 * turns out to be STALE -- its [7:4] stamp never advanced past the
	 * previous accepted reply, meaning the slave never decoded our
	 * request and is re-serving the old staged reply (the residual
	 * hazard documented in the firmware's transport_spi.c; byte-exact
	 * replays silicon-fingerprinted 2026-06-06).  A stale verdict on a
	 * link the slave is still stamping means the request was not
	 * decoded, so the re-send is its first execution.  That inference
	 * does NOT hold across a slave reset (stamps revert to 0): those
	 * are caught by spi_seq_classify() as RESET and never re-sent.
	 * Without the negotiated feature the loop body runs exactly once. */
	for (unsigned send_attempt = 0u;; ++send_attempt) {
		attn_pre_request(ctx);
		/* Read the clock BEFORE the first byte goes out: any edge stamped
		 * earlier cannot be this request's reply. */
		uint32_t t_start = 0u;
		if (ctx->attn_active) {
			/* Drain first: every latched edge is older than the clock read
			 * below, so none can alias a "fresh" stamp after a counter wrap. */
			ctx->attn.drain(ctx->attn.user);
			t_start = ctx->attn.now(ctx->attn.user);
		}
		alp_status_t s = alp_spi_write(ctx->spi, req, crc_covered + 2u);
		if (s != ALP_OK) return s;

		/* v0.15 ATTN: with the line granted, the slave raises it only
		 * once the reply is armed, so the host waits for that edge
		 * (interrupt + semaphore in the backend, no polling) and reads
		 * at once.  Without it -- or on a lost edge, for this command
		 * only -- let the slave's CS-rising handler stage the reply
		 * before the first read (see the schedule comment above): cheaper
		 * than eating a wasted drain transaction + a ladder wait on
		 * every command. */
		const bool awaited  = ctx->attn_active;
		const bool got_edge = attn_await_reply(ctx, t_start);
		ctx->attn_last_edge = awaited && got_edge;
		if (!got_edge) {
			alp_delay_us(reply_staging_gap_us(cmd, req_payload, req_payload_len, reply_max));
		}

		/* Reply envelope: SOF | STATUS | PAYLOAD | CRC(SOF..PAYLOAD).
		 * Total bytes the host must clock = 1 + 1 + reply_max + 2.
		 * Read with the re-read schedule above: a too-early read is a
		 * harmless miss, not a failure.
		 *
		 * STATUS byte on SPI = [7:4] sequence stamp (zero until the
		 * v0.7 STATUS_SEQ feature is negotiated; old firmware never
		 * sets these bits) | [3:0] status code -- masking the code is
		 * unconditionally safe on this transport. */
		uint8_t     *reply     = ctx->spi_reply;
		const size_t reply_len = 2u + reply_max + 2u;

		bool   reply_ok  = false;
		bool   stale     = false;
		size_t plen_done = reply_max;
		for (unsigned attempt = 0u; attempt < GD32G553_REPLY_READ_TRIES; ++attempt) {
			s = alp_spi_read(ctx->spi, reply, reply_len);
			if (s != ALP_OK) return s; /* transport-level error: fail loud */

			if (reply[0] == GD32G553_BRIDGE_SOF) {
				const bool status_ok = (reply[1] & GD32G553_STATUS_CODE_MASK) == 0x00u;
				size_t     plen      = reply_max;
				if (decode != NULL) {
					/* Variable-length reply: the CRC position comes from the
					 * reply's own length field, so only a STATUS_OK frame has
					 * a payload to measure; an error is the 4-byte envelope. */
					plen = status_ok ? decode(&reply[2], reply_max, decode_arg) : REPLY_LEN_INVALID;
				}
				if (plen != REPLY_LEN_INVALID) {
					const uint16_t expect_crc = alp_crc16_ccitt_false(reply, 2u + plen);
					const uint16_t got_crc =
					    (uint16_t)reply[2u + plen] | (uint16_t)reply[2u + plen + 1u] << 8;
					if (got_crc == expect_crc) {
						reply_ok  = true;
						plen_done = plen;
						break;
					}
				}

				/* ERROR-ENVELOPE FALLBACK.  Firmware error replies carry NO
				 * payload (stage_error_reply: SOF | STATUS | CRC = 4 bytes)
				 * regardless of the opcode's success-reply width, so for any
				 * payload-bearing opcode the full-width CRC check above
				 * fails on a legitimate error reply -- the bytes past the
				 * 4-byte envelope are TX-FIFO idle filler.  Without this
				 * fallback every firmware error surfaced as ALP_ERR_IO,
				 * masking the real status (silicon 2026-06-04: an entire
				 * class of "-5 from cycle 1" HiL rows -- pwm_capture's
				 * documented NOSUPPORT among them -- were short error
				 * replies the host could not decode). */
				if (reply_max > 0u && !status_ok) {
					const uint16_t err_crc = alp_crc16_ccitt_false(reply, 2u);
					const uint16_t err_got = (uint16_t)reply[2] | ((uint16_t)reply[3] << 8);
					if (err_got == err_crc) {
						if (ctx->seq_enabled) {
							const uint8_t              stamp = (uint8_t)(reply[1] >> 4);
							const enum spi_seq_verdict v     = spi_seq_classify(ctx, stamp);
							if (v == SPI_SEQ_RESET) return spi_seq_reset_recover(ctx);
							if (v == SPI_SEQ_STALE) {
								stale = true; /* stale ERROR reply: re-send */
								break;
							}
							ctx->seq_last = stamp;
						}
						/* Read the status BEFORE the resync PING: the shared RX
						 * buffer is reused by that PING's own reply. */
						const alp_status_t fw_err =
						    status_from_wire(reply[1] & GD32G553_STATUS_CODE_MASK);
						if (cmd != 0x00u /* PING */) spi_resync_after_failure(ctx);
						return fw_err;
					}
				}
			}
			if (attempt + 1u < GD32G553_REPLY_READ_TRIES) {
				alp_delay_us(gd32g553_reply_retry_us[attempt]);
			}
		}
		if (!reply_ok && !stale) {
			if (cmd != 0x00u /* PING */) spi_resync_after_failure(ctx);
			return ALP_ERR_IO;
		}

		if (reply_ok) {
			const uint8_t code  = (uint8_t)(reply[1] & GD32G553_STATUS_CODE_MASK);
			const uint8_t stamp = (uint8_t)(reply[1] >> 4);

			if (cmd == GD32G553_CMD_LINK_FEATURES && code == 0x00u && reply_max >= 1u) {
				/* Negotiation reply: arm/disarm host-side sequencing and
				 * take THIS reply's stamp as the baseline (the firmware
				 * stamps the negotiation reply itself once it grants).  Both
				 * the 1-byte and the 10-byte form carry the STATUS_SEQ bit
				 * in payload byte 0. */
				ctx->seq_enabled = (reply[2] & GD32G553_LINK_FEAT_STATUS_SEQ) != 0u;
				ctx->seq_last    = stamp;
			} else if (ctx->seq_enabled) {
				const enum spi_seq_verdict v = spi_seq_classify(ctx, stamp);
				if (v == SPI_SEQ_RESET) return spi_seq_reset_recover(ctx);
				if (v == SPI_SEQ_STALE) {
					stale = true; /* slave never decoded the request */
				} else {
					ctx->seq_last = stamp;
				}
			}

			if (!stale) {
				const alp_status_t firmware_status = status_from_wire(code);
				if (firmware_status != ALP_OK) return firmware_status;

				if (plen_done > 0u && reply_payload != NULL) {
					memcpy(reply_payload, &reply[2], plen_done);
				}
				if (reply_len_out != NULL) *reply_len_out = plen_done;
				return ALP_OK;
			}
		}

		/* Stale reply detected.  One re-send: the hazard is a
		 * single-transaction race, so the second attempt is expected
		 * to land; a SECOND stale in a row means the link is wedged
		 * beyond this mechanism -- resync + fail loud. */
		ctx->seq_stale_count++;
		if (send_attempt >= 1u) {
			if (cmd != 0x00u /* PING */) spi_resync_after_failure(ctx);
			return ALP_ERR_IO;
		}
	}
}

/* Fixed-length reply: the common case. */
static alp_status_t spi_xfer(gd32g553_t    *ctx,
                             uint8_t        cmd,
                             const uint8_t *req_payload,
                             size_t         req_payload_len,
                             uint8_t       *reply_payload,
                             size_t         reply_payload_len)
{
	return spi_xfer_core(
	    ctx, cmd, req_payload, req_payload_len, reply_payload, reply_payload_len, NULL, NULL, NULL);
}

/* ----------------------------------------------------------------- */
/* v0.15 link-feature negotiation (protocol 0.15 §2, §8)               */
/* ----------------------------------------------------------------- */

/* Send the 6-byte CMD_LINK_FEATURES form and record the 10-byte reply. */
static alp_status_t link_features_ext(gd32g553_t *ctx, uint32_t want, uint16_t mp_req)
{
	uint8_t req[6];
	put_le32(req, want);
	req[4] = (uint8_t)(mp_req & 0xFFu);
	req[5] = (uint8_t)(mp_req >> 8);

	uint8_t      reply[10];
	alp_status_t s =
	    spi_xfer(ctx, GD32G553_CMD_LINK_FEATURES, req, sizeof(req), reply, sizeof(reply));
	if (s != ALP_OK) return s;

	/* Never believe a grant wider than the request, and keep the stored
	 * view self-consistent: BIG_FRAME needs a ceiling above 65, ATTN
	 * needs STATUS_SEQ.  An inconsistent reply degrades to the safe
	 * (smaller) interpretation rather than failing the link. */
	uint32_t granted = get_le32(&reply[0]) & want;
	uint16_t mp      = (uint16_t)((uint16_t)reply[8] | ((uint16_t)reply[9] << 8));
	if ((granted & GD32G553_LINK_FEAT_BIG_FRAME) != 0u &&
	    (mp <= GD32G553_MAX_PAYLOAD_BYTES || mp > GD32G553_BIG_MAX_PAYLOAD_BYTES)) {
		granted &= ~GD32G553_LINK_FEAT_BIG_FRAME;
	}
	if ((granted & GD32G553_LINK_FEAT_BIG_FRAME) == 0u) mp = GD32G553_MAX_PAYLOAD_BYTES;
	if ((granted & GD32G553_LINK_FEAT_STATUS_SEQ) == 0u) granted &= ~GD32G553_LINK_FEAT_ATTN;

	ctx->granted     = granted;
	ctx->supported   = get_le32(&reply[4]);
	ctx->max_payload = mp;
	return ALP_OK;
}

/* §8 step 3 for one SPI link.  Best-effort: any failure leaves the link
 * on the legacy framing (the features are an integrity/throughput
 * upgrade, not a liveness requirement). */
static void spi_negotiate(gd32g553_t *ctx)
{
	ctx->granted     = 0u;
	ctx->supported   = 0u;
	ctx->max_payload = GD32G553_MAX_PAYLOAD_BYTES;
	ctx->attn_active = false;
	if (ctx->spi == NULL) return;

	if (ctx->version.major == GD32G553_HOST_PROTOCOL_MAJOR &&
	    ctx->version.minor >= GD32G553_V015_MIN_PROTOCOL_MINOR) {
		uint32_t   want      = GD32G553_LINK_FEAT_STATUS_SEQ | GD32G553_LINK_FEAT_BIG_FRAME |
		                       GD32G553_LINK_FEAT_ADC_STREAM2 | GD32G553_LINK_FEAT_BATCH;
		const bool want_attn = ctx->attn.now != NULL && ctx->attn.wait != NULL &&
		                       ctx->attn.read_level != NULL && ctx->attn.drain != NULL &&
		                       !ctx->attn_unusable;
		if (want_attn) {
			want |= GD32G553_LINK_FEAT_ATTN;
			/* Provisional: the reply that grants ATTN is the first
			 * ATTN-signalled one, so await it on an edge (self-test). */
			ctx->attn_active = true;
		}
		const alp_status_t s    = link_features_ext(ctx, want, GD32G553_BIG_MAX_PAYLOAD_BYTES);
		ctx->attn_active        = false;
		ctx->attn_timeout_run   = 0u;
		ctx->attn_fault_pending = false;
		if (s == ALP_OK) {
			if ((ctx->granted & GD32G553_LINK_FEAT_ATTN) != 0u) {
				if (ctx->attn_last_edge) {
					ctx->attn_active = true;
				} else {
					/* Self-test failed: no edge on the grant reply.  Withdraw
					 * ATTN at once (the slave returns PA14 to SWCLK) and do
					 * not ask again until the next init. */
					ctx->attn_unusable = true;
					(void)link_features_ext(
					    ctx, ctx->granted & ~GD32G553_LINK_FEAT_ATTN, ctx->max_payload);
				}
			}
			return;
		}
		/* INVAL / NOSUPPORT (older firmware, or a build refusing the form)
		 * or a transport error: fall back to the legacy form. */
	}

	uint8_t want    = GD32G553_LINK_FEAT_STATUS_SEQ;
	uint8_t granted = 0u;
	if (spi_xfer(ctx, GD32G553_CMD_LINK_FEATURES, &want, 1u, &granted, 1u) == ALP_OK) {
		ctx->granted = granted & GD32G553_LINK_FEAT_STATUS_SEQ;
	}
}

/* Work deferred from the end of the previous command, run at the START of
 * the next one -- never between receiving a reply and parsing it, because
 * every frame here reuses ctx->spi_req / ctx->spi_reply:
 *
 *   - the bridge reset under us (OTA commit/rollback): re-read the version
 *     and re-run the link negotiation (§8), so BEGIN2/BATCH handles and
 *     ATTN are not left failing with NOSUPPORT.  While the bridge is still
 *     rebooting (trial window answers BUSY) the flag stays set and the next
 *     command retries;
 *   - the ATTN fault limit was hit (3 lost / stuck edges): renegotiate with
 *     the ATTN bit cleared, which also returns PA14 to SWCLK on the slave. */
static void spi_prepare(gd32g553_t *ctx)
{
	if (ctx->spi == NULL) return;
	if (ctx->renegotiate_pending) {
		uint8_t v[3];
		if (spi_xfer(ctx, GD32G553_CMD_GET_VERSION, NULL, 0u, v, sizeof(v)) == ALP_OK) {
			ctx->renegotiate_pending = false;
			ctx->version.major       = v[0];
			ctx->version.minor       = v[1];
			ctx->version.patch       = v[2];
			ctx->version_cached      = true;
			ctx->attn_unusable       = false;
			spi_negotiate(ctx);
		}
		return;
	}
	if (!ctx->attn_fault_pending) return;
	ctx->attn_fault_pending = false;
	ctx->attn_active        = false;
	ctx->attn_unusable      = true;
	(void)link_features_ext(ctx, ctx->granted & ~GD32G553_LINK_FEAT_ATTN, ctx->max_payload);
}

/* ----------------------------------------------------------------- */
/* Transport: I2C                                                     */
/* ----------------------------------------------------------------- */

static alp_status_t i2c_xfer(gd32g553_t    *ctx,
                             uint8_t        cmd,
                             const uint8_t *req_payload,
                             size_t         req_payload_len,
                             uint8_t       *reply_payload,
                             size_t         reply_payload_len)
{
	if (ctx->i2c == NULL) return ALP_ERR_NOSUPPORT;

	/* Write side: [reg=0x00][CMD][PAYLOAD][CRC(CMD..PAYLOAD) lo, hi] */
	uint8_t wbuf[GD32G553_MAX_I2C_WRITE_BYTES];
	if (1u + 1u + req_payload_len + 2u > sizeof(wbuf)) return ALP_ERR_INVAL;

	wbuf[0] = GD32G553_BRIDGE_I2C_REG_CMD; /* virtual command register */
	wbuf[1] = cmd;
	if (req_payload_len > 0u && req_payload != NULL) {
		memcpy(&wbuf[2], req_payload, req_payload_len);
	}
	/* CRC covers CMD | PAYLOAD (NOT the reg byte, NOT the I2C address). */
	const size_t   crc_covered      = 1u + req_payload_len;
	const uint16_t crc              = alp_crc16_ccitt_false(&wbuf[1], crc_covered);
	wbuf[2u + req_payload_len]      = (uint8_t)(crc & 0xFFu);
	wbuf[2u + req_payload_len + 1u] = (uint8_t)((crc >> 8) & 0xFFu);

	const size_t wlen = 2u + req_payload_len + 2u;

	/* Read side: [STATUS][PAYLOAD][CRC(STATUS..PAYLOAD) lo, hi] */
	uint8_t      rbuf[GD32G553_MAX_I2C_READ_BYTES];
	const size_t rlen = 1u + reply_payload_len + 2u;
	if (rlen > sizeof(rbuf)) return ALP_ERR_INVAL;

	/* Combined write-then-(repeated-start)-read.  The firmware can
     * clock-stretch between phases to give itself time to process
     * the request; the bridge doc §5 documents this behaviour. */
	alp_status_t s = alp_i2c_write_read(ctx->i2c, ctx->i2c_addr, wbuf, wlen, rbuf, rlen);
	if (s != ALP_OK) return s;

	const uint16_t expect_crc = alp_crc16_ccitt_false(rbuf, 1u + reply_payload_len);
	const uint16_t got_crc =
	    (uint16_t)rbuf[1u + reply_payload_len] | (uint16_t)rbuf[1u + reply_payload_len + 1u] << 8;
	if (got_crc != expect_crc) {
		/* Error-envelope fallback -- same trap as the SPI side: firmware
         * error replies carry no payload ([STATUS][CRC] = 3 bytes on
         * I2C), so the full-width CRC fails on a legitimate error.
         * Decode the short shape before declaring transport failure. */
		if (reply_payload_len > 0u && rbuf[0] != 0x00u /* STATUS_OK */) {
			const uint16_t err_crc = alp_crc16_ccitt_false(rbuf, 1u);
			const uint16_t err_got = (uint16_t)rbuf[1] | ((uint16_t)rbuf[2] << 8);
			if (err_got == err_crc) {
				return status_from_wire(rbuf[0]);
			}
		}
		return ALP_ERR_IO;
	}

	const alp_status_t firmware_status = status_from_wire(rbuf[0]);
	if (firmware_status != ALP_OK) return firmware_status;

	if (reply_payload_len > 0u && reply_payload != NULL) {
		memcpy(reply_payload, &rbuf[1], reply_payload_len);
	}
	return ALP_OK;
}

/* ----------------------------------------------------------------- */
/* Transport router                                                   */
/* ----------------------------------------------------------------- */

static alp_status_t cmd_send(gd32g553_t          *ctx,
                             gd32g553_transport_t t,
                             uint8_t              cmd,
                             const uint8_t       *req_payload,
                             size_t               req_payload_len,
                             uint8_t             *reply_payload,
                             size_t               reply_payload_len)
{
	if (t == GD32G553_TRANSPORT_DEFAULT) t = ctx->default_transport;
	alp_status_t s;
	/* An accepted Deep-sleep: wake the bridge before this command's frame,
	 * which would otherwise be the one a CS wake loses. */
	if (ctx->power_asleep && (s = gd32g553_power_wake(ctx)) != ALP_OK) return s;
	switch (t) {
	case GD32G553_TRANSPORT_SPI:
		/* Deferred ATTN fault / post-reset renegotiation.  GET_VERSION is the
		 * exception: gd32g553_refresh_version() finishes a pending
		 * renegotiation itself, from its own reply, so a version read after an
		 * OTA reset costs one GET_VERSION on the wire, not two. */
		if (cmd != GD32G553_CMD_GET_VERSION) spi_prepare(ctx);
		s = spi_xfer(ctx, cmd, req_payload, req_payload_len, reply_payload, reply_payload_len);
		break;
	case GD32G553_TRANSPORT_I2C:
		s = i2c_xfer(ctx, cmd, req_payload, req_payload_len, reply_payload, reply_payload_len);
		break;
	default:
		return ALP_ERR_INVAL;
	}
	/* A transport-level failure means the link (or the bridge behind it)
     * is in an unknown state and the caller will likely re-init: do not
     * keep vouching for a version read before it. */
	if (s == ALP_ERR_IO || s == ALP_ERR_TIMEOUT) ctx->version_cached = false;
	return s;
}

/* ----------------------------------------------------------------- */
/* Public API                                                          */
/* ----------------------------------------------------------------- */

/* OTA trial-boot window: after OTA_COMMIT/OTA_ROLLBACK a trial-capable
 * bridge (>= 0.2.14, see gd32g553_ota_begin()'s @p fw_version doc)
 * reboots into an unconfirmed TRIAL image and answers STATUS_BUSY to
 * EVERY opcode -- including PING/GET_VERSION -- until it decodes its
 * first CRC-valid frame, which itself confirms the trial; the bridge
 * then resets a SECOND time to boot the now-confirmed image, so the
 * link also drops for a few ms right after that first BUSY reply
 * lands.  That second drop surfaces to the host as a transient
 * transport error (STATUS_IO / STATUS_TIMEOUT, or a CRC miss the
 * driver already maps to ALP_ERR_IO), not BUSY.
 *
 * Retry rule (#2314 review): ALP_ERR_BUSY is unconditionally
 * transient -- only a live, trial-confirming bridge answers it at
 * all.  ALP_ERR_IO / ALP_ERR_TIMEOUT are transient ONLY once this
 * same init() call has already seen at least one ALP_ERR_BUSY --
 * i.e. only once we KNOW a trial is in progress and the second reset
 * is the explanation.  An absent or unflashed bridge answers neither
 * opcode at all and fails with IO/TIMEOUT WITHOUT ever having shown a
 * BUSY, so it still fails fast, exactly as before this change --
 * load-bearing for callers like src/zephyr/v2n_supervisor.c, which
 * re-runs this init on every acquire() while the bridge stays absent
 * and budgets that on a ~1 ms bridge op, not a 2 s retry ladder.
 *
 * Residual: a re-init that happens to start WHILE the bridge is
 * already mid-COMMIT-reset (i.e. this call's very first PING lands
 * inside that first reset, before any BUSY was ever observed) still
 * fails fast with IO/TIMEOUT -- the caller's own retry (already
 * required today for any transient init failure) picks it up on the
 * next attempt, same as before this change.
 *
 * Budget: ~2 s, ONE shared deadline read via alp_uptime_ms() (#1953)
 * at entry and reused across BOTH the PING and GET_VERSION phases --
 * never re-read per phase -- so the wall-clock retry window is
 * actually bounded to the budget even though it charges real elapsed
 * time (bus calls, not just the backoff sleeps) against it.  The
 * bootloader's FWDGT window is ~32.8 s nominal (far longer than this
 * budget), so 2 s is sized to the confirm+reboot path, not the
 * watchdog-revert path -- a caller riding out an actual revert simply
 * sees this budget expire and gets back the transient status, same as
 * any other failed init. */
#define GD32G553_INIT_RETRY_BUDGET_MS 2000u
static const uint16_t gd32g553_init_retry_ms[] = { 10u, 20u, 40u, 80u, 160u, 160u, 160u, 160u };
#define GD32G553_INIT_RETRY_RUNGS \
	(unsigned)(sizeof(gd32g553_init_retry_ms) / sizeof(gd32g553_init_retry_ms[0]))

static bool gd32g553_init_status_is_transient(alp_status_t s, bool saw_busy_this_init)
{
	if (s == ALP_ERR_BUSY) return true;
	return saw_busy_this_init && (s == ALP_ERR_IO || s == ALP_ERR_TIMEOUT);
}

/* Sleep the next rung of the ladder, clamped to what's left of the
 * shared deadline.  Returns false once the deadline has passed
 * (caller gives up with the last status it saw). */
static bool gd32g553_init_retry_wait(unsigned *attempt, uint64_t deadline_ms)
{
	const uint64_t now_ms = alp_uptime_ms();
	if (now_ms >= deadline_ms) return false;
	const unsigned rung =
	    (*attempt < GD32G553_INIT_RETRY_RUNGS) ? *attempt : GD32G553_INIT_RETRY_RUNGS - 1u;
	uint16_t       wait_ms      = gd32g553_init_retry_ms[rung];
	const uint64_t remaining_ms = deadline_ms - now_ms;
	if ((uint64_t)wait_ms > remaining_ms) wait_ms = (uint16_t)remaining_ms;
	alp_delay_ms(wait_ms);
	(*attempt)++;
	return true;
}

alp_status_t gd32g553_init(gd32g553_t *ctx, alp_spi_t *spi, alp_i2c_t *i2c, uint8_t i2c_addr_7bit)
{
	return gd32g553_init_ex(ctx, spi, i2c, i2c_addr_7bit, NULL);
}

alp_status_t gd32g553_init_ex(gd32g553_t                 *ctx,
                              alp_spi_t                  *spi,
                              alp_i2c_t                  *i2c,
                              uint8_t                     i2c_addr_7bit,
                              const gd32g553_attn_hook_t *attn)
{
	if (ctx == NULL) return ALP_ERR_INVAL;
	if (spi == NULL && i2c == NULL) return ALP_ERR_INVAL;
	if (i2c != NULL && i2c_addr_7bit > 0x7Fu) return ALP_ERR_INVAL;

	memset(ctx, 0, sizeof(*ctx));
	ctx->spi               = spi;
	ctx->i2c               = i2c;
	ctx->i2c_addr          = i2c_addr_7bit;
	ctx->default_transport = (spi != NULL) ? GD32G553_TRANSPORT_SPI : GD32G553_TRANSPORT_I2C;
	ctx->max_payload       = GD32G553_MAX_PAYLOAD_BYTES;
	if (attn != NULL) ctx->attn = *attn;
	ctx->initialised = true;

	const uint64_t deadline_ms = alp_uptime_ms() + GD32G553_INIT_RETRY_BUDGET_MS;
	bool           saw_busy    = false;
	unsigned       attempt     = 0u;

	alp_status_t s;
	for (;;) {
		s = gd32g553_ping(ctx);
		if (s == ALP_ERR_BUSY) saw_busy = true;
		if (!gd32g553_init_status_is_transient(s, saw_busy)) break;
		if (!gd32g553_init_retry_wait(&attempt, deadline_ms)) break;
	}
	if (s != ALP_OK) {
		ctx->initialised = false;
		return s;
	}

	gd32g553_version_t v;
	for (;;) {
		s = gd32g553_refresh_version(ctx, &v);
		if (s == ALP_ERR_BUSY) saw_busy = true;
		if (!gd32g553_init_status_is_transient(s, saw_busy)) break;
		if (!gd32g553_init_retry_wait(&attempt, deadline_ms)) break;
	}
	if (s != ALP_OK) {
		ctx->initialised = false;
		return s;
	}
	if (v.major != GD32G553_HOST_PROTOCOL_MAJOR) {
		ctx->initialised = false;
		return ALP_ERR_NOSUPPORT;
	}
	/* gd32g553_refresh_version() filled ctx->version and armed the cache. */

	/* Link-feature negotiation (best-effort, SPI only -- STATUS_SEQ, BIG_FRAME,
	 * ATTN, STREAM2 and BATCH are SPI-framing features; I2C needs nothing,
	 * it carries only the allow-listed management opcodes at 65 B).  A
	 * v0.15+ peer gets the 6-byte form (see spi_negotiate()); an older
	 * peer, or one that refuses it, the legacy 1-byte STATUS_SEQ form,
	 * so the wire stays byte-identical to v0.14.  spi_xfer arms
	 * ctx->seq_enabled from the negotiation reply itself, which carries
	 * the stamp baseline.  Any error here is deliberately non-fatal: the
	 * features are an integrity/throughput upgrade, not a liveness
	 * requirement. */
	spi_negotiate(ctx);
	return ALP_OK;
}

alp_status_t gd32g553_set_default_transport(gd32g553_t *ctx, gd32g553_transport_t t)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (t == GD32G553_TRANSPORT_SPI && ctx->spi == NULL) return ALP_ERR_NOSUPPORT;
	if (t == GD32G553_TRANSPORT_I2C && ctx->i2c == NULL) return ALP_ERR_NOSUPPORT;
	if (t != GD32G553_TRANSPORT_SPI && t != GD32G553_TRANSPORT_I2C) return ALP_ERR_INVAL;
	ctx->default_transport = t;
	return ALP_OK;
}

alp_status_t gd32g553_ping(gd32g553_t *ctx)
{
	if (ctx == NULL) return ALP_ERR_INVAL;
	/* Allow ping during init even before initialised==true: this is the
     * link-liveness probe gd32g553_init() runs.  The transport pointers
     * have already been set at that point. */
	return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_PING, NULL, 0u, NULL, 0u);
}

alp_status_t gd32g553_ping_via(gd32g553_t *ctx, gd32g553_transport_t t)
{
	if (ctx == NULL) return ALP_ERR_INVAL;
	return cmd_send(ctx, t, GD32G553_CMD_PING, NULL, 0u, NULL, 0u);
}

alp_status_t gd32g553_refresh_version(gd32g553_t *ctx, gd32g553_version_t *out)
{
	if (ctx == NULL || out == NULL) return ALP_ERR_INVAL;
	uint8_t      reply[3];
	alp_status_t s = cmd_send(
	    ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_GET_VERSION, NULL, 0u, reply, sizeof(reply));
	if (s != ALP_OK) return s;
	out->major          = reply[0];
	out->minor          = reply[1];
	out->patch          = reply[2];
	ctx->version        = *out;
	ctx->version_cached = true;
	/* The reply is already copied out, so re-running the negotiation here (the
	 * bridge reset under us, see spi_prepare()) cannot clobber anything. */
	if (ctx->renegotiate_pending && ctx->spi != NULL) {
		ctx->renegotiate_pending = false;
		ctx->attn_unusable       = false;
		spi_negotiate(ctx);
	}
	return ALP_OK;
}

alp_status_t gd32g553_get_version(gd32g553_t *ctx, gd32g553_version_t *out)
{
	if (ctx == NULL || out == NULL) return ALP_ERR_INVAL;
	if (ctx->initialised && ctx->version_cached) {
		*out = ctx->version;
		return ALP_OK;
	}
	return gd32g553_refresh_version(ctx, out);
}

alp_status_t gd32g553_get_build_id(gd32g553_t *ctx, char build_id[GD32G553_BUILD_ID_LEN + 1])
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (build_id == NULL) return ALP_ERR_INVAL;
	uint8_t      reply[GD32G553_BUILD_ID_LEN];
	alp_status_t s = cmd_send(
	    ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_GET_BUILD_ID, NULL, 0u, reply, sizeof(reply));
	if (s != ALP_OK) return s;
	memcpy(build_id, reply, GD32G553_BUILD_ID_LEN);
	build_id[GD32G553_BUILD_ID_LEN] = '\0';
	return ALP_OK;
}

alp_status_t gd32g553_get_reset_reason(gd32g553_t *ctx, gd32g553_reset_cause_t *out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (out == NULL) return ALP_ERR_INVAL;
	uint8_t      reply;
	alp_status_t s =
	    cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_RESET_REASON, NULL, 0u, &reply, 1u);
	if (s != ALP_OK) return s;
	*out = (gd32g553_reset_cause_t)reply;
	return ALP_OK;
}

alp_status_t gd32g553_gpio_read(gd32g553_t *ctx, uint32_t mask, uint32_t *levels)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (levels == NULL) return ALP_ERR_INVAL;
	uint8_t req[4];
	put_le32(req, mask);
	uint8_t      reply[4];
	alp_status_t s = cmd_send(ctx,
	                          GD32G553_TRANSPORT_DEFAULT,
	                          GD32G553_CMD_GPIO_READ,
	                          req,
	                          sizeof(req),
	                          reply,
	                          sizeof(reply));
	if (s != ALP_OK) return s;
	*levels = get_le32(reply);
	return ALP_OK;
}

alp_status_t gd32g553_gpio_write(gd32g553_t *ctx, uint32_t mask, uint32_t levels)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	uint8_t req[8];
	put_le32(&req[0], mask);
	put_le32(&req[4], levels);
	return cmd_send(
	    ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_GPIO_WRITE, req, sizeof(req), NULL, 0u);
}

alp_status_t
gd32g553_pwm_set(gd32g553_t *ctx, uint8_t channel, uint32_t period_ns, uint32_t duty_ns)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (duty_ns > period_ns) return ALP_ERR_INVAL;
	uint8_t req[10];
	req[0] = channel;
	req[1] = 0u; /* reserved */
	put_le32(&req[2], period_ns);
	put_le32(&req[6], duty_ns);
	return cmd_send(
	    ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_PWM_SET, req, sizeof(req), NULL, 0u);
}

alp_status_t
gd32g553_pwm_get(gd32g553_t *ctx, uint8_t channel, uint32_t *period_ns, uint32_t *duty_ns)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (period_ns == NULL || duty_ns == NULL) return ALP_ERR_INVAL;
	uint8_t      req = channel;
	uint8_t      reply[8];
	alp_status_t s = cmd_send(
	    ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_PWM_GET, &req, 1u, reply, sizeof(reply));
	if (s != ALP_OK) return s;
	*period_ns = get_le32(&reply[0]);
	*duty_ns   = get_le32(&reply[4]);
	return ALP_OK;
}

alp_status_t gd32g553_adc_read(gd32g553_t *ctx, uint8_t channel, uint8_t samples, uint16_t *mv)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (mv == NULL) return ALP_ERR_INVAL;
	if (samples == 0u || samples > GD32G553_BRIDGE_ADC_MAX_SAMPLES) {
		return ALP_ERR_INVAL;
	}

	uint8_t req[2] = { channel, samples };
	/* Reply payload echoes `samples` then carries 2 bytes per reading.
     * cmd_send needs the reply length known up-front; the host passes
     * the same `samples` it requested. */
	const size_t reply_payload_len = 1u + (size_t)samples * 2u;
	uint8_t      reply[1u + (GD32G553_BRIDGE_ADC_MAX_SAMPLES * 2u)];

	alp_status_t s = cmd_send(ctx,
	                          GD32G553_TRANSPORT_DEFAULT,
	                          GD32G553_CMD_ADC_READ,
	                          req,
	                          sizeof(req),
	                          reply,
	                          reply_payload_len);
	if (s != ALP_OK) return s;

	/* The firmware MUST echo the same samples value back at byte 0.
     * If it doesn't, the host treats the frame as corrupt -- the
     * CRC has already passed so this guards against a firmware bug,
     * not a wire glitch. */
	if (reply[0] != samples) return ALP_ERR_IO;

	for (uint8_t i = 0u; i < samples; ++i) {
		mv[i] = (uint16_t)reply[1u + i * 2u] | (uint16_t)reply[1u + i * 2u + 1u] << 8;
	}
	return ALP_OK;
}

alp_status_t gd32g553_da9292_status_forward(gd32g553_t *ctx, uint8_t *status)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (status == NULL) return ALP_ERR_INVAL;
	return cmd_send(
	    ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_DA9292_STATUS_FORWARD, NULL, 0u, status, 1u);
}

alp_status_t gd32g553_se_reset(gd32g553_t *ctx, bool assert)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	const uint8_t req = assert ? 1u : 0u;
	return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_SE_RESET, &req, 1u, NULL, 0u);
}

alp_status_t gd32g553_dac_set(gd32g553_t *ctx, uint8_t channel, uint16_t value_mv)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (channel >= GD32G553_BRIDGE_DAC_CHANNELS) return ALP_ERR_INVAL;
	uint8_t req[4];
	req[0] = channel;
	req[1] = 0u; /* reserved */
	req[2] = (uint8_t)(value_mv & 0xFFu);
	req[3] = (uint8_t)((value_mv >> 8) & 0xFFu);
	return cmd_send(
	    ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_DAC_SET, req, sizeof(req), NULL, 0u);
}

alp_status_t gd32g553_dac_get(gd32g553_t *ctx, uint8_t channel, uint16_t *value_mv)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (value_mv == NULL) return ALP_ERR_INVAL;
	if (channel >= GD32G553_BRIDGE_DAC_CHANNELS) return ALP_ERR_INVAL;
	uint8_t      reply[2];
	alp_status_t s = cmd_send(
	    ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_DAC_GET, &channel, 1u, reply, sizeof(reply));
	if (s != ALP_OK) return s;
	*value_mv = (uint16_t)reply[0] | ((uint16_t)reply[1] << 8);
	return ALP_OK;
}

alp_status_t gd32g553_qenc_read(gd32g553_t *ctx, uint8_t encoder, int32_t *position_out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (position_out == NULL) return ALP_ERR_INVAL;
	if (encoder >= GD32G553_BRIDGE_QENC_CHANNELS) return ALP_ERR_INVAL;
	uint8_t      reply[4];
	alp_status_t s = cmd_send(ctx,
	                          GD32G553_TRANSPORT_DEFAULT,
	                          GD32G553_CMD_QENC_READ,
	                          &encoder,
	                          1u,
	                          reply,
	                          sizeof(reply));
	if (s != ALP_OK) return s;
	*position_out = (int32_t)get_le32(reply);
	return ALP_OK;
}

alp_status_t gd32g553_qenc_reset(gd32g553_t *ctx, uint8_t encoder)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (encoder >= GD32G553_BRIDGE_QENC_CHANNELS) return ALP_ERR_INVAL;
	return cmd_send(
	    ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_QENC_RESET, &encoder, 1u, NULL, 0u);
}

alp_status_t gd32g553_counter_read(gd32g553_t *ctx, uint8_t counter, uint32_t *ticks_out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (ticks_out == NULL) return ALP_ERR_INVAL;
	if (counter >= GD32G553_BRIDGE_COUNTER_CHANNELS) return ALP_ERR_INVAL;
	uint8_t      reply[4];
	alp_status_t s = cmd_send(ctx,
	                          GD32G553_TRANSPORT_DEFAULT,
	                          GD32G553_CMD_COUNTER_READ,
	                          &counter,
	                          1u,
	                          reply,
	                          sizeof(reply));
	if (s != ALP_OK) return s;
	*ticks_out = get_le32(reply);
	return ALP_OK;
}

/* ------------------------------------------------------------------ */
/* v0.3 -- GD32G5 HW knobs                                            */
/* ------------------------------------------------------------------ */

alp_status_t gd32g553_pwm_configure(gd32g553_t          *ctx,
                                    uint8_t              channel,
                                    gd32g553_pwm_align_t align_mode,
                                    uint32_t             dead_time_ns,
                                    uint8_t              break_cfg)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if ((unsigned)align_mode > 3u) return ALP_ERR_INVAL;
	uint8_t req[7];
	req[0] = channel;
	req[1] = (uint8_t)align_mode;
	put_le32(&req[2], dead_time_ns);
	req[6] = break_cfg;
	return cmd_send(
	    ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_PWM_CONFIGURE, req, sizeof(req), NULL, 0u);
}

alp_status_t gd32g553_adc_configure(gd32g553_t *ctx,
                                    uint8_t     channel,
                                    uint16_t    oversample_ratio,
                                    uint16_t    sample_cycles,
                                    uint8_t     resolution_bits)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	/* Resolution must be one of the supported widths (or 0 for
     * "firmware default") -- catch early so a typo doesn't have
     * to round-trip the wire. */
	switch (resolution_bits) {
	case 0u:
	case 6u:
	case 8u:
	case 10u:
	case 12u:
	case 14u:
	case 16u:
		break;
	default:
		return ALP_ERR_INVAL;
	}
	uint8_t req[7];
	req[0] = channel;
	req[1] = 0u; /* reserved */
	req[2] = (uint8_t)(oversample_ratio & 0xFFu);
	req[3] = (uint8_t)((oversample_ratio >> 8) & 0xFFu);
	req[4] = (uint8_t)(sample_cycles & 0xFFu);
	req[5] = (uint8_t)((sample_cycles >> 8) & 0xFFu);
	req[6] = resolution_bits;
	return cmd_send(
	    ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_ADC_CONFIGURE, req, sizeof(req), NULL, 0u);
}

alp_status_t gd32g553_adc_stream_begin(gd32g553_t *ctx,
                                       uint8_t     stream_id,
                                       uint8_t     channel,
                                       uint32_t    sample_rate_hz)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (stream_id >= GD32G553_BRIDGE_ADC_STREAM_COUNT) return ALP_ERR_INVAL;
	if (sample_rate_hz == 0u) return ALP_ERR_INVAL;
	uint8_t req[7];
	req[0] = stream_id;
	req[1] = channel;
	req[2] = 0u; /* reserved */
	put_le32(&req[3], sample_rate_hz);
	return cmd_send(
	    ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_ADC_STREAM_BEGIN, req, sizeof(req), NULL, 0u);
}

alp_status_t gd32g553_adc_stream_read(gd32g553_t *ctx,
                                      uint8_t     stream_id,
                                      uint8_t     max_samples,
                                      uint8_t    *got_samples,
                                      uint16_t   *mv)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (got_samples == NULL || mv == NULL) return ALP_ERR_INVAL;
	if (stream_id >= GD32G553_BRIDGE_ADC_STREAM_COUNT) return ALP_ERR_INVAL;
	if (max_samples == 0u) return ALP_ERR_INVAL;
	if (max_samples > GD32G553_BRIDGE_ADC_STREAM_READ_MAX) {
		max_samples = GD32G553_BRIDGE_ADC_STREAM_READ_MAX;
	}

	/* cmd_send needs the reply length up-front; for streaming we
     * pre-commit to `1 + max_samples * 2` reply bytes regardless of
     * how many samples the firmware actually has ready -- byte 0
     * carries the real count `got`, and slots past `got` are
     * zero-padded by the firmware so the on-wire envelope length
     * stays deterministic. */
	uint8_t       reply[1u + (GD32G553_BRIDGE_ADC_STREAM_READ_MAX * 2u)];
	const size_t  reply_len = 1u + ((size_t)max_samples * 2u);
	const uint8_t req[2]    = { stream_id, max_samples };
	alp_status_t  s         = cmd_send(ctx,
	                                   GD32G553_TRANSPORT_DEFAULT,
	                                   GD32G553_CMD_ADC_STREAM_READ,
	                                   req,
	                                   sizeof(req),
	                                   reply,
	                                   reply_len);
	if (s != ALP_OK) {
		*got_samples = 0u;
		return s;
	}
	const uint8_t got = reply[0];
	if (got > max_samples) return ALP_ERR_IO; /* firmware contract violation */
	*got_samples = got;
	for (uint8_t i = 0u; i < got; ++i) {
		mv[i] = (uint16_t)reply[1u + i * 2u] | ((uint16_t)reply[1u + i * 2u + 1u] << 8);
	}
	return ALP_OK;
}

alp_status_t gd32g553_adc_stream_end(gd32g553_t *ctx, uint8_t stream_id)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (stream_id >= GD32G553_BRIDGE_ADC_STREAM_COUNT) return ALP_ERR_INVAL;
	const alp_status_t s = cmd_send(
	    ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_ADC_STREAM_END, &stream_id, 1u, NULL, 0u);
	if (s == ALP_OK) {
		/* The stream (and any watermark events it owed) is gone. */
		ctx->stream2_armed &= (uint8_t)~(1u << stream_id);
		ctx->stream2_seen &= (uint8_t)~(1u << stream_id);
	}
	return s;
}

/* ------------------------------------------------------------------ */
/* v0.15 -- ATTN event path, ADC_STREAM2 (BEGIN2 / READ2), BATCH       */
/*                                                                    */
/* All SPI-only: they force the SPI transport and need the matching    */
/* link feature granted (see spi_negotiate()).  Wire layouts are in    */
/* docs/gd32-bridge-protocol.md §3.16 - §3.18.                         */
/* ------------------------------------------------------------------ */

alp_status_t gd32g553_attn_wait_event(gd32g553_t *ctx, uint32_t timeout_ms)
{
	if (ctx == NULL) return ALP_ERR_INVAL;
	if (!ctx->initialised) return ALP_ERR_NOT_READY;
	if (!ctx->attn_active) return ALP_ERR_NOSUPPORT;
	uint32_t t_edge = 0u;
	if (ctx->attn.wait(ctx->attn.user, timeout_ms, &t_edge) != ALP_OK) return ALP_ERR_TIMEOUT;
	ctx->attn_event_edge = true;
	return ALP_OK;
}

static bool stream2_granted(const gd32g553_t *ctx)
{
	return ctx->spi != NULL && (ctx->granted & GD32G553_LINK_FEAT_ADC_STREAM2) != 0u;
}

/* READ2 ceiling for this link: floor((mp - 9) / 2) -- 28 at 65, 121 at 252. */
static size_t read2_max_samples(const gd32g553_t *ctx)
{
	return (link_max_payload(ctx) - GD32G553_READ2_HDR_BYTES) / 2u;
}

/* READ2 reply length from its own `got` byte; `arg` = max_samples asked.
 * A `got` above that is a contract violation the CRC cannot vouch for. */
static size_t read2_reply_len(const uint8_t *payload, size_t avail, const void *arg)
{
	const size_t max_samples = *(const uint8_t *)arg;
	if (avail < GD32G553_READ2_HDR_BYTES) return REPLY_LEN_INVALID;
	const size_t got = payload[8];
	if (got > max_samples) return REPLY_LEN_INVALID;
	return GD32G553_READ2_HDR_BYTES + (2u * got);
}

alp_status_t gd32g553_adc_stream_begin2(gd32g553_t                  *ctx,
                                        uint8_t                      stream_id,
                                        uint8_t                      channel,
                                        uint32_t                     sample_rate_hz,
                                        uint16_t                     watermark,
                                        gd32g553_adc_stream2_info_t *info)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (stream_id >= GD32G553_BRIDGE_ADC_STREAM_COUNT) return ALP_ERR_INVAL;
	if (sample_rate_hz == 0u) return ALP_ERR_INVAL;
	if (sample_rate_hz > GD32G553_BRIDGE_ADC_STREAM_MAX_RATE_HZ) return ALP_ERR_OUT_OF_RANGE;
	if (channel > 7u) return ALP_ERR_OUT_OF_RANGE;
	switch (watermark) {
	case 0u:
	case 16u:
	case 32u:
	case 64u:
	case 128u:
	case 256u:
	case 512u:
		break;
	default:
		return ALP_ERR_INVAL;
	}
	if (!stream2_granted(ctx)) return ALP_ERR_NOSUPPORT;

	uint8_t req[12];
	req[0] = stream_id;
	req[1] = channel;
	req[2] = GD32G553_STREAM2_TRIGGER_PACE_TIMER;
	req[3] = 0u; /* trigger_arg: must be 0 for the pace timer */
	put_le32(&req[4], sample_rate_hz);
	req[8]  = (uint8_t)(watermark & 0xFFu);
	req[9]  = (uint8_t)(watermark >> 8);
	req[10] = 0u; /* reserved */
	req[11] = 0u;

	uint8_t      reply[17];
	alp_status_t s = cmd_send(ctx,
	                          GD32G553_TRANSPORT_SPI,
	                          GD32G553_CMD_ADC_STREAM_BEGIN2,
	                          req,
	                          sizeof(req),
	                          reply,
	                          sizeof(reply));
	if (s != ALP_OK) return s;

	if (info != NULL) {
		info->tick_hz      = get_le32(&reply[0]);
		info->period_ticks = get_le32(&reply[4]);
		info->full_scale   = (uint16_t)(reply[8] | ((uint16_t)reply[9] << 8));
		info->vref_mv      = (uint16_t)(reply[10] | ((uint16_t)reply[11] << 8));
		info->flags        = reply[12];
		info->watermark    = (uint16_t)(reply[13] | ((uint16_t)reply[14] << 8));
		info->ring_depth   = (uint16_t)(reply[15] | ((uint16_t)reply[16] << 8));
	}
	/* A fresh stream's delivered index D starts at 0. */
	ctx->stream2_next[stream_id] = 0u;
	ctx->stream2_seen |= (uint8_t)(1u << stream_id);
	if (watermark != 0u) {
		ctx->stream2_armed |= (uint8_t)(1u << stream_id);
	} else {
		ctx->stream2_armed &= (uint8_t)~(1u << stream_id);
	}
	return ALP_OK;
}

uint32_t gd32g553_adc_stream2_read_interval_us(const gd32g553_adc_stream2_info_t *info)
{
	if (info == NULL || info->tick_hz == 0u || info->period_ticks == 0u) return 0u;
	/* The ring the firmware GRANTED, not the watermark asked for: at high
	 * rates it sizes the ring up (>= 5 ms of samples), so granted > requested. */
	const uint32_t samples = (info->watermark != 0u) ? info->watermark : (info->ring_depth / 2u);
	/* samples * period_ticks / tick_hz seconds -> microseconds, in 64 bit. */
	const uint64_t us = ((uint64_t)samples * info->period_ticks * 1000000u) / info->tick_hz;
	return (us > UINT32_MAX) ? UINT32_MAX : (uint32_t)us;
}

/* first_index(n) == first_index(n-1) + got(n-1) + dropped(n), except across a
 * discontinuity sentinel (then first_index must not move).  Shared by
 * standalone READ2 and READ2 sub-replies inside a BATCH, so the two paths
 * keep one running index per stream. */
static void stream2_account(gd32g553_t *ctx,
                            uint8_t     stream_id,
                            uint32_t    first_index,
                            uint32_t    dropped,
                            uint8_t     got)
{
	const uint8_t bit = (uint8_t)(1u << stream_id);
	if ((ctx->stream2_seen & bit) != 0u) {
		const uint32_t expect = (dropped == GD32G553_READ2_DROPPED_UNKNOWN)
		                            ? ctx->stream2_next[stream_id]
		                            : ctx->stream2_next[stream_id] + dropped;
		if (first_index != expect) ctx->stream2_gaps++;
	}
	ctx->stream2_seen |= bit;
	ctx->stream2_next[stream_id] = first_index + got;
}

alp_status_t gd32g553_adc_stream_read2(gd32g553_t *ctx,
                                       uint8_t     stream_id,
                                       uint8_t     max_samples,
                                       uint32_t   *first_index,
                                       uint32_t   *dropped,
                                       uint8_t    *got,
                                       uint16_t   *codes)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (first_index == NULL || dropped == NULL || got == NULL || codes == NULL) {
		return ALP_ERR_INVAL;
	}
	if (stream_id >= GD32G553_BRIDGE_ADC_STREAM_COUNT) return ALP_ERR_INVAL;
	if (max_samples == 0u) return ALP_ERR_INVAL;
	/* Deferred ATTN fault / post-reset renegotiation runs HERE, before this
	 * command's frame is built -- never after its reply is received. */
	spi_prepare(ctx);
	if (!stream2_granted(ctx)) return ALP_ERR_NOSUPPORT;
	if (max_samples > read2_max_samples(ctx)) return ALP_ERR_OUT_OF_RANGE;

	const uint8_t req[2] = { stream_id, max_samples };
	size_t        plen   = 0u;
	*got                 = 0u;
	/* Clock the worst case; the reply's own `got` locates the CRC.  The
	 * payload is read in place out of the context's RX buffer. */
	alp_status_t s = spi_xfer_core(ctx,
	                               GD32G553_CMD_ADC_STREAM_READ2,
	                               req,
	                               sizeof(req),
	                               NULL,
	                               GD32G553_READ2_HDR_BYTES + (2u * (size_t)max_samples),
	                               read2_reply_len,
	                               &max_samples,
	                               &plen);
	if (s == ALP_ERR_IO || s == ALP_ERR_TIMEOUT) ctx->version_cached = false;
	if (s != ALP_OK) return s;

	const uint8_t *p = &ctx->spi_reply[2];
	*first_index     = get_le32(&p[0]);
	*dropped         = get_le32(&p[4]);
	*got             = p[8];
	for (uint8_t i = 0u; i < *got; ++i) {
		codes[i] = (uint16_t)(p[GD32G553_READ2_HDR_BYTES + 2u * i] |
		                      ((uint16_t)p[GD32G553_READ2_HDR_BYTES + 2u * i + 1u] << 8));
	}

	stream2_account(ctx, stream_id, *first_index, *dropped, *got);

	/* Idle-path accounting (§4.6): an edge that READ2 answers with nothing
	 * and no loss, eight times running, is a stuck line, not an event. */
	if (ctx->attn_event_edge) {
		ctx->attn_event_edge = false;
		if (*got == 0u && *dropped == 0u) {
			if (++ctx->attn_idle_run >= GD32G553_ATTN_IDLE_EMPTY_LIMIT) {
				attn_fault(ctx); /* serviced at the start of the next command */
			}
		} else {
			ctx->attn_idle_run = 0u;
		}
	}
	return ALP_OK;
}

/* The ops a BATCH may carry (protocol 0.15 §6.3): bounded, side-effect-
 * local handlers.  `reply_max` is the exact reply length of a fixed-reply
 * op; READ2 is variable (9 + 2 * got) and handled separately. */
struct batch_allow {
	uint8_t op;
	uint8_t req_len;
	uint8_t reply_max;
};

static const struct batch_allow gd32g553_batch_allow[] = {
	{ GD32G553_CMD_PING, 0u, 0u },
	{ GD32G553_CMD_GPIO_READ, 4u, 4u },
	{ GD32G553_CMD_GPIO_WRITE, 8u, 0u },
	{ GD32G553_CMD_PWM_SET, 10u, 0u },
	{ GD32G553_CMD_PWM_GET, 1u, 8u },
	{ GD32G553_CMD_PWM_CAPTURE_READ, 1u, 8u },
	{ GD32G553_CMD_ADC_STREAM_READ2, 2u, 0u },
	{ GD32G553_CMD_DA9292_STATUS_FORWARD, 0u, 1u },
	{ GD32G553_CMD_DAC_SET, 4u, 0u },
	{ GD32G553_CMD_DAC_GET, 1u, 2u },
	{ GD32G553_CMD_QENC_READ, 1u, 4u },
	{ GD32G553_CMD_QENC_RESET, 1u, 0u },
	{ GD32G553_CMD_COUNTER_READ, 1u, 4u },
	{ GD32G553_CMD_TMU_COMPUTE, 12u, 4u },
};

struct batch_plan {
	uint8_t  count;
	uint16_t reply_max[GD32G553_BATCH_MAX_OPS]; /* per-op ceiling */
	bool     variable[GD32G553_BATCH_MAX_OPS];  /* READ2: 9 + 2 * got */
};

/* BATCH reply: `executed:u8` then `{status:u8, len:u8, payload[len]}` per
 * executed op.  Returns the payload length, or REPLY_LEN_INVALID for a
 * reply the request cannot explain (protocol 0.15 §6.5): executed > count,
 * an entry longer than its op's maximum, a fixed-reply op with the wrong
 * length, a failing op carrying data, or entries running past the buffer. */
static size_t batch_reply_len(const uint8_t *p, size_t avail, const void *arg)
{
	const struct batch_plan *plan = arg;
	if (avail < 1u || p[0] > plan->count) return REPLY_LEN_INVALID;

	size_t  pos         = 1u;
	uint8_t last_status = 0u;
	for (unsigned i = 0u; i < p[0]; ++i) {
		if (pos + 2u > avail) return REPLY_LEN_INVALID;
		const uint8_t status = p[pos] & GD32G553_STATUS_CODE_MASK;
		last_status          = status;
		const size_t len     = p[pos + 1u];
		pos += 2u;
		if (pos + len > avail || len > plan->reply_max[i]) return REPLY_LEN_INVALID;
		if (status != 0u) {
			if (len != 0u) return REPLY_LEN_INVALID;
		} else if (plan->variable[i]) {
			/* READ2 sub-reply: 9-byte header + `got` codes. */
			if (len < GD32G553_READ2_HDR_BYTES || ((len - GD32G553_READ2_HDR_BYTES) & 1u) != 0u ||
			    p[pos + 8u] != (len - GD32G553_READ2_HDR_BYTES) / 2u) {
				return REPLY_LEN_INVALID;
			}
		} else if (len != plan->reply_max[i]) {
			return REPLY_LEN_INVALID;
		}
		pos += len;
	}
	/* A batch that stopped short of `count` although its last executed op
	 * succeeded (including one that executed nothing: last_status stays 0)
	 * is not something the firmware produces -- it only stops on an error. */
	if (p[0] < plan->count && last_status == 0u) return REPLY_LEN_INVALID;
	return pos;
}

alp_status_t
gd32g553_batch(gd32g553_t *ctx, gd32g553_batch_op_t *ops, uint8_t count, uint8_t *executed)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (ops == NULL) return ALP_ERR_INVAL;
	if (count == 0u) return ALP_ERR_INVAL;
	if (count > GD32G553_BATCH_MAX_OPS) return ALP_ERR_OUT_OF_RANGE;
	spi_prepare(ctx); /* before this command's frame is built (see READ2) */
	if (ctx->spi == NULL || (ctx->granted & GD32G553_LINK_FEAT_BATCH) == 0u) {
		return ALP_ERR_NOSUPPORT;
	}

	/* Validate, and size the worst-case reply, before touching the wire --
	 * the firmware's validation pass would reject the same shapes, but a
	 * local reject costs no SPI transaction pair. */
	const size_t      mp         = link_max_payload(ctx);
	size_t            req_len    = 1u;
	size_t            worst_rply = 1u;
	struct batch_plan plan       = { .count = count };
	for (unsigned i = 0u; i < count; ++i) {
		const struct batch_allow *a = NULL;
		for (size_t k = 0u; k < sizeof(gd32g553_batch_allow) / sizeof(gd32g553_batch_allow[0]);
		     ++k) {
			if (gd32g553_batch_allow[k].op == ops[i].op) {
				a = &gd32g553_batch_allow[k];
				break;
			}
		}
		if (a == NULL || ops[i].args_len != a->req_len) return ALP_ERR_INVAL;
		if (a->req_len > 0u && ops[i].args == NULL) return ALP_ERR_INVAL;

		uint16_t rmax = a->reply_max;
		if (a->op == GD32G553_CMD_ADC_STREAM_READ2) {
			const uint8_t max_samples = ops[i].args[1];
			if (max_samples == 0u) return ALP_ERR_INVAL;
			/* Two READ2s on one stream in a batch would split one backlog
			 * between them, so the firmware rejects it: reject it here too,
			 * before any bus traffic. */
			if (ops[i].args[0] >= GD32G553_BRIDGE_ADC_STREAM_COUNT) return ALP_ERR_INVAL;
			for (unsigned k = 0u; k < i; ++k) {
				if (plan.variable[k] && ops[k].args[0] == ops[i].args[0]) return ALP_ERR_INVAL;
			}
			if (max_samples > read2_max_samples(ctx)) return ALP_ERR_OUT_OF_RANGE;
			rmax             = (uint16_t)(GD32G553_READ2_HDR_BYTES + 2u * (uint16_t)max_samples);
			plan.variable[i] = true;
		}
		if (rmax > 0u && (ops[i].reply == NULL || ops[i].reply_cap < rmax)) return ALP_ERR_INVAL;

		plan.reply_max[i] = rmax;
		req_len += 2u + a->req_len;
		worst_rply += 2u + rmax;
	}
	if (req_len > mp || worst_rply > mp) return ALP_ERR_OUT_OF_RANGE;

	/* Assemble `count, {op, len, args[len]}...` straight into the SPI TX
	 * buffer (spi_xfer_core recognises the in-place payload). */
	uint8_t *pl = &ctx->spi_req[2];
	size_t   w  = 0u;
	pl[w++]     = count;
	for (unsigned i = 0u; i < count; ++i) {
		pl[w++] = ops[i].op;
		pl[w++] = ops[i].args_len;
		if (ops[i].args_len > 0u) memcpy(&pl[w], ops[i].args, ops[i].args_len);
		w += ops[i].args_len;
	}

	size_t       plen = 0u;
	alp_status_t s    = spi_xfer_core(
	    ctx, GD32G553_CMD_BATCH, pl, w, NULL, worst_rply, batch_reply_len, &plan, &plen);
	if (s == ALP_ERR_IO || s == ALP_ERR_TIMEOUT) ctx->version_cached = false;
	if (s != ALP_OK) return s;

	const uint8_t *p   = &ctx->spi_reply[2];
	const uint8_t  n   = p[0];
	size_t         pos = 1u;
	for (unsigned i = 0u; i < count; ++i) {
		ops[i].status    = ALP_ERR_NOT_READY; /* not executed */
		ops[i].reply_len = 0u;
		if (i >= n) continue;
		const uint8_t len = p[pos + 1u];
		ops[i].status     = status_from_wire(p[pos] & GD32G553_STATUS_CODE_MASK);
		ops[i].reply_len  = len;
		if (len > 0u) memcpy(ops[i].reply, &p[pos + 2u], len);
		if (ops[i].op == GD32G553_CMD_ADC_STREAM_READ2 && ops[i].status == ALP_OK) {
			/* decode validated the length and `got`; keep the stream's running
			 * index in step so the next standalone READ2 is not a false gap */
			const uint8_t *r = &p[pos + 2u];
			stream2_account(ctx, ops[i].args[0], get_le32(&r[0]), get_le32(&r[4]), r[8]);
		}
		pos += 2u + len;
	}
	if (executed != NULL) *executed = n;
	return ALP_OK;
}

alp_status_t gd32g553_adc_spectrum_read(gd32g553_t *ctx,
                                        uint8_t     stream_id,
                                        uint16_t    bin_offset,
                                        uint8_t     max_bins,
                                        uint32_t   *seq_out,
                                        uint16_t   *total_bins_out,
                                        uint8_t    *got_bins,
                                        float      *bins)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (seq_out == NULL || total_bins_out == NULL || got_bins == NULL || bins == NULL) {
		return ALP_ERR_INVAL;
	}
	if (stream_id >= GD32G553_BRIDGE_ADC_STREAM_COUNT) return ALP_ERR_INVAL;
	if (max_bins == 0u) return ALP_ERR_INVAL;
	if (max_bins > GD32G553_BRIDGE_ADC_SPECTRUM_READ_MAX) {
		max_bins = GD32G553_BRIDGE_ADC_SPECTRUM_READ_MAX;
	}

	/* Fixed reply envelope like stream_read: seq(4) total(2) got(1) +
	 * max_bins float32, zero-padded past `got`. */
	uint8_t       reply[7u + (GD32G553_BRIDGE_ADC_SPECTRUM_READ_MAX * 4u)];
	const size_t  reply_len = 7u + ((size_t)max_bins * 4u);
	const uint8_t req[4]    = {
		stream_id, (uint8_t)(bin_offset & 0xFFu), (uint8_t)((bin_offset >> 8) & 0xFFu), max_bins
	};
	alp_status_t s = cmd_send(ctx,
	                          GD32G553_TRANSPORT_DEFAULT,
	                          GD32G553_CMD_ADC_SPECTRUM_READ,
	                          req,
	                          sizeof(req),
	                          reply,
	                          reply_len);
	if (s != ALP_OK) {
		*got_bins = 0u;
		return s;
	}
	*seq_out        = (uint32_t)reply[0] | ((uint32_t)reply[1] << 8) | ((uint32_t)reply[2] << 16) |
	                  ((uint32_t)reply[3] << 24);
	*total_bins_out = (uint16_t)reply[4] | ((uint16_t)reply[5] << 8);
	const uint8_t got = reply[6];
	if (got > max_bins) return ALP_ERR_IO; /* firmware contract violation */
	*got_bins = got;
	for (uint8_t i = 0u; i < got; ++i) {
		uint32_t w = (uint32_t)reply[7u + i * 4u] | ((uint32_t)reply[7u + i * 4u + 1u] << 8) |
		             ((uint32_t)reply[7u + i * 4u + 2u] << 16) |
		             ((uint32_t)reply[7u + i * 4u + 3u] << 24);
		memcpy(&bins[i], &w, sizeof(float));
	}
	return ALP_OK;
}

alp_status_t gd32g553_trng_read(gd32g553_t *ctx, uint8_t *dest, size_t len)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (dest == NULL) return ALP_ERR_INVAL;
	if (len == 0u || len > GD32G553_BRIDGE_TRNG_MAX_BYTES) return ALP_ERR_INVAL;
	const uint8_t req = (uint8_t)len;

	/* A max-length pull spans a whole 256-bit NIST conditioning round,
     * so the firmware legitimately answers STATUS_BUSY when its FIFO
     * runs dry mid-request (it bounds its in-handler wait instead of
     * overrunning the reply window).  Ride out a few rounds here --
     * conditioning completes in low milliseconds and randomness is
     * not a latency-critical surface. */
	alp_status_t s = ALP_ERR_BUSY;
	for (unsigned attempt = 0u; attempt < 4u && s == ALP_ERR_BUSY; ++attempt) {
		/* The backoff sleeps rather than spins: it sits BETWEEN cmd_send()
		 * calls with no bus transaction in flight and no lock held, so
		 * yielding is safe, and a 2 ms non-yielding spin per retry buys
		 * nothing. */
		if (attempt != 0u) alp_delay_ms(2u);
		s = cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_TRNG_READ, &req, 1u, dest, len);
	}
	return s;
}

/* ------------------------------------------------------------------ */
/* v0.4 -- GD32G5 TMU (CORDIC) math accelerator                        */
/* ------------------------------------------------------------------ */

alp_status_t gd32g553_tmu_compute(gd32g553_t             *ctx,
                                  gd32g553_tmu_function_t function,
                                  gd32g553_tmu_format_t   format,
                                  uint32_t                in_a,
                                  uint32_t                in_b,
                                  uint32_t               *result_out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (result_out == NULL) return ALP_ERR_INVAL;
	/* Range-check the function + format enums host-side so callers
     * find typos at the API boundary rather than after a wire trip. */
	if ((unsigned)function > (unsigned)GD32G553_TMU_FN_HYPOT) return ALP_ERR_INVAL;
	if ((unsigned)format > (unsigned)GD32G553_TMU_FMT_F32) return ALP_ERR_INVAL;

	uint8_t req[12];
	req[0] = (uint8_t)function;
	req[1] = (uint8_t)format;
	req[2] = 0u; /* reserved */
	req[3] = 0u; /* reserved */
	put_le32(&req[4], in_a);
	put_le32(&req[8], in_b);

	uint8_t      reply[4] = { 0 };
	alp_status_t s        = cmd_send(ctx,
	                                 GD32G553_TRANSPORT_DEFAULT,
	                                 GD32G553_CMD_TMU_COMPUTE,
	                                 req,
	                                 sizeof(req),
	                                 reply,
	                                 sizeof(reply));
	if (s != ALP_OK) return s;
	*result_out = get_le32(reply);
	return ALP_OK;
}

/* ----------------------------------------------------------------- */
/* v0.5 (§2B.2 + §2B.3) -- advanced timer extras + power-mode set    */
/*                                                                    */
/* Wire frames per docs/gd32-bridge-protocol.md.  Firmware returns   */
/* STATUS_NOSUPPORT today via the default-case dispatch until the    */
/* corresponding bridge_hw_* HAL bodies land; the host helpers below */
/* return ALP_ERR_NOSUPPORT in lockstep.                              */
/* ----------------------------------------------------------------- */

alp_status_t gd32g553_pwm_capture_begin(gd32g553_t *ctx, uint8_t channel, uint8_t edge)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (edge > 2u) return ALP_ERR_INVAL;
	uint8_t req[2] = { channel, edge };
	return cmd_send(ctx,
	                GD32G553_TRANSPORT_DEFAULT,
	                GD32G553_CMD_PWM_CAPTURE_BEGIN,
	                req,
	                sizeof(req),
	                NULL,
	                0u);
}

alp_status_t
gd32g553_pwm_capture_read(gd32g553_t *ctx, uint8_t channel, uint32_t *period_ns, uint32_t *pulse_ns)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (period_ns == NULL && pulse_ns == NULL) return ALP_ERR_INVAL;
	uint8_t      req[1]   = { channel };
	uint8_t      reply[8] = { 0 };
	alp_status_t s        = cmd_send(ctx,
	                                 GD32G553_TRANSPORT_DEFAULT,
	                                 GD32G553_CMD_PWM_CAPTURE_READ,
	                                 req,
	                                 sizeof(req),
	                                 reply,
	                                 sizeof(reply));
	if (s != ALP_OK) return s;
	if (period_ns != NULL) *period_ns = get_le32(&reply[0]);
	if (pulse_ns != NULL) *pulse_ns = get_le32(&reply[4]);
	return ALP_OK;
}

alp_status_t gd32g553_pwm_capture_end(gd32g553_t *ctx, uint8_t channel)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	uint8_t req[1] = { channel };
	return cmd_send(
	    ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_PWM_CAPTURE_END, req, sizeof(req), NULL, 0u);
}

alp_status_t gd32g553_pwm_single_pulse(gd32g553_t *ctx, uint8_t channel, uint32_t pulse_ns)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (pulse_ns == 0u) return ALP_ERR_INVAL;
	uint8_t req[8];
	req[0] = channel;
	req[1] = 0u; /* reserved */
	req[2] = 0u;
	req[3] = 0u;
	put_le32(&req[4], pulse_ns);
	return cmd_send(
	    ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_PWM_SINGLE_PULSE, req, sizeof(req), NULL, 0u);
}

alp_status_t gd32g553_timer_sync(gd32g553_t *ctx, uint8_t master, uint8_t slave, uint8_t mode)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	uint8_t req[3] = { master, slave, mode };
	return cmd_send(
	    ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_TIMER_SYNC, req, sizeof(req), NULL, 0u);
}

static alp_status_t power_mode_send(gd32g553_t *ctx,
                                    uint8_t     mode,
                                    uint8_t     flags,
                                    uint32_t    wake_bitmap,
                                    uint32_t    wake_after_ms)
{
	uint8_t req[10];
	req[0] = mode;
	req[1] = flags;
	put_le32(&req[2], wake_bitmap);
	put_le32(&req[6], wake_after_ms);
	/* SPI when there is one: the opcode is not on the I2C allow-list. */
	const gd32g553_transport_t t =
	    (ctx->spi != NULL) ? GD32G553_TRANSPORT_SPI : GD32G553_TRANSPORT_DEFAULT;
	return cmd_send(ctx, t, GD32G553_CMD_POWER_MODE_SET, req, sizeof(req), NULL, 0u);
}

alp_status_t
gd32g553_power_mode_set(gd32g553_t *ctx, uint8_t mode, uint32_t wake_bitmap, uint32_t wake_after_ms)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (mode > 3u) return ALP_ERR_INVAL;
	return power_mode_send(ctx, mode, 0u, wake_bitmap, wake_after_ms);
}

alp_status_t gd32g553_power_wake(gd32g553_t *ctx)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (!ctx->power_asleep) return ALP_OK;

	/* Cleared first so the commands below do not recurse into this wake; put
	 * back on failure so the next command retries it. */
	ctx->power_asleep = false;
	alp_status_t s    = ALP_ERR_IO;
	for (uint8_t attempt = 0u; attempt <= ctx->power_wake_retries; ++attempt) {
		/* Wake pulse.  A CS wake loses the frame clocked during it (it fails
		 * its CRC on IRC8M and never reaches a handler), so the answer, or
		 * the lack of one, means nothing. */
		(void)cmd_send(ctx, GD32G553_TRANSPORT_SPI, GD32G553_CMD_PING, NULL, 0u, NULL, 0u);
		alp_delay_us(ctx->power_wake_latency_us);
		/* RUN: idempotent, proves the link is back, and cancels a request
		 * that was accepted but never reached the sleep. */
		s = power_mode_send(ctx, 0u, 0u, 0u, 0u);
		if (s != ALP_ERR_IO && s != ALP_ERR_TIMEOUT) break;
	}
	if (s == ALP_ERR_IO || s == ALP_ERR_TIMEOUT) ctx->power_asleep = true;
	/* Any other answer (OK, or a firmware without the opcode) means it is awake. */
	return (s == ALP_ERR_NOSUPPORT || s == ALP_ERR_INVAL) ? ALP_OK : s;
}

alp_status_t
gd32g553_set_power_mode(gd32g553_t *ctx, uint8_t mode, const gd32g553_power_opts_t *opts)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (mode > 3u) return ALP_ERR_INVAL;
	const gd32g553_power_opts_t zero = { 0 };
	if (opts == NULL) opts = &zero;
	if ((opts->flags & (uint8_t)~GD32G553_POWER_FLAG_WAKE_I2C) != 0u) return ALP_ERR_INVAL;
	if (opts->flags != 0u && mode != 2u) return ALP_ERR_INVAL;

	/* The bridge may still be asleep from an earlier request. */
	alp_status_t s = gd32g553_power_wake(ctx);
	if (s != ALP_OK) return s;

	s = power_mode_send(ctx, mode, opts->flags, opts->wake_bitmap, opts->wake_after_ms);
	if (s != ALP_OK) return s;

	if (mode == 2u) {
		ctx->power_asleep = true;
		ctx->power_wake_latency_us =
		    (opts->wake_latency_us != 0u) ? opts->wake_latency_us
		                                  : GD32G553_POWER_WAKE_LATENCY_US_DEFAULT;
		ctx->power_wake_retries = (opts->wake_retries != 0u) ? opts->wake_retries
		                                                     : GD32G553_POWER_WAKE_RETRIES_DEFAULT;
	} else if (mode == 3u) {
		/* The wake is a reset: link features, sequencing and streams are gone. */
		spi_drop_negotiated(ctx);
		ctx->version_cached      = false;
		ctx->renegotiate_pending = (ctx->spi != NULL);
	}
	return ALP_OK;
}

/* ----------------------------------------------------------------- */
/* v0.5 (§2B wave-2) -- chunked DSP-chain upload                     */
/*                                                                    */
/* Wire frames per docs/gd32-bridge-protocol.md §3.x.  Firmware       */
/* returns STATUS_NOSUPPORT today via the default-case dispatch       */
/* until the bridge_hw_adc_dsp_* HAL bodies land; the host helpers    */
/* below short-circuit to ALP_ERR_NOSUPPORT in lockstep with that     */
/* contract -- the wire request still serialises correctly so the    */
/* firmware-side handler tree sees real envelopes when it ships.     */
/* ----------------------------------------------------------------- */

alp_status_t gd32g553_adc_dsp_chain_open(gd32g553_t *ctx, uint8_t *chain_id_out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (chain_id_out == NULL) return ALP_ERR_INVAL;

	uint8_t      reply[1] = { 0 };
	alp_status_t s        = cmd_send(ctx,
	                                 GD32G553_TRANSPORT_DEFAULT,
	                                 GD32G553_CMD_ADC_DSP_CHAIN_OPEN,
	                                 NULL,
	                                 0u,
	                                 reply,
	                                 sizeof(reply));
	if (s != ALP_OK) return s;
	*chain_id_out = reply[0];
	return ALP_OK;
}

alp_status_t gd32g553_adc_dsp_stage_push(gd32g553_t    *ctx,
                                         uint8_t        chain_id,
                                         uint8_t        stage_index,
                                         uint8_t        kind,
                                         const uint8_t *stage_params,
                                         uint16_t       stage_params_len)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (stage_index >= GD32G553_BRIDGE_ADC_DSP_MAX_STAGES) return ALP_ERR_INVAL;
	if (kind > 3u) return ALP_ERR_INVAL;
	if (stage_params == NULL && stage_params_len != 0u) return ALP_ERR_INVAL;
	if (stage_params_len > GD32G553_BRIDGE_ADC_DSP_MAX_STAGE_BYTES) return ALP_ERR_OUT_OF_RANGE;

	/* Chunk the per-kind blob into wire envelopes no larger than
     * MAX_CHUNK_BYTES.  Empty blobs (stage_params_len == 0) still
     * send a single zero-length STAGE_PUSH so the firmware sees the
     * kind declaration arrive at the requested stage_index.  The
     * 7-byte header is chain_id + stage_index + kind + chunk_offset:u16
     * + chunk_total_size:u16; chunk_total_size carries the FULL
     * per-kind blob length (not this chunk's length) so the firmware
     * knows when assembly is complete. */
	uint16_t offset = 0u;
	do {
		uint16_t this_chunk = (uint16_t)(stage_params_len - offset);
		if (this_chunk > GD32G553_BRIDGE_ADC_DSP_MAX_CHUNK_BYTES) {
			this_chunk = GD32G553_BRIDGE_ADC_DSP_MAX_CHUNK_BYTES;
		}
		uint8_t req[7u + GD32G553_BRIDGE_ADC_DSP_MAX_CHUNK_BYTES];
		req[0] = chain_id;
		req[1] = stage_index;
		req[2] = kind;
		req[3] = (uint8_t)(offset & 0xFFu);
		req[4] = (uint8_t)((offset >> 8) & 0xFFu);
		req[5] = (uint8_t)(stage_params_len & 0xFFu);
		req[6] = (uint8_t)((stage_params_len >> 8) & 0xFFu);
		if (this_chunk > 0u) {
			memcpy(&req[7], stage_params + offset, this_chunk);
		}
		alp_status_t s = cmd_send(ctx,
		                          GD32G553_TRANSPORT_DEFAULT,
		                          GD32G553_CMD_ADC_DSP_STAGE_PUSH,
		                          req,
		                          (size_t)(7u + this_chunk),
		                          NULL,
		                          0u);
		if (s != ALP_OK) return s;
		offset = (uint16_t)(offset + this_chunk);
	} while (offset < stage_params_len);

	return ALP_OK;
}

alp_status_t gd32g553_adc_dsp_chain_bind(gd32g553_t *ctx, uint8_t chain_id, uint8_t stream_id)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (stream_id >= GD32G553_BRIDGE_ADC_STREAM_COUNT) return ALP_ERR_INVAL;
	uint8_t req[2] = { chain_id, stream_id };
	return cmd_send(ctx,
	                GD32G553_TRANSPORT_DEFAULT,
	                GD32G553_CMD_ADC_DSP_CHAIN_BIND,
	                req,
	                sizeof(req),
	                NULL,
	                0u);
}

/* ----------------------------------------------------------------- */
/* OTA helpers -- the firmware-side opcodes return STATUS_NOSUPPORT  */
/* against the scaffold today, which maps to ALP_ERR_NOSUPPORT here. */
/* When the bridge ships real bodies the same call paths return the  */
/* documented payloads.  GET_STATE answers concretely against the    */
/* scaffold so customer telemetry already works.                      */
/* ----------------------------------------------------------------- */

bool gd32g553_ota_supported(const gd32g553_t *ctx)
{
	return ctx != NULL && ctx->initialised && ctx->version.minor >= GD32G553_OTA_MIN_PROTOCOL_MINOR;
}

alp_status_t gd32g553_ota_begin(gd32g553_t               *ctx,
                                uint32_t                  size_bytes,
                                uint32_t                  expected_crc32,
                                const gd32g553_version_t *fw_version,
                                uint16_t                 *chunk_max_bytes,
                                gd32g553_ota_slot_t      *target_slot)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (chunk_max_bytes == NULL || target_slot == NULL) return ALP_ERR_INVAL;
	if (size_bytes == 0u) return ALP_ERR_INVAL;
	/* Refuse a pre-v0.6 peer BEFORE the bridge erases the target slot: the
	 * v0.6 chunk framing it would receive corrupts the image (#751). */
	if (!gd32g553_ota_supported(ctx)) return ALP_ERR_NOSUPPORT;

	/* v0.7 additive form: size:u32, crc:u32 [, maj:u8, min:u8, pat:u8].
     * The version triple lands in the bridge's A/B metadata record at
     * COMMIT (fw_version[slot], "0 = unknown").  NULL = send the legacy
     * 8-byte form; older firmware ignores the 3 trailing bytes of the
     * long form, so either pairing degrades gracefully. */
	uint8_t      req[11];
	const size_t req_len = (fw_version != NULL) ? 11u : 8u;
	put_le32(&req[0], size_bytes);
	put_le32(&req[4], expected_crc32);
	if (fw_version != NULL) {
		req[8]  = fw_version->major;
		req[9]  = fw_version->minor;
		req[10] = fw_version->patch;
	}

	/* Reply: chunk_max_bytes:u16 + target_slot:u8. */
	uint8_t      reply[3] = { 0 };
	alp_status_t s        = cmd_send(ctx,
	                                 GD32G553_TRANSPORT_DEFAULT,
	                                 GD32G553_CMD_OTA_BEGIN,
	                                 req,
	                                 req_len,
	                                 reply,
	                                 sizeof(reply));
	if (s != ALP_OK) return s;
	*chunk_max_bytes = (uint16_t)reply[0] | ((uint16_t)reply[1] << 8);
	*target_slot     = (gd32g553_ota_slot_t)reply[2];
	return ALP_OK;
}

alp_status_t gd32g553_ota_write_chunk(gd32g553_t    *ctx,
                                      uint32_t       offset,
                                      const uint8_t *data,
                                      size_t         data_len,
                                      uint32_t      *received_bytes)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (data == NULL || data_len == 0u) return ALP_ERR_INVAL;
	if (received_bytes == NULL) return ALP_ERR_INVAL;
	/* Belt-and-suspenders: even if a caller skips BEGIN's gate, never
	 * program the v0.6 chunk layout into a pre-v0.6 peer (#751). */
	if (!gd32g553_ota_supported(ctx)) return ALP_ERR_NOSUPPORT;

	/* Chunk size is bounded by the wire payload ceiling.  Bridges
     * that advertise a smaller chunk_max_bytes in BEGIN are honoured
     * by the caller -- this helper only enforces the absolute limit.
     * (v0.6: the payload carries an explicit len byte after the
     * offset, so the data ceiling is MAX_PAYLOAD - 5.) */
	if (data_len > GD32G553_MAX_PAYLOAD_BYTES - 5u) return ALP_ERR_INVAL;

	uint8_t req[GD32G553_MAX_PAYLOAD_BYTES] = { 0 };
	put_le32(&req[0], offset);
	req[4] = (uint8_t)data_len; /* v0.6 anti-extension cross-check */
	memcpy(&req[5], data, data_len);

	uint8_t      reply[4] = { 0 };
	alp_status_t s        = cmd_send(ctx,
	                                 GD32G553_TRANSPORT_DEFAULT,
	                                 GD32G553_CMD_OTA_WRITE_CHUNK,
	                                 req,
	                                 5u + data_len,
	                                 reply,
	                                 sizeof(reply));
	if (s != ALP_OK) return s;
	*received_bytes = get_le32(reply);
	return ALP_OK;
}

alp_status_t gd32g553_ota_verify(gd32g553_t *ctx, bool *verified, uint32_t *computed_crc32)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (verified == NULL || computed_crc32 == NULL) return ALP_ERR_INVAL;

	/* Reply: computed_crc32(u32) + verified(u8). */
	uint8_t      reply[5] = { 0 };
	alp_status_t s        = cmd_send(
	    ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_OTA_VERIFY, NULL, 0u, reply, sizeof(reply));
	if (s != ALP_OK) return s;
	*computed_crc32 = get_le32(&reply[0]);
	*verified       = (reply[4] != 0u);
	return ALP_OK;
}

/* COMMIT / ROLLBACK reset the bridge: its link features revert to off
 * (PA14 returns to SWCLK), so drop the host-side negotiated state too.  The caller re-inits
 * (which re-negotiates); until then the link runs un-sequenced instead of
 * misreading the reset bridge's stamp-0 replies as stale. */
static alp_status_t ota_reset_cmd(gd32g553_t *ctx, uint8_t cmd)
{
	const alp_status_t s = cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, cmd, NULL, 0u, NULL, 0u);
	if (s == ALP_OK) {
		/* Sequencing, BIG_FRAME, ATTN, STREAM2, BATCH and every stream
		 * handle are gone with the reset. */
		spi_drop_negotiated(ctx);
		/* The bridge reboots into a (possibly different) image: the next
		 * SPI command re-reads the version and re-runs the negotiation (§8)
		 * instead of leaving BEGIN2 / BATCH / ATTN failing with NOSUPPORT. */
		ctx->version_cached      = false;
		ctx->renegotiate_pending = (ctx->spi != NULL);
	}
	return s;
}

alp_status_t gd32g553_ota_commit(gd32g553_t *ctx)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	return ota_reset_cmd(ctx, GD32G553_CMD_OTA_COMMIT);
}

alp_status_t gd32g553_ota_rollback(gd32g553_t *ctx)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	return ota_reset_cmd(ctx, GD32G553_CMD_OTA_ROLLBACK);
}

alp_status_t gd32g553_ota_get_state(gd32g553_t *ctx, gd32g553_ota_state_info_t *out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (out == NULL) return ALP_ERR_INVAL;

	/* Reply: state(u8) + active(u8) + pending(u8) + boot_count(u16 LE)
	 * [+ err(u8) since protocol v0.14, gh#101].  The wire carries no
	 * length (docs/gd32-bridge-protocol.md §4), so the host must know
	 * up front how many bytes to clock -- branch on the MINOR this
	 * link negotiated at init() rather than always reading 6: an older
	 * bridge never appends the err byte, and reading past what it sent
	 * would desync the CRC over a byte that was never on the wire. */
	const bool   has_err   = ctx->version.minor >= GD32G553_OTA_ERR_MIN_PROTOCOL_MINOR;
	const size_t reply_len = has_err ? 6u : 5u;
	uint8_t      reply[6]  = { 0 };
	alp_status_t s         = cmd_send(
	    ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_OTA_GET_STATE, NULL, 0u, reply, reply_len);
	if (s != ALP_OK) return s;
	out->state        = (gd32g553_ota_state_t)reply[0];
	out->active_slot  = (gd32g553_ota_slot_t)reply[1];
	out->pending_slot = (gd32g553_ota_slot_t)reply[2];
	out->boot_count   = (uint16_t)reply[3] | ((uint16_t)reply[4] << 8);
	out->err          = has_err ? (gd32g553_ota_err_t)reply[5] : GD32G553_OTA_ERR_NONE;
	return ALP_OK;
}

alp_status_t gd32g553_ota_abort(gd32g553_t *ctx)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_OTA_ABORT, NULL, 0u, NULL, 0u);
}

void gd32g553_deinit(gd32g553_t *ctx)
{
	if (ctx == NULL) return;
	/* Bus handles are owned by the caller -- don't close them. */
	ctx->initialised    = false;
	ctx->version_cached = false;
	spi_drop_negotiated(ctx);
	ctx->spi = NULL;
	ctx->i2c = NULL;
}
