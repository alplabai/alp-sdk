/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * CC3501E OTA firmware update -- stream a new image over the bridge
 * (opcodes 0x40..0x44, 0x46, 0x47).  See <alp/chips/cc3501e/ota.h> for
 * the public API.
 */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "cc3501e_internal.h"
#include "../../src/common/alp_checked_arith.h"

/* An OTA op that touches flash tears the bridge DMA down -- the device re-opens
 * and re-arms it at the end of cc3501e_hw_ota_pump -- so the reply to the very
 * op that caused the blackout can be lost in transit.  BEGIN and FINISH are
 * idempotent on the device (a repeat BEGIN while WRITING returns OK, and STATUS
 * reports the resulting session state), so a lost reply is confirmed against
 * OTA_STATUS rather than reported to the caller as a failure.
 *
 * Bench 2026-08-21: ota_do_begin walks BOTH slots through
 * query+cancel+reject+clean (~2.3 s of flash work) before the bridge comes back.
 * The SAME build returned `begin -> 0` on one run and `begin -> -1` on the next
 * with no code change -- the only difference was whether the reply survived the
 * blackout.  Streaming then never started, which is why a healthy link (20 soak
 * PINGs, scan, BLE up) still produced a dead OTA. */
/* BEGIN is the longest blackout in the session: psa_fwu_cancel/reject/clean +
 * psa_fwu_start is a secondary-slot ERASE, measured at 22-41 s on silicon.
 * 20 s could not cover it and reported ALP_ERR_TIMEOUT on a healthy device.
 * As above, the budget is charged as an upper bound so the real floor is
 * roughly WAIT_MS/5. */
/* One ATTEMPT's worth of patience, not the whole erase.  The caller retries and
 * can report device state between attempts -- far more useful than one opaque
 * multi-minute block that prints nothing if it fails. */
#define CC3501E_OTA_BLACKOUT_WAIT_MS 60000u
/* Blind settle after a BEGIN that did not answer at once: the device is erasing
 * the target slot and CANNOT answer anything until it finishes -- it serves the
 * bridge from its SPI-slave ISR, and the flash op is what stops that ISR.
 *
 * SIZED FROM THE ACTUAL SLOT GEOMETRY, not guessed.  The generated flash map
 * (firmware/cc3501e/build/ti/memcfg/ti_flash_map_config.c) gives
 * vendor_image_slot_2_region_size = 0x002A2000 = 2,760,704 B, and the memory
 * configurator gives flash_sector_size = 4096 -- so ONE slot clean is 674 sector
 * erases on a PY25Q64LB.  At ~100-300 ms per sector that is ~67-200 s, and the
 * bench has measured a BEGIN still unfinished at 181 s.  A 120 s budget gave up
 * MID-ERASE, which leaves the slot dirty so the next BEGIN must erase all over
 * again -- the impatience never converges.  Size for the worst case; a clean
 * slot costs nothing because BEGIN then answers immediately and never gets
 * here (ota_do_begin only erases when psa_fwu_query says the slot is not
 * READY, mirroring TI's OTA_FWU_prepareSlot). */
#define CC3501E_OTA_BEGIN_BLIND_MS   180000u
#define CC3501E_OTA_BLACKOUT_POLL_MS 50u
/* Per-poll frame cap.  cc3501e_ota_status BLOCKS for whatever timeout it is
 * given, so charging only the sleep against the budget made a nominal 20 s wait
 * run for up to 400 * timeout_ms (~8000 s at a 20 s caller timeout) -- observed
 * as a BEGIN that never returned at all, silicon 2026-08-21. */
#define CC3501E_OTA_BLACKOUT_POLL_TIMEOUT_MS 200u

static alp_status_t ota_settled_as(cc3501e_t *ctx, uint8_t want, uint32_t timeout_ms);

alp_status_t cc3501e_ota_begin(cc3501e_t *ctx, uint32_t total_len, uint32_t timeout_ms)
{
	/* UPDATE MODE IS A PRECONDITION, not a nicety.  ota_do_begin runs
	 * psa_fwu_query and, on a dirty slot, a 674-sector erase -- and a
	 * callback/DMA SPI_open on the bridge PERMANENTLY prevents psa_fwu_start /
	 * psa_fwu_write from returning (bench-proven; SPI_close does not undo the
	 * claim).  So a BEGIN issued on the normal DMA bridge does not fail, it
	 * WEDGES the device, recoverable only by a WIFI_EN/nRESET cold cycle.
	 *
	 * cc3501e_ota_update and the bring-up example both enter update mode first,
	 * but this is a public entry point and `alp companion ota begin` called it
	 * straight from the shell -- a one-line command that bricked the link until a
	 * power cycle.  Refuse instead. */
	if (!cc3501e_peer_is_polled()) {
		return ALP_ERR_NOT_READY;
	}
	uint8_t req[4];
	req[0] = (uint8_t)(total_len & 0xFFu);
	req[1] = (uint8_t)((total_len >> 8) & 0xFFu);
	req[2] = (uint8_t)((total_len >> 16) & 0xFFu);
	req[3] = (uint8_t)((total_len >> 24) & 0xFFu);
	/* SEND ONCE, then GO SILENT.  poll_by_repeat re-clocked this payload-bearing
	 * frame every 50 ms for the whole BEGIN blackout -- the same anti-pattern the
	 * WRITE path was fixed for.  Worse, polling through a blackout CANNOT work:
	 * the device answers from its SPI-slave ISR, and the flash op is precisely
	 * what stops that ISR, so every frame clocked in that window is clocked into a
	 * dead slave.  Silicon 2026-08-21 proved it -- BEGIN and the STATUS read that
	 * chased it BOTH returned ALP_ERR_TIMEOUT (-4) after 81 s, with no device
	 * field readable.  There is no in-band way to observe the device mid-erase;
	 * the READY line is the only out-of-band signal and it is not HW-validated on
	 * this bench.  So: one frame, then a blind settle sized to a slot erase, and
	 * only then confirm against STATUS. */
	const alp_status_t s = cc3501e_request(
	    ctx, ALP_CC3501E_CMD_OTA_BEGIN, req, sizeof(req), NULL, 0, NULL, timeout_ms);
	/* An ALP_OK reply means the device QUEUED the op, NOT that the session is
	 * open: cc3501e_hw_ota_begin submits to the pump and answers immediately,
	 * while ota_do_begin (slot query + any prepare) runs afterwards.  Returning
	 * on that reply made the caller start WRITING while the device was still
	 * preparing, so the very first chunk landed in a blackout and burned the whole
	 * flush hold-off (silicon 2026-08-21: `OTA begin -> 0 (2 ms)` followed by ZERO
	 * progress lines).  ALWAYS confirm the session really reached WRITING; only
	 * blind-settle first when the reply itself was lost to a blackout. */
	/* ALP_ERR_BUSY is NOT a lost reply -- it is the device ANSWERING.
	 * cc3501e_hw_ota_begin() submits to the pump and ota_submit() returns
	 * CC3501E_HW_BUSY, which the protocol layer maps to RESP_ERR_BUSY.  So a
	 * healthy BEGIN on a CLEAN slot (ota_do_begin skips the erase when
	 * psa_fwu_query says READY -- ~2 ms) still arrives here as -3.  Treating
	 * that like a blackout cost a FLAT 180 s on EVERY OTA, measured identical
	 * to the millisecond across three runs (`OTA begin -> 0 (180035 ms)`),
	 * which is the host sleeping, not the device erasing.
	 *
	 * The device answered, so it is alive and pollable: go straight to the
	 * confirmation poll.  Only fall back to the blind settle if the poll does
	 * NOT settle -- that is the genuinely dirty slot, where the erase has since
	 * taken the device deaf and there is no in-band way to watch it. */
	if (s != ALP_OK && s != ALP_ERR_BUSY) {
		alp_delay_ms(CC3501E_OTA_BEGIN_BLIND_MS);
	}
	/* ota_settled_as() distinguishes "not there yet" (TIMEOUT) from "the device
	 * latched OTA ERROR" (IO).  Discarding that told the caller ALP_ERR_BUSY --
	 * "try again" -- for a session that had already failed, after first sleeping
	 * CC3501E_OTA_BEGIN_BLIND_MS and polling a device known to be dead.  A
	 * latched ERROR is terminal: report it. */
	alp_status_t settled = ota_settled_as(ctx, ALP_CC3501E_OTA_STATE_WRITING, timeout_ms);
	if (settled == ALP_OK) {
		return ALP_OK;
	}
	if (settled == ALP_ERR_IO) {
		return ALP_ERR_IO;
	}
	if (s == ALP_ERR_BUSY) {
		alp_delay_ms(CC3501E_OTA_BEGIN_BLIND_MS);
		settled = ota_settled_as(ctx, ALP_CC3501E_OTA_STATE_WRITING, timeout_ms);
		if (settled == ALP_OK) {
			return ALP_OK;
		}
		if (settled == ALP_ERR_IO) {
			return ALP_ERR_IO;
		}
	}
	return (s == ALP_OK) ? ALP_ERR_TIMEOUT : s;
}

alp_status_t cc3501e_ota_write(cc3501e_t     *ctx,
                               uint32_t       offset,
                               const uint8_t *data,
                               size_t         len,
                               uint32_t       timeout_ms)
{
	if (data == NULL || len == 0u || len > ALP_CC3501E_OTA_MAX_CHUNK) {
		return ALP_ERR_INVAL;
	}
	/* Frame = offset(LE32) followed by the raw image bytes (<= MAX_PAYLOAD). */
	uint8_t buf[4u + ALP_CC3501E_OTA_MAX_CHUNK];
	buf[0] = (uint8_t)(offset & 0xFFu);
	buf[1] = (uint8_t)((offset >> 8) & 0xFFu);
	buf[2] = (uint8_t)((offset >> 16) & 0xFFu);
	buf[3] = (uint8_t)((offset >> 24) & 0xFFu);
	memcpy(&buf[4], data, len);
	/* SEND ONCE -- deliberately NOT poll_by_repeat (#1610).
	 *
	 * poll_by_repeat re-sends this whole 4+len payload every CC3501E_POLL_GAP_MS
	 * until the device stops answering BUSY.  That was safe while WRITE never
	 * touched flash: the old firmware staged the entire image in RAM and did all
	 * its flashing at FINISH, so a WRITE could never coincide with a torn-down
	 * bridge DMA.
	 *
	 * With windowed staging a WRITE *can* land exactly when the device is flushing
	 * the window to flash, and clocking payload into a slave whose DMA is down is
	 * precisely what broke the 2026-06-19 per-chunk-flash attempt.  So this pushes
	 * the frame once and lets the CALLER hold off on BUSY by polling header-only
	 * OTA_STATUS until reserved[1] (flush_pending) clears -- see
	 * cc3501e_ota_update.  A caller driving raw chunks itself must do the
	 * same; BUSY here means "retry this same chunk later", not "failed". */
	return cc3501e_request(
	    ctx, ALP_CC3501E_CMD_OTA_WRITE, buf, 4u + len, NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ota_finish(cc3501e_t *ctx, uint32_t timeout_ms)
{
	const alp_status_t s =
	    poll_by_repeat(ctx, ALP_CC3501E_CMD_OTA_FINISH, NULL, 0, NULL, 0, NULL, timeout_ms);
	if (s == ALP_OK) return ALP_OK;
	/* Same discarded verdict that was fixed in cc3501e_ota_begin, left in its
	 * sibling.  A device that latched OTA ERROR (a flush wrote nothing) answers
	 * the next FINISH with RESP_ERR_INVALID, because its state is no longer
	 * WRITING -- so this returned ALP_ERR_INVAL, telling the caller "you called
	 * FINISH at the wrong time" for what was actually a failed flash write.
	 * ota_settled_as already computes the right answer; keep it. */
	const alp_status_t settled = ota_settled_as(ctx, ALP_CC3501E_OTA_STATE_STAGED, timeout_ms);
	if (settled == ALP_OK) {
		return ALP_OK;
	}
	if (settled == ALP_ERR_IO) {
		return ALP_ERR_IO;
	}
	return s;
}

alp_status_t cc3501e_ota_abort(cc3501e_t *ctx, uint32_t timeout_ms)
{
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_OTA_ABORT, NULL, 0, NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ota_promote(cc3501e_t *ctx, uint32_t timeout_ms)
{
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_OTA_PROMOTE, NULL, 0, NULL, 0, NULL, timeout_ms);
}

/* ---- OTA update mode (0x47) --------------------------------------------
 *
 * WHY this exists at all (silicon 2026-08-21, bisected on the bench): a
 * SPI_MODE_CALLBACK (DMA) SPI_open() on the device PERMANENTLY prevents
 * psa_fwu_start() and psa_fwu_write() from returning.  psa_fwu_start before
 * transport_spi_init() returns; after a POLLED (SPI_MODE_BLOCKING) SPI_open the
 * whole sequence still returns; after a callback/DMA SPI_open psa_fwu_start
 * NEVER returns and the device is gone until a WIFI_EN/nRESET.  SPI_close()
 * does not undo the claim, and SPI_transferCancel() hung the bridge twice.  So
 * an OTA can only run on a boot whose bridge was opened POLLED -- which is what
 * this opcode arms: the device persists a flag, warm-reboots, and comes back
 * running nothing but "service one polled frame, then pump the OTA flush".
 *
 * The device leaves update mode by ITSELF after a successful FINISH (the swap
 * reboot must land in the normal DMA bridge or the freshly-swapped firmware
 * comes up deaf to the radio), so the enable=false direction exists only for a
 * caller that wants to back out of a session it never finished. */

/* Blind settle across the warm reboot: CLOCK NOTHING here.  On this CS-less
 * 3-wire link there is no chip-select to recover framing on, so a byte clocked
 * before the slave has armed is a PERMANENT 1-byte phase offset on the link
 * (cc3501e_core.c's desync note), not merely a dropped frame.  Same budget
 * cc3501e_hard_reset uses for a re-boot; do NOT shorten it on the strength of
 * the firmware's hardware-SS0 comments -- the CS-less rule is the strictly
 * stronger constraint and this code must hold on the units that lack the SS0
 * bodge. */
#define CC3501E_UPDATE_MODE_SETTLE_MS 3500u
#define CC3501E_UPDATE_MODE_POLL_MS   250u
/* Per-poll frame cap, charged against the caller's budget TOGETHER with the
 * sleep.  Note what does NOT make a readback expensive: cc3501e_request()
 * IGNORES its timeout_ms outright ("(void)timeout_ms; -- reserved for a future
 * IRQ-driven wait", cc3501e_core.c), unlike poll_by_repeat() which really does
 * re-issue for the whole budget.  What costs time is the READY gate: once
 * g_ready_line_proven latches, EACH reply phase may wait
 * CC3501E_READY_WAIT_US = 250000 us, so one 4-phase 0x47 readback can burn ~1 s
 * of wall time on a bodged unit.  Charging only the sleep is exactly what turned
 * a nominal 20 s BEGIN wait into ~8000 s (silicon 2026-08-21), so this cap is
 * charged whether or not the frame really blocked -- an UPPER bound, as
 * CC3501E_OTA_BLACKOUT_POLL_TIMEOUT_MS above is.
 *
 * CONSEQUENCE FOR CALLERS: the real confirm window is about
 * timeout_ms * POLL_MS / (POLL_MS + this) ~= timeout_ms/6 -- at the bench's
 * CC3501E_OTA_DEMO_TIMEOUT_MS 20000 that is ~3.4 s of polling after the 3.5 s
 * settle, which covers a warm reboot with margin.  cc3501e_ota_update_mode's
 * timeout_ms is therefore a WHOLE-OPERATION budget, not the per-frame budget the
 * other cc3501e_ota_* entry points take; a caller that passes a per-frame value
 * (100-200 ms) gets one or two polls and little else. */
#define CC3501E_UPDATE_MODE_POLL_TIMEOUT_MS 1200u

/* One 0x47 round trip; true ONLY when the device read back `want` as the mode it
 * is running RIGHT NOW.  That readback -- not the ack -- is the whole confirm
 * signal: cc3501e_hw_reset_cause() returns ALP_CC3501E_RESET_UNKNOWN
 * unconditionally (hal/ti/cc3501e_hw_ti_log.c), so GET_DIAG_INFO's reset_cause
 * byte is a hardcoded 0 and cannot tell a soft reboot from anything at all
 * (uptime_ms is real and may corroborate, but must never be the gate).
 *
 * The mode byte is also what defeats the #1378 dead-phase alias: a payload phase
 * that dies clocks literal 0x00 for every byte and 0x00 is ALSO RESP_OK, so a
 * bare-OK reply is indistinguishable from a dead link.  That defence only works
 * in the want=1 direction (0 != 1) -- which is the direction that matters, since
 * this opcode's whole job is to be the last frame before a blackout. */
static bool update_mode_reads_as(cc3501e_t *ctx, uint8_t want, uint32_t timeout_ms)
{
	uint8_t            reply[4] = { 0 };
	size_t             got      = 0u;
	const alp_status_t s        = cc3501e_request(
	    ctx, ALP_CC3501E_CMD_OTA_UPDATE_MODE, &want, 1u, reply, sizeof(reply), &got, timeout_ms);
	if (s != ALP_OK || got < 1u || reply[0] != want) {
		return false;
	}
	if (want != 0u) {
		return true; /* 0x00 cannot forge a 1 -- the readback IS the proof. */
	}
	/* want == 0 is the direction the mode byte CANNOT defend (see the comment
	 * above): a dead payload phase clocks literal 0x00, which is byte-identical
	 * to a genuine "normal bridge, OTA idle" reply, so reply[0] == 0 alone is
	 * the absence of evidence, not evidence.  This function is what the public
	 * ALP_OK of cc3501e_ota_update_mode(ctx, false, ...) rests on, and both
	 * include/alp/protocol/cc3501e.h and docs/cc3501e-bridge.md instruct hosts
	 * to corroborate here -- so corroborate: require a diag reply carrying a
	 * NON-ZERO uptime_ms.  A dead phase reads uptime_ms as 0; a live device
	 * that has run far enough to service this frame never does.  reset_cause is
	 * deliberately NOT used: the HAL hardcodes it to 0. */
	alp_cc3501e_diag_info_t info = { 0 };
	return cc3501e_diag_info(ctx, &info) == ALP_OK && info.uptime_ms != 0u;
}

alp_status_t cc3501e_ota_update_mode(cc3501e_t *ctx, bool enable, uint32_t timeout_ms)
{
	/* Guarded HERE rather than left to cc3501e_request (which answers
	 * ALP_ERR_NOT_READY for both conditions) because this function ACTS on a
	 * failed readback: it would blind-settle 3.5 s and then run
	 * cc3501e_hard_reset before reporting a misleading ALP_ERR_TIMEOUT.  Not a
	 * hypothetical caller bug -- cc3501e_reset() CLEARS ctx->initialised when the
	 * firmware's GET_VERSION disagrees with ALP_CC3501E_PROTOCOL_VERSION
	 * (cc3501e_core.c), which is precisely the state a bench unit still running v4
	 * firmware is in when the operator reaches for OTA to fix it.  That operator
	 * must read NOT_READY, not a stalled TIMEOUT. */
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;

	const uint8_t want = enable ? 1u : 0u;

	/* SEND ONCE, then go silent -- the same rule (and the same reason) as
	 * cc3501e_ota_begin: re-clocking a payload-bearing frame at a device that is
	 * rebooting cannot work, because the thing that would answer is the thing
	 * that is down.  Losing this ack to the reboot it triggers is the EXPECTED
	 * outcome, not a failure, so the send's status is not reported.
	 *
	 * IDEMPOTENT BY CONTRACT: the firmware replies with the mode it is running
	 * right now and reboots only when that differs from the request.  A reply
	 * that already reads back `want` therefore proves no reboot was armed --
	 * return at once instead of burning the settle on a device that never left.
	 * (It is also why the confirm loop below may re-issue the same opcode.) */
	if (update_mode_reads_as(ctx, want, timeout_ms)) {
		cc3501e_set_peer_polled(enable); /* polled slave -> edge-gate READY */
		return ALP_OK;
	}

	alp_delay_ms(CC3501E_UPDATE_MODE_SETTLE_MS);

	/* Poll THROUGH failed reads -- a failing read IS the blackout, the same shape
	 * ota_settled_as() polls through.  Re-issuing is also what recovers a FIRST
	 * request the device never received: the mode still differs, so the device
	 * arms its reboot then and the next readback confirms it.  Every re-issue
	 * starts with a header phase, so a desync left by an earlier frame self-heals
	 * on the next clean transaction (cc3501e_core.c reports IO and re-aligns
	 * rather than byte-walking). */
	const uint32_t poll_ms = (timeout_ms < CC3501E_UPDATE_MODE_POLL_TIMEOUT_MS)
	                             ? timeout_ms
	                             : CC3501E_UPDATE_MODE_POLL_TIMEOUT_MS;
	uint32_t       waited  = 0u;
	for (;;) {
		if (update_mode_reads_as(ctx, want, poll_ms)) {
			cc3501e_set_peer_polled(enable);
			return ALP_OK;
		}
		if (waited >= timeout_ms) break;
		alp_delay_ms(CC3501E_UPDATE_MODE_POLL_MS);
		waited += CC3501E_UPDATE_MODE_POLL_MS + poll_ms;
	}

	/* Never came back in the requested mode.  Recover with the WARM reset, NOT
	 * cc3501e_reset: its cold cycle re-triggers the Puya double-boot bug and can
	 * leave ctx NOT_READY (see cc3501e_hard_reset).  Its status is ignored -- a
	 * unit with no reset pin answers ALP_ERR_NOSUPPORT and the verdict reported
	 * here is the timeout either way.
	 *
	 * Clear the polled flag FIRST: the reset always lands the device in NORMAL
	 * mode, so leaving it set would keep the host edge-gating READY against a
	 * level-driving peer -- up to CC3501E_READY_EDGE_US of extra wait on every
	 * phase, for the rest of the session. */
	cc3501e_set_peer_polled(false);
	(void)cc3501e_hard_reset(ctx);
	return ALP_ERR_TIMEOUT;
}

/* Bail out of cc3501e_ota_update AFTER update mode was entered but BEFORE FINISH
 * was issued, reporting @p s.
 *
 * Update mode is a radio-dead boot mode: nothing else runs, so the worker never
 * drains and every Wi-Fi/BLE/GET_MAC command queues forever answering BUSY.
 * Returning an error while leaving the device parked there makes the NEXT thing
 * the application does -- a scan, a connect, the bringup soak -- fail for a
 * reason that has nothing to do with what it is doing, which is exactly the kind
 * of phantom this bench has already lost days to.  So every pre-FINISH failure
 * exit takes the device back out.
 *
 * Best-effort by construction, and never WORSE than not trying: the status is
 * discarded, and if the link is genuinely dead the enable=false readback hits the
 * #1378 all-zero alias (a dead phase reads mode 0, which IS the mode being asked
 * for) and returns OK having done nothing.  When the device is merely unhappy --
 * a rejected image, a cursor mismatch, a slot that would not erase -- it is alive
 * and answering, and this really does return it to the normal DMA bridge.  When
 * it is unreachable, cc3501e_ota_update_mode's own exhaustion path runs
 * cc3501e_hard_reset, and a reset ALWAYS lands in normal mode (the flag is RAM
 * only and read-and-cleared at boot).
 *
 * Deliberately NOT used on the FINISH path.  A FINISH that acked arms the
 * deferred swap reboot and the device leaves update mode by itself; a FINISH that
 * merely failed to CONFIRM may still have armed it, and firing 0x47 (then
 * possibly cc3501e_hard_reset) at a device mid slot-swap risks interrupting the
 * swap.  Leave a post-FINISH device alone. */
static alp_status_t ota_update_bail(cc3501e_t *ctx, alp_status_t s, uint32_t timeout_ms)
{
	(void)cc3501e_ota_update_mode(ctx, false, timeout_ms);
	return s;
}

alp_status_t cc3501e_ota_status(cc3501e_t *ctx, alp_cc3501e_ota_status_t *out, uint32_t timeout_ms)
{
	if (out == NULL) return ALP_ERR_INVAL;
	uint8_t      reply[12] = { 0 };
	size_t       got       = 0;
	alp_status_t s         = poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_OTA_STATUS, NULL, 0, reply, sizeof(reply), &got, timeout_ms);
	if (s != ALP_OK) return s;
	if (got < sizeof(reply)) return ALP_ERR_IO;
	out->state         = reply[0];
	out->reserved[0]   = reply[1];
	out->reserved[1]   = reply[2];
	out->reserved[2]   = reply[3];
	out->bytes_written = (uint32_t)reply[4] | ((uint32_t)reply[5] << 8) |
	                     ((uint32_t)reply[6] << 16) | ((uint32_t)reply[7] << 24);
	out->total_len = (uint32_t)reply[8] | ((uint32_t)reply[9] << 8) | ((uint32_t)reply[10] << 16) |
	                 ((uint32_t)reply[11] << 24);
	return ALP_OK;
}

/* Poll OTA_STATUS until the session settles in `want`.  Returns ALP_OK on
 * settle, ALP_ERR_IO if the device latched ERROR, ALP_ERR_TIMEOUT otherwise.
 * A STATUS read that itself fails is expected here -- that IS the blackout --
 * so keep polling until the deadline rather than bailing on the first error. */
static alp_status_t ota_settled_as(cc3501e_t *ctx, uint8_t want, uint32_t timeout_ms)
{
	const uint32_t poll_ms = (timeout_ms < CC3501E_OTA_BLACKOUT_POLL_TIMEOUT_MS)
	                             ? timeout_ms
	                             : CC3501E_OTA_BLACKOUT_POLL_TIMEOUT_MS;
	uint32_t       waited  = 0u;
	for (;;) {
		alp_cc3501e_ota_status_t st = { 0 };
		if (cc3501e_ota_status(ctx, &st, poll_ms) == ALP_OK) {
			if (st.state == want) return ALP_OK;
			if (st.state == ALP_CC3501E_OTA_STATE_ERROR) return ALP_ERR_IO;
		}
		if (waited >= CC3501E_OTA_BLACKOUT_WAIT_MS) return ALP_ERR_TIMEOUT;
		alp_delay_ms(CC3501E_OTA_BLACKOUT_POLL_MS);
		/* Charge the poll frame too -- the budget must cover time spent BLOCKED
		 * in the STATUS read, not just the sleep between reads. */
		waited += CC3501E_OTA_BLACKOUT_POLL_MS + poll_ms;
	}
}

/* Hold-off budget while the device flushes its staging window to flash (#1610).
 * A flush is at most CC3501E_OTA_WINDOW/4096 psa_fwu_write calls plus bridge
 * re-arms; 10 s is far beyond that and still finite, so a device that never
 * clears flush_pending fails the stream instead of hanging it.  Polled at the
 * same 50 ms cadence poll_by_repeat uses, but with HEADER-ONLY frames. */
/* 10 s covered a plain 4-block flush but NOT the first one, which also runs
 * psa_fwu_start (secondary-slot prepare/erase).  Silicon 2026-08-21: still
 * flushing after 60 s, device healthy and answering STATUS throughout. */
/* NOTE the budget is charged as an UPPER BOUND (sleep + the full per-poll frame,
 * even when the poll returns immediately), so the REAL floor is roughly
 * WAIT_MS * POLL_MS / (POLL_MS + POLL_TIMEOUT_MS) ~= WAIT_MS/5.  Sized here so
 * that floor still comfortably covers the first flush (psa_fwu_start + slot
 * prepare).  The chips layer has no portable monotonic clock, hence the model
 * rather than a timestamp; the bench example uses k_uptime_get directly. */
#define CC3501E_OTA_FLUSH_WAIT_MS 600000u
#define CC3501E_OTA_FLUSH_POLL_MS 50u

/* Consecutive hold-offs allowed that move the device cursor NOWHERE.  A flush
 * window ends with the device taking the retried chunk, so a hold-off that
 * clears (flush_pending == 0, state still WRITING) and still leaves
 * bytes_written short is not a flush at all -- it is a link carrying
 * header-only frames while dropping every payload one, which is exactly the
 * shape of a desync.  Nothing on that path sleeps, so without this bound the
 * retry spins at full speed forever and the caller hangs instead of failing. */
#define CC3501E_OTA_STALL_MAX 8u

alp_status_t
cc3501e_ota_update(cc3501e_t *ctx, const uint8_t *image, size_t len, uint32_t timeout_ms)
{
	if (image == NULL || len == 0u) return ALP_ERR_INVAL;

	/* OTA_BEGIN's total_len is a wire LE32 (<alp/protocol/cc3501e.h>), so the
	 * wire width is the only bound the HOST can know -- it is NOT the real
	 * image maximum.  The device enforces its own at BEGIN
	 * (CC3501E_OTA_IMAGE_MAX, firmware/cc3501e/hal/ti/cc3501e_hw_ti_ota.c),
	 * rejecting an oversize BEGIN with ERR_INVAL before any image data is
	 * streamed.  That value is HAL-private and unpublished on the wire, and it
	 * MOVES -- it was the 64 KiB whole-image RAM buffer until #1610 replaced
	 * that with a sliding window, after which it became a sanity bound with the
	 * real ceiling being the vendor slot.  So the host deliberately does NOT
	 * duplicate it: a hardcoded copy here would have started falsely rejecting
	 * valid images the day the staging changed.  Enforce only what the wire
	 * itself constrains, and leave the real limit to the device that owns it.
	 *
	 * Reject anything that would not round-trip BEFORE issuing BEGIN,
	 * converting len to the wire width exactly ONCE (#732): every offset
	 * streamed by the loop below is < len, so it is already proven to fit and
	 * the per-chunk (uint32_t)off cast needs no re-validation. */
	uint32_t total_len_u32;
	if (!alp_size_to_u32(len, &total_len_u32)) {
		return ALP_ERR_INVAL;
	}

	/* Enter OTA update mode FIRST -- before BEGIN, never mid-session.  The OTA
	 * session is RAM-only, so entering it later throws away the write cursor and
	 * forces a full re-BEGIN, i.e. another whole-slot erase (0x002A2000 =
	 * 2,760,704 B = 674 sector erases, 22-181 s measured).  A hard failure here
	 * is fatal on purpose: continuing in DMA/callback mode means psa_fwu_start
	 * never returns and the device disappears mid-update. */
	alp_status_t s = cc3501e_ota_update_mode(ctx, true, timeout_ms);
	if (s != ALP_OK) return s;

	s = cc3501e_ota_begin(ctx, total_len_u32, timeout_ms);
	if (s != ALP_OK) return ota_update_bail(ctx, s, timeout_ms);

	/* 256 B = the CC35 flash page / psa_fwu_write granularity (the validated
	 * SELFTEST installer used CC3501E_OTA_WRITE_CHUNK 256).  Non-page-sized
	 * chunks make the device psa_fwu_write fail -> the host loops on IO until the
	 * per-frame timeout (silicon 2026-06-19).  Keep host chunks page-aligned.
	 * (The final remainder chunk is < 256 B; psa_fwu accepts the partial tail,
	 * as the selftest's last write did.) */
	const size_t chunk = 256u;
	uint32_t     stall = 0u; /* consecutive hold-offs with no cursor movement */
	for (size_t off = 0u; off < len;) {
		size_t n = len - off;
		if (n > chunk) {
			n = chunk;
		}
		s = cc3501e_ota_write(ctx, (uint32_t)off, image + off, n, timeout_ms);
		if (s == ALP_ERR_BUSY || s == ALP_ERR_IO) {
			/* The device queued a window flush and did NOT consume this chunk
			 * (#1610).  Hold off ALL payload while its DMA is torn down and poll
			 * header-only until reserved[1] (flush_pending) clears, then retry the
			 * SAME chunk.  Re-sending payload across the blackout instead is the
			 * 2026-06-19 failure mode this whole design exists to avoid.
			 *
			 * Bounded so a device that never clears the flag cannot hang the
			 * stream: OTA_FLUSH_WAIT_MS is generous against a ~4-block flush but
			 * finite. `off` is NOT advanced, so the retry is a plain re-send. */
			/* Cap the poll FRAME, and charge it.  cc3501e_ota_status is
			 * poll_by_repeat, which really does re-issue for its whole budget --
			 * and during a flush the device is inside psa_fwu_write and answers
			 * nothing, so each iteration burned the caller's full timeout_ms (20 s
			 * at the bench) while `waited` counted only the 50 ms sleep.  Exit then
			 * needed 600000/50 = 12000 iterations, i.e. this "bounded" hold-off
			 * could block for ~66 HOURS.  Same accounting ota_settled_as and
			 * cc3501e_ota_update_mode already do, and what the comment on
			 * CC3501E_OTA_FLUSH_WAIT_MS already claims happens here. */
			const uint32_t fpoll_ms = (timeout_ms < CC3501E_OTA_BLACKOUT_POLL_TIMEOUT_MS)
			                              ? timeout_ms
			                              : CC3501E_OTA_BLACKOUT_POLL_TIMEOUT_MS;
			uint32_t       waited   = 0u;
			bool           landed   = false;
			for (;;) {
				alp_cc3501e_ota_status_t fs;
				/* The STATUS read itself can fail here -- while the slave re-arms
				 * its SPI the link is DOWN, so header-only polls return IO too.
				 * That is the expected shape of a flush window, not an error:
				 * keep polling until the device answers again. */
				if (cc3501e_ota_status(ctx, &fs, fpoll_ms) == ALP_OK && fs.reserved[1] == 0u) {
					/* A device that has latched ERROR (or dropped out of the
					 * session entirely) will never take another chunk, so
					 * breaking out to re-send would spin this loop forever --
					 * nothing on the retry path sleeps.  Treat anything other
					 * than WRITING as fatal, exactly as the lost-reply path at
					 * the bottom of this function already does. */
					if (fs.state != ALP_CC3501E_OTA_STATE_WRITING) {
						(void)cc3501e_ota_abort(ctx, timeout_ms);
						return ota_update_bail(ctx, ALP_ERR_IO, timeout_ms);
					}
					/* Device is back. Did this chunk actually land before the
					 * blackout swallowed its reply?  The cursor is authoritative:
					 * WRITE is NOT idempotent for an already-passed offset, so
					 * re-sending one the device already took is rejected as
					 * out-of-order -- which is exactly how a lost reply used to
					 * abort a perfectly healthy session. */
					if (fs.bytes_written >= (uint32_t)(off + n)) landed = true;
					break;
				}
				if (waited >= CC3501E_OTA_FLUSH_WAIT_MS) {
					(void)cc3501e_ota_abort(ctx, timeout_ms);
					return ota_update_bail(ctx, ALP_ERR_TIMEOUT, timeout_ms);
				}
				alp_delay_ms(CC3501E_OTA_FLUSH_POLL_MS);
				/* Charge the frame too, not just the sleep. */
				waited += CC3501E_OTA_FLUSH_POLL_MS + fpoll_ms;
			}
			if (landed) {
				off += n; /* reply was lost, bytes are in -- move on */
				stall = 0u;
			} else if (++stall >= CC3501E_OTA_STALL_MAX) {
				(void)cc3501e_ota_abort(ctx, timeout_ms);
				return ota_update_bail(ctx, ALP_ERR_IO, timeout_ms);
			}
			continue; /* else retry the SAME chunk */
		}
		if (s != ALP_OK) {
			/* A lost reply can leave the host unsure whether the chunk landed.
			 * OTA_WRITE is NOT idempotent (a re-sent already-written offset is
			 * rejected as out-of-order), so re-sync to the device's actual write
			 * cursor before deciding: if it already advanced past this chunk the
			 * write took -- continue; otherwise abort + report.  off and n are
			 * both already proven <= total_len_u32 (the BEGIN bound above), so
			 * the narrowing + addition below is done through the checked
			 * helpers rather than a raw `(uint32_t)(off + n)` cast (#732). */
			alp_cc3501e_ota_status_t st;
			uint32_t                 off_u32, n_u32, expect_u32;
			if (cc3501e_ota_status(ctx, &st, timeout_ms) == ALP_OK &&
			    st.state == ALP_CC3501E_OTA_STATE_WRITING && alp_size_to_u32(off, &off_u32) &&
			    alp_size_to_u32(n, &n_u32) && alp_u32_add_checked(off_u32, n_u32, &expect_u32) &&
			    st.bytes_written == expect_u32) {
				/* chunk landed; the reply was lost -- proceed. */
			} else {
				(void)cc3501e_ota_abort(ctx, timeout_ms);
				return ota_update_bail(ctx, s, timeout_ms);
			}
		}
		off += n;
		stall = 0u;
	}

	/* The ONE exit that did not go through ota_update_bail.  A FINISH that fails
	 * because the device latched ERROR staged nothing and armed no swap -- yet it
	 * left the CC3501E parked in the radio-dead polled boot, where Wi-Fi, BLE and
	 * GET_MAC queue forever and answer BUSY forever.  Every later scan/connect in
	 * the application then failed for an unrelated reason.  Take the device back
	 * out on that path, exactly as the bring-up example already does. */
	const alp_status_t fs = cc3501e_ota_finish(ctx, timeout_ms);
	if (fs == ALP_OK) {
		return ALP_OK; /* a successful FINISH leaves update mode by itself */
	}
	return ota_update_bail(ctx, fs, timeout_ms);
}
