/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * CC3501E SPI1 host-passthrough helpers (0x55..0x57).  See
 * <alp/chips/cc3501e/core.h> for the public API.
 *
 * The E1M connector's SPI1 lands on the CC3501E, NOT on the Alif
 * (E1M-AEN-2626-R2 netlist: AG10 SCK -> CC35 GPIO_32, AG9 MOSI -> GPIO_33,
 * AG8 MISO -> GPIO_34, AH9 CS0 -> GPIO_31, AH8 CS1 -> GPIO_15), so a device on
 * that bus is driven by RELAY: the CC3501E is the SPI controller and these
 * calls hand it the bytes.  Nothing here touches the inter-chip bridge itself
 * -- that is a different CC35 instance (SPI0, GPIO_27/28/29 + GPIO16).
 *
 * The firmware worker-routes all three opcodes: a polled 4 KB controller
 * transfer is ~800 us at 10 MHz, and running that inside the SPI0 slave ISR
 * would stall the slave's re-arm -- the wedge signature this link has spent
 * months chasing.  Worker-routed means poll-by-repeat, i.e. the host re-issues
 * the SAME frame while the firmware answers RESP_ERR_BUSY; the seq byte below
 * is what makes that safe on a bus that drives flash.
 *
 * Wire structs here are category (B): hand-packed field by field in explicit
 * little-endian, never memcpy'd or pointer-cast onto the frame.
 */

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "cc3501e_internal.h"

/* v6 firmware accepts 8 and rejects everything else with RESP_ERR_INVALID.
 * The field rides the wire so a later firmware can widen the word size without
 * another opcode, but until one does, exposing it as a parameter would only let
 * a caller spend a round trip being told no -- so it is pinned here, and the
 * parameter arrives when a firmware that honours it does. */
#define CC3501E_SPI1_BITS_PER_WORD 8u

/* Decode the TRANSFER reply -- { len(LE16) | flags | seq } followed inline by
 * the received bytes -- into the caller's buffer.  The length that matters is
 * the one INSIDE the reply, not the frame's: the frame's declared payload_len
 * includes the zero pad, which is why every variable-length reply on this wire
 * has to be self-delimiting. */
static alp_status_t
spi1_take_rx(const uint8_t *reply, size_t got, uint8_t seq, uint8_t *rx, uint16_t len)
{
	const size_t hdr = sizeof(alp_cc3501e_spi1_transfer_resp_t);
	if (got < hdr) return ALP_ERR_IO; /* short reply header -- link desync */
	/* The reply echoes the request's seq.  A mismatch means this is the answer to
	 * some OTHER request (the firmware's cached one, or a desynced read), not to
	 * ours -- report the desync rather than hand the caller another transaction's
	 * bytes.  The reply's CS_HOLD bit (reply[2]) is deliberately not surfaced: it
	 * is only an echo of the request's own CS_HOLD, not an independent readback,
	 * so the caller already has it -- and a transfer that failed to leave CS
	 * where it asked returns an error rather than a flag. */
	if (reply[3] != seq) return ALP_ERR_IO;
	if (rx == NULL) return ALP_OK; /* NO_RX was set: firmware reports len 0 */

	const uint16_t rx_len = (uint16_t)((uint16_t)reply[0] | ((uint16_t)reply[1] << 8));
	/* Exactly len bytes or nothing.  The caller's buffer is sized for len and this
	 * API has no short-count out-parameter, so a truncated reply must not leave
	 * half of it holding stale data that reads as received. */
	if (rx_len != len || got < hdr + (size_t)len) return ALP_ERR_IO;
	if (len > 0u) memcpy(rx, &reply[hdr], len);
	return ALP_OK;
}

/* See <alp/chips/cc3501e/core.h>. */
alp_status_t cc3501e_spi1_configure(cc3501e_t            *ctx,
                                    uint32_t              freq_hz,
                                    uint8_t               mode,
                                    alp_cc3501e_spi1_cs_t cs,
                                    uint32_t             *actual_freq_hz_out,
                                    uint16_t             *max_xfer_out,
                                    uint32_t              timeout_ms)
{
	if (ctx == NULL) return ALP_ERR_INVAL;
	/* Reject locally what the firmware rejects anyway: a bad mode or cs is a
	 * caller bug, and learning it from the peer costs a full round trip on a link
	 * where round trips are the entire cost. */
	if (mode > 3u || (uint8_t)cs > (uint8_t)ALP_CC3501E_SPI1_CS1) return ALP_ERR_INVAL;
	if (actual_freq_hz_out != NULL) *actual_freq_hz_out = 0u;
	if (max_xfer_out != NULL) *max_xfer_out = 0u;

	/* CONFIGURE (0x55) wire = alp_cc3501e_spi1_configure_t { freq_hz(LE32) | mode
	 * | bits_per_word | cs | reserved }. */
	uint8_t p[sizeof(alp_cc3501e_spi1_configure_t)];
	memset(p, 0, sizeof(p));
	p[0] = (uint8_t)(freq_hz & 0xFFu);
	p[1] = (uint8_t)((freq_hz >> 8) & 0xFFu);
	p[2] = (uint8_t)((freq_hz >> 16) & 0xFFu);
	p[3] = (uint8_t)((freq_hz >> 24) & 0xFFu);
	p[4] = mode;
	p[5] = CC3501E_SPI1_BITS_PER_WORD;
	p[6] = (uint8_t)cs;

	/* Reply DATA = alp_cc3501e_spi1_config_resp_t { freq_hz(LE32) | max_xfer(LE16)
	 * | bits_per_word | reserved }. */
	uint8_t reply[sizeof(alp_cc3501e_spi1_config_resp_t)] = { 0 };
	size_t  got                                           = 0;

	alp_status_t s = poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_SPI1_CONFIGURE, p, sizeof(p), reply, sizeof(reply), &got, timeout_ms);
	if (s != ALP_OK) return s;
	if (got < sizeof(reply)) return ALP_ERR_IO; /* short reply -- firmware/wire gap */

	/* The divider rounds, so the ACTUAL clock is read back rather than assumed: a
	 * peripheral with a hard SCK ceiling is the caller's problem to respect, and
	 * it cannot respect what it was never told. */
	if (actual_freq_hz_out != NULL) {
		*actual_freq_hz_out = (uint32_t)reply[0] | ((uint32_t)reply[1] << 8) |
		                      ((uint32_t)reply[2] << 16) | ((uint32_t)reply[3] << 24);
	}
	/* The PEER's chunk size, not this header's constant: a host that chunks to
	 * what the firmware in front of it actually accepts survives a later firmware
	 * moving the cap without another version bump. */
	if (max_xfer_out != NULL) {
		*max_xfer_out = (uint16_t)((uint16_t)reply[4] | ((uint16_t)reply[5] << 8));
	}
	/* SESSION binding, not bus state -- see the field comment in core.h.  A
	 * successful CONFIGURE in THIS session is what cc3501e_spi1_transfer()
	 * gates on, so a fresh ctx is always forced through a real round trip
	 * (whose worker_poll() orphan-discards any stale cached TRANSFER result
	 * from a session the firmware outlived) before it can collide with one. */
	ctx->spi1_configured = true;
	return ALP_OK;
}

/* See <alp/chips/cc3501e/core.h>. */
alp_status_t cc3501e_spi1_transfer(cc3501e_t     *ctx,
                                   const uint8_t *tx,
                                   uint8_t       *rx,
                                   uint16_t       len,
                                   uint8_t        tx_fill,
                                   bool           cs_hold,
                                   uint32_t       timeout_ms)
{
	if (ctx == NULL) return ALP_ERR_INVAL;
	/* SESSION gate, checked locally and BEFORE anything touches the wire (see
	 * spi1_configured's comment in core.h).  The firmware's g_configured latch
	 * and cached (seq, result) are file statics that outlive an Alif reboot,
	 * so a freshly memset ctx's first TRANSFER is always seq 1 -- which, on a
	 * link the firmware never rebooted, is exactly the seq the LAST session
	 * also started from.  Without this gate that first TRANSFER can match the
	 * previous session's cached seq-1 DONE result and hand back its RX bytes
	 * as ALP_OK with the bus never re-clocked.  A real CONFIGURE in THIS
	 * session is what clears the trap -- its own round trip polls a DIFFERENT
	 * opcode, and worker_poll()'s orphan-discard arm drops the stale cache the
	 * moment it does. */
	if (!ctx->spi1_configured) return ALP_ERR_NOT_READY;
	/* Refuse rather than truncate.  A silently short SPI transfer is a corrupted
	 * device transaction, and the caller was handed the peer's real chunk size by
	 * cc3501e_spi1_configure() precisely so it can split the work itself. */
	if (len > ALP_CC3501E_SPI1_MAX_XFER) return ALP_ERR_INVAL;

	/* A NULL buffer means "drop this direction from the wire" -- the same
	 * convention the firmware's HAL seam and TI's SPI_Transaction already use, so
	 * the wire flags collapse into the pointers instead of a second argument.
	 * Each dropped direction removes up to 4 KB from a link where per-transaction
	 * latency is the whole cost -- boards without the input-enable pinctrl group
	 * on the READY pad have no working READY line at all (chips/cc3501e/
	 * cc3501e_sockets.c, silicon-measured 2026-08-24) and fall back to fixed
	 * settle gaps; even on a board with it, dropping a direction is still a
	 * whole phase not clocked: write-only traffic (flash page program) passes
	 * rx == NULL, read-only traffic (flash read, FIFO drain) passes tx == NULL
	 * plus the fill byte to clock. */
	uint8_t flags = (uint8_t)(cs_hold ? ALP_CC3501E_SPI1_XFER_CS_HOLD : 0u);
	if (tx == NULL) flags |= ALP_CC3501E_SPI1_XFER_NO_TX;
	if (rx == NULL) flags |= ALP_CC3501E_SPI1_XFER_NO_RX;

	/* Per-context staging, NOT a stack frame: a full chunk is 8 + 4088 bytes and
	 * the Zephyr shell thread is CONFIG_SHELL_STACK_SIZE=2048 -- exactly how the
	 * scan/event (#740) and socket paths took usage faults.  spi1_busy turns
	 * same-ctx reentrancy into an explicit BUSY instead of silent aliasing; it is
	 * not a lock for two genuinely concurrent callers, the transport lock inside
	 * cc3501e_request() is. */
	if (ctx->spi1_busy) return ALP_ERR_BUSY;
	ctx->spi1_busy = true;

	/* TRANSFER (0x56) wire = alp_cc3501e_spi1_transfer_t { len(LE16) | flags | seq
	 * | tx_fill | reserved(3) } + tx[len] inline, absent when NO_TX. */
	const size_t hdr = sizeof(alp_cc3501e_spi1_transfer_t);
	uint8_t     *req = ctx->spi1_tx_buf;
	memset(req, 0, hdr);
	req[0] = (uint8_t)(len & 0xFFu);
	req[1] = (uint8_t)((len >> 8) & 0xFFu);
	req[2] = flags;
	/* Free-running counter owned by the driver, not by the caller.  poll_by_repeat
	 * re-issues the IDENTICAL frame when the firmware answers BUSY (worker in
	 * flight) or the bridge reads IO (a radio op desynced the request phase), and
	 * the firmware answers a repeated seq from its cached result instead of
	 * re-clocking the bus.  On a bus that drives flash that is the difference
	 * between a retried read and a DOUBLE PAGE PROGRAM.  A monotonic counter can
	 * never collide with the single cached entry (always the immediately preceding
	 * value), which also satisfies "do not reuse a seq across a CONFIGURE" without
	 * mirroring configure state up here. */
	req[3] = ++ctx->spi1_seq;
	req[4] = tx_fill;

	/* Exact-length contract, which the firmware checks with != rather than <:
	 * NO_TX -> payload == hdr; otherwise payload == hdr + len. */
	size_t req_len = hdr;
	if (tx != NULL) {
		if (len > 0u) memcpy(&req[hdr], tx, len);
		req_len += len;
	}

	size_t       got = 0;
	alp_status_t s   = poll_by_repeat(ctx,
	                                  ALP_CC3501E_CMD_SPI1_TRANSFER,
	                                  req,
	                                  req_len,
	                                  ctx->spi1_rx_buf,
	                                  sizeof(ctx->spi1_rx_buf),
	                                  &got,
	                                  timeout_ms);
	if (s == ALP_OK) s = spi1_take_rx(ctx->spi1_rx_buf, got, req[3], rx, len);
	ctx->spi1_busy = false;
	return s;
}

/* See <alp/chips/cc3501e/core.h>. */
alp_status_t cc3501e_spi1_release(cc3501e_t *ctx, uint32_t timeout_ms)
{
	if (ctx == NULL) return ALP_ERR_INVAL;
	/* The escape hatch, so it carries no payload and has no preconditions: the
	 * firmware answers RESP_OK even with nothing open.  A host that lost track of
	 * a CS_HOLD chain (a timeout mid-chain, a restarted app) therefore always has
	 * one call that returns the bus to a known-free state. */
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_SPI1_RELEASE, NULL, 0, NULL, 0, NULL, timeout_ms);
}
