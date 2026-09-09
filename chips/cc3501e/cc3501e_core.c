/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif-side host driver for the on-module TI CC3501E Wi-Fi 6 +
 * BLE 5.4 coprocessor.  See <alp/chips/cc3501e.h> for the public
 * lifecycle and <alp/protocol/cc3501e.h> for the wire protocol.
 *
 * Core module: lifecycle (init / reset / hard_reset / sync / get_version /
 * stream_write), the request/reply framing primitive (cc3501e_request), and
 * poll_by_repeat -- the retry wrapper every other cc3501e_<subsystem>.c
 * module builds its worker-routed helpers on (declared in
 * cc3501e_internal.h).
 *
 * Ships the call shape (init / reset / get_version / request /
 * set_event_callback / deinit) and the framing logic.  The actual
 * reset-pin pulse + WIFI.EN sequencing arrives once the EVK overlay
 * declares the Alif's P15_5 / P15_1_FLEX as GPIOs reachable via
 * alp_gpio_*; until then reset() returns NOSUPPORT cleanly.
 *
 * Wire framing matches the embedded firmware
 * (cc3501e-bridge-firmware:hal/ti/transport_hw_ti_spi.c): the current E1M-AEN
 * rev wires only SCLK/MOSI/MISO (no CS, no host IRQ -- both arrive next
 * rev), so a request/reply is clocked as four deterministic fixed-count
 * transfers in lockstep (request header, request payload, reply header,
 * reply payload) with a settle gap before the reply read.  The reply
 * payload's first byte is the response status (mapped via
 * resp_to_status); the data follows.
 */

#include <string.h>
#include <stdint.h>

#include "alp/peripheral.h"
#include "alp/protocol/crc16.h"
#include "cc3501e_internal.h"

static void
encode_header(uint8_t *frame, alp_cc3501e_cmd_t cmd, uint8_t flags, uint16_t payload_len)
{
	frame[0] = (uint8_t)cmd;
	frame[1] = flags;
	frame[2] = (uint8_t)(payload_len & 0xFF);
	frame[3] = (uint8_t)((payload_len >> 8) & 0xFF);
}

static uint16_t decode_header_payload_len(const uint8_t *frame)
{
	return (uint16_t)frame[2] | ((uint16_t)frame[3] << 8);
}

alp_status_t cc3501e_init(cc3501e_t *ctx, alp_spi_t *bus)
{
	if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
	memset(ctx, 0, sizeof(*ctx));
	ctx->bus         = bus;
	ctx->initialised = true;
	return ALP_OK;
}

/* See <alp/chips/cc3501e/core.h>. */
alp_status_t cc3501e_recover(cc3501e_t *ctx)
{
	if (ctx == NULL) return ALP_ERR_INVAL;

	/* A WARM reset -- nRESET only, rails up -- is what clears this state.
	 * Bench-established on e1m-aen-evk-01 (#1691): every observed wedge recovered
	 * with `warm-reset -> 0  PING -> 0`, and the .TI.noinit snapshot read back
	 * afterwards showed the firmware had been perfectly healthy the whole time --
	 * housekeeping ticks advancing, slave armed in PH_REQ_HEADER, READY HIGH, and
	 * g_resync_count / g_arm_fail_count both zero.
	 *
	 * That is why no firmware self-heal fires: bridge_transport_spi_is_dead() only
	 * reports a failed SPI_open, and the stall watchdog deliberately watches only
	 * REPLY phases because PH_REQ_HEADER legitimately waits forever for the host.
	 * The slave cannot tell "host idle" from "host clocking, I am not receiving".
	 * The HOST can, which is why recovery lives here and not in the firmware. */
	const alp_status_t rs = cc3501e_hard_reset(ctx);

	if (rs != ALP_OK) {
		/* nRESET alone did not take -- fall back to cutting the supply, which is
		 * the heavier hammer and loses all device state. */
		(void)cc3501e_power_off(ctx);
		return cc3501e_reset(ctx);
	}
	return cc3501e_ping(ctx);
}

/* See <alp/chips/cc3501e/core.h>. */
alp_status_t cc3501e_power_off(cc3501e_t *ctx)
{
	if (ctx == NULL) return ALP_ERR_INVAL;
	if (ctx->enable_pin == NULL) {
		/* Board ties WIFI_EN on -- the supply cannot be gated from software here,
		 * so say so rather than pretending the rail went down. */
		return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
	}

	/* nRESET low BEFORE the supply so the CC3501E is held in reset while VPA
	 * collapses, never left floating against a decaying rail. */
	(void)alp_gpio_write(ctx->reset_pin, false);
	(void)alp_gpio_write(ctx->enable_pin, false);

	/* Every later call on this ctx now fails ALP_ERR_NOT_READY instead of
	 * clocking frames at an unpowered slave and burning a full timeout each.
	 * cc3501e_reset() is what brings it back -- it re-runs the cold-boot
	 * sequence and re-arms this flag. */
	ctx->initialised = false;
	return ALP_OK;
}

/* v4.0 (#2035): pure gate decision -- is @p fw_major a MAJOR this bilingual
 * host will talk to?  Exactly ALP_CC3501E_PROTOCOL_MAJOR (4, this driver's
 * own wire) and ALP_CC3501E_PROTOCOL_MAJOR_LEGACY (3, the migration-window
 * predecessor -- see the paragraph above ALP_CC3501E_PROTOCOL_MAJOR in
 * <alp/protocol/cc3501e.h>).  Everything else is refused, INCLUDING 0 (the
 * pre-ADR-0033 raw-integer legacy this scheme was built to distinguish from a
 * genuine disagreement -- see ALP_CC3501E_PROTOCOL_VERSION's doc comment).
 * Pulled out of cc3501e_reset() as its own function, with no ctx and no I/O,
 * so it is testable directly (cc3501e_internal.h) without a live link. */
bool cc3501e_fw_major_is_acceptable(uint8_t fw_major)
{
	return fw_major == (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR ||
	       fw_major == (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR_LEGACY;
}

alp_status_t cc3501e_reset(cc3501e_t *ctx)
{
	/* RECOVERY PRIMITIVE: deliberately does NOT require ctx->initialised.
	 *
	 * This is the only way back from a context marked down, and two paths mark one
	 * down: cc3501e_power_off(), which gates the supply, and the GET_VERSION compat
	 * gate below, which clears the flag on a mismatch.  Gating reset on that same
	 * flag made BOTH states unrecoverable through the API -- bench-proven,
	 * `POWER OFF -> 0 ... reset -> -2`.  The pin checks below are the real
	 * precondition; a never-initialised context has NULL pins and still gets a
	 * clean ALP_ERR_NOSUPPORT. */
	if (ctx == NULL || ctx->bus == NULL) return ALP_ERR_NOT_READY;
	if (ctx->reset_pin == NULL || ctx->enable_pin == NULL) {
		/* The studio's pin allocator (or hand-written firmware
         * via alp_gpio_open) must populate enable_pin / reset_pin
         * before reset() is meaningful.  Until then there's no
         * line to pulse. */
		return ALP_ERR_NOSUPPORT;
	}
	/* Reset sequence per TI SWRU626 §7.1.5 (CC3501E technical
     * reference manual):
     *
     *   1. Assert nRESET low while bringing rails down so the
     *      chip stays clamped through the supply transition.
     *   2. Drop WIFI_EN low; wait briefly for the rails to
     *      discharge (10us is comfortably above the rail RC).
     *   3. Raise WIFI_EN; wait ~5 ms for the supply ramps to
     *      stabilise (typical PMIC soft-start window).
     *   4. Hold nRESET low for >= 10 us per §7.1.5 after the
     *      supplies are valid.
     *   5. Release nRESET; wait the T1+T2+T3+T4 boot budget
     *      (~900 ms typical for BL1 + BL2 + Chain-of-Trust)
     *      before the first PING is meaningful.
     *
     * Total blocking time: ~905 ms.  Callers that don't want
     * the synchronous wait can call cc3501e_reset asynchronously
     * (kicked off from a worker thread) and poll via PING; v0.3.x
     * adds a non-blocking variant once the firmware's "boot done"
     * GPIO is wired. */
	(void)alp_gpio_write(ctx->reset_pin, false);
	(void)alp_gpio_write(ctx->enable_pin, false);
	/* COLD-BOOT POWER SEQUENCE (2026-06-17): generous, cold-safe timings.
	 * The CC3501E's secure boot (BL1->BL2->vendor image) runs ONLY on a true
	 * cold power-on; on this E1M-AEN board VPA(3.3V) is gated by WIFI_EN via the
	 * U1 load switch and the HFXT(52 MHz) crystal must be stable before/through
	 * the SES launch.  The earlier 10us discharge / 5ms ramp were too aggressive
	 * for a clean cold POR (warm reset hid it), so widen every window:
	 *   - 50 ms discharge so the rails fully collapse => the CC35 sees a real POR
	 *     (not a brown-out that skips Chain-of-Trust re-init), and
	 *   - 100 ms after WIFI_EN so VPA + the crystal are fully settled before
	 *     nRESET is released (TI SWRU626 §2.2.2.1: all supplies valid before
	 *     nRESET), with a 1 ms asserted-low hold, and
	 *   - 1500 ms boot budget before the first PING. */
	alp_delay_ms(50u);
	(void)alp_gpio_write(ctx->enable_pin, true);
	alp_delay_ms(100u);
	/* nRESET stays low through the rail ramp; this assignment is
     * idempotent but kept explicit for clarity. */
	(void)alp_gpio_write(ctx->reset_pin, false);
	alp_delay_ms(1u);
	(void)alp_gpio_write(ctx->reset_pin, true);
	alp_delay_ms(1500u);
	/* Puya-flash (PY25Q64LB / 64Mbit) cold-boot workaround -- TI SDK bug confirmed
	 * by the CC35 module vendor 2026-06-18: the FIRST boot after a cold power-on
	 * mis-reads the Puya flash (the bug is specific to 32/64Mbit Puya parts), so the
	 * secure boot never launches the vendor image (host sees reqhdr_rx=0xFFFFFFFF).
	 * Re-boot once with the rails kept up; the second boot reads the now-settled
	 * flash and launches normally.  Validated on silicon (cold reqhdr_rx
	 * 0xFFFFFFFF -> 0x5A5A5A5A, ping_ok climbing, after one hard reset).  The
	 * bringup soak calls cc3501e_hard_reset() again if a single re-boot is not
	 * enough.  Remove once TI ships the Puya flash fix. */
	alp_status_t s = cc3501e_hard_reset(ctx);
	if (s != ALP_OK) return s;

	/* Re-arm BEFORE the version read, not after.
	 *
	 * The device is physically up at this point, and the compat gate below reads
	 * it through cc3501e_get_version() -- which itself refuses on a context marked
	 * down.  Setting the flag afterwards is therefore circular: a context downed by
	 * cc3501e_power_off() (or by a previous version mismatch) could never verify,
	 * so reset returned ALP_OK while every later call still failed
	 * ALP_ERR_NOT_READY.  Bench-proven on a healthy device:
	 * `POWER OFF -> 0  while-off PING -> -2  reset -> 0  PING -> -2`.
	 *
	 * The mismatch path below still clears it, so a genuine wire disagreement is
	 * unchanged -- this only removes the deadlock on the way back up.
	 *
	 * Also re-arm fw_proto_major/fw_proto_minor to 0 here, not just
	 * initialised.  Leaving a PREVIOUS peer's major latched means the
	 * GET_VERSION call two lines down would frame ITS OWN request in that
	 * stale dialect (cc3501e_request_locked()'s want_req_crc reads
	 * ctx->fw_proto_major), and cc3501e_reply_verdict() would decode the
	 * reply against it too -- both wrong for a peer this reset has not
	 * identified yet.  Concretely: a stale major 4 latched from a PRIOR
	 * session, reset against a 3.1 peer whose first GET_VERSION reply is
	 * merely garbled (nothing protects that wire) -- the request now
	 * wrongly carries a CRC trailer the 3.1 firmware rejects, GET_VERSION
	 * never completes, and the transport-hiccup branch below returns
	 * ALP_OK with initialised=true and the bad major still latched: a
	 * permanently wedged link that reports reset as successful.  Zeroing
	 * here makes GET_VERSION go out CRC-less (correct: no major is known
	 * yet) and makes cc3501e_reply_verdict() use its own
	 * fw_proto_major==0 content-based shape detection, exactly like the
	 * very first reset on a fresh ctx. */
	ctx->initialised    = true;
	ctx->fw_proto_major = 0u;
	ctx->fw_proto_minor = 0u;

	/* Wire-protocol compatibility gate (issue #1371): cc3501e-bridge-firmware:DESIGN.md
     * has always documented "host refuses a mismatch" for GET_VERSION, but
     * nothing ever compared the reply against ALP_CC3501E_PROTOCOL_VERSION --
     * mirrors the GD32 bridge's major-version gate (gd32g553_init(),
     * GD32G553_HOST_PROTOCOL_MAJOR), except the CC3501E wire carries a single
     * flat uint16_t (protocol_meta.c), not a major/minor/patch triple, so
     * there is no gradation to be lenient about: any difference means the
     * host cannot know the frame layout it is about to parse.
     *
     * This is the ONLY place the comparison runs -- deliberately NOT inside
     * cc3501e_get_version() itself, which stays a bare round-trip.  Two
     * callers depend on that: the #1116 concurrency regression
     * (tests/zephyr/cc3501e_transport_lock) drives cc3501e_get_version()
     * directly against a modelled slave that never claims to speak
     * ALP_CC3501E_PROTOCOL_VERSION, and the cold-boot liveness soaks
     * (examples/aen/aen-cc3501e-bringup's soak loop, every 8th cycle;
     * examples/peripheral-io/alp-console's cc3501e_bridge_bringup retry) use
     * cc3501e_get_version() purely as "did the round trip complete", not as a
     * compat gate -- putting the check there would turn a liveness probe
     * into a hard failure on real hardware.
     *
     * A GET_VERSION round trip that does not complete at all (the common
     * case immediately after this reset -- the Puya cold-boot flash bug
     * documented above routinely needs a second, caller-driven
     * cc3501e_hard_reset() before the slave answers anything) is NOT a
     * version verdict: only an ANSWERED request can be compared, so leave
     * the context usable and let the caller's own retry loop keep trying. */
	uint16_t     fw_version = 0u;
	alp_status_t vs         = cc3501e_get_version(ctx, &fw_version);
	if (vs != ALP_OK) {
		/* Transport hiccup reading the version, not a version disagreement: leave
		 * the context as it was and let the caller retry (the soaks treat
		 * GET_VERSION as a liveness probe, not a compat gate). */
		return ALP_OK;
	}
	/* MAJOR gates the link; MINOR does not (ADR 0033).
	 *
	 * The old check was `fw_version != ALP_CC3501E_PROTOCOL_VERSION` on a flat
	 * integer, which refused on ANY difference -- so a purely additive firmware
	 * change (a new opcode this host simply never sends) cost a customer the
	 * same forced lockstep reflash as a change that genuinely reinterprets
	 * bytes an old host writes as zero.  Two of the four v6..v9 bumps were the
	 * harmless kind.
	 *
	 * MAJOR is defined as "an unchanged host would be MISREAD", so refusing on
	 * it keeps exactly the protection v7 and v8 needed.  MINOR is defined as
	 * additive, which is what makes connecting across it safe: this host never
	 * sends an opcode it does not know, and the firmware never spontaneously
	 * emits an event nobody armed. */
	const uint8_t fw_major = (uint8_t)ALP_CC3501E_PROTOCOL_VERSION_MAJOR(fw_version);
	const uint8_t fw_minor = (uint8_t)ALP_CC3501E_PROTOCOL_VERSION_MINOR(fw_version);

	/* v4.0 (#2035): the host is BILINGUAL during the migration window -- accepts
	 * this driver's own ALP_CC3501E_PROTOCOL_MAJOR (4) AND the previous
	 * ALP_CC3501E_PROTOCOL_MAJOR_LEGACY (3), never anything else.  See the
	 * migration-order paragraph above ALP_CC3501E_PROTOCOL_MAJOR in
	 * <alp/protocol/cc3501e.h>: a board still on 3.1 firmware needs the OTA
	 * itself to get to 4.0, and that OTA has to run THROUGH this host, so a host
	 * that refused major 3 outright could never reach a board that needed it.
	 * cc3501e_reply_verdict() below (and cc3501e_request_locked()'s TX side)
	 * key their legacy-vs-4.0 wire decode off ctx->fw_proto_major, recorded a
	 * few lines down. */
	if (!cc3501e_fw_major_is_acceptable(fw_major)) {
		/* Permanent, not transient: retrying cannot reconcile two binaries
         * that disagree about the wire, so unlike the transport-timeout case
         * above this clears initialised -- every later call on this ctx now
         * fails ALP_ERR_NOT_READY instead of talking a wrong frame layout to
         * a radio.
         *
         * fw_major == 0 is the DISTINGUISHABLE legacy case: a firmware built
         * before ADR 0033 answers with its raw v1..v9 integer, which decodes
         * to major 0.  Same refusal -- such a firmware really does disagree
         * about the wire -- but the caller can tell "older than the scheme"
         * from "disagrees about the frame layout", which is the difference
         * between "reflash the bridge" and "something is wrong". */
		ctx->initialised    = false;
		ctx->fw_proto_major = fw_major;
		ctx->fw_proto_minor = fw_minor;
		return ALP_ERR_VERSION;
	}

	/* Same major: usable.  Record what the firmware actually speaks so callers
	 * can gate a feature on it -- though CMD_GET_CAPABILITIES is the better
	 * question, because it reports what this BUILD implements rather than what
	 * its version number implies. */
	ctx->fw_proto_major = fw_major;
	ctx->fw_proto_minor = fw_minor;

	return ALP_OK;
}

alp_status_t cc3501e_hard_reset(cc3501e_t *ctx)
{
	/* Recovery primitive like cc3501e_reset(): must work on a context marked down,
	 * or there is no way back from a supply gate or a version mismatch. */
	if (ctx == NULL || ctx->bus == NULL) return ALP_ERR_NOT_READY;
	if (ctx->reset_pin == NULL) return ALP_ERR_NOSUPPORT;
	/* Pulse nRESET while keeping WIFI_EN asserted so the module re-boots WITHOUT a
	 * cold power cycle (a cold cycle would re-trigger the Puya-flash bug).  This is
	 * the "second boot" of the cold-boot workaround and the retry primitive the
	 * bringup soak uses when a cold-booted module has not come up yet. */
	(void)alp_gpio_write(ctx->reset_pin, false); /* assert nRESET; rails stay up */
	alp_delay_ms(50u);
	(void)alp_gpio_write(ctx->reset_pin, true); /* release -> re-boot */

	/* BLIND boot settle -- NO clocking until the slave is armed.  This is the
	 * cold first-contact fix: on the CS-less fixed-count link, any byte the host
	 * clocks BEFORE the slave's SPI is armed (e.g. a readiness poll's probes) is
	 * not consumed by the slave, which sets a permanent 1-byte frame offset that
	 * cannot self-correct (clocking advances both sides equally).  So the host
	 * stays QUIET while the module boots + arms; the slave then parks driving the
	 * 0xA5 marker, and the caller's first PING lands at the slave's fresh frame
	 * boundary = aligned.  The Wi-Fi build cold-boots (Puya double-boot + crypto
	 * Board_init) in ~2-3 s; 3.5 s covers it with margin and no clocking. */
	alp_delay_ms(3500u);
	return ALP_OK;
}

/* Map a CC3501E response status byte (first reply-payload byte, per
 * <alp/protocol/cc3501e.h>) onto the SDK's alp_status_t.
 *
 * BILINGUAL (v4.0, #2035): @p fw_major selects which byte means success.
 * fw_major == ALP_CC3501E_PROTOCOL_MAJOR_LEGACY (3, the pre-4.0 wire) treats
 * ALP_CC3501E_RESP_OK_LEGACY (0x00) as OK, matching every firmware still on
 * the migration path (see the migration-order note in <alp/protocol/
 * cc3501e.h>).  Any OTHER major -- 4, or 0 meaning "the version gate has not
 * run yet" -- treats ONLY ALP_CC3501E_RESP_OK (0x5A) as OK; 0x00 from a
 * fw_major-4 peer is never legitimate (real 4.0 success is always 0x5A), so
 * it is left unmapped and falls through to the IO default below -- which is
 * exactly right, because the only way a 4.0 peer emits a literal 0x00 is the
 * #1378 dead-phase alias this whole change exists to close.  fw_major == 0
 * (unknown, pre-gate) getting the STRICT branch here is deliberate too: the
 * one caller that runs before the major is known (cc3501e_get_version(), via
 * cc3501e_reply_verdict() below) does its OWN shape detection off the raw
 * status byte before ever reaching this function, so by the time resp_to_
 * status() sees a 0x00 with fw_major still 0, that 0x00 has already been
 * classified as the LEGACY shape and resp_to_status() is called with
 * ALP_CC3501E_PROTOCOL_MAJOR_LEGACY, not 0 -- see cc3501e_reply_verdict(). */
static alp_status_t resp_to_status(uint8_t resp, uint8_t fw_major)
{
	if (resp == ALP_CC3501E_RESP_OK) return ALP_OK;
	/* fw_major MUST gate this -- a fw_proto_major-4 peer's real success byte is
	 * ALWAYS 0x5A (matched above), so 0x00 reaching here from that peer is
	 * never legitimate and must fall through to the IO default, NOT be
	 * accepted unconditionally.  Dropping the fw_major check here would widen
	 * the #1378 alias from "an all-zero reply" to "any reply whose status
	 * byte happens to be 0x00", covering strictly more cases than the CRC
	 * check alone defends -- see
	 * test_cc3501e_reply_verdict_major4_legacy_status_byte_rejected. */
	if (resp == ALP_CC3501E_RESP_OK_LEGACY &&
	    fw_major == (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR_LEGACY) {
		return ALP_OK;
	}
	switch (resp) {
	case ALP_CC3501E_RESP_ERR_INVALID:
		return ALP_ERR_INVAL;
	case ALP_CC3501E_RESP_ERR_BUSY:
		return ALP_ERR_BUSY;
	case ALP_CC3501E_RESP_ERR_TIMEOUT:
		return ALP_ERR_TIMEOUT;
	case ALP_CC3501E_RESP_ERR_NO_MEM:
		return ALP_ERR_NOMEM;
	case ALP_CC3501E_RESP_ERR_NOT_READY:
		return ALP_ERR_NOT_READY;
	case ALP_CC3501E_RESP_ERR_VERSION:
		return ALP_ERR_VERSION;
	case ALP_CC3501E_RESP_ERR_STATE:
		/* Deterministic firmware reject (e.g. BLE_GATT_REGISTER's NimBLE
		 * ble_gatts_mutable() ordering guard) -- distinct from ERR_BUSY's
		 * "worker still running, re-poll" and from ERR_RADIO's "transport/
		 * radio fault, maybe transient".  Mapped to the SAME ALP_ERR_BUSY a
		 * caller sees for a retryable busy, but poll_by_repeat below treats
		 * it as TERMINAL (checks the raw resp, not just the mapped status)
		 * so it is never retried and never burns the poll budget. */
		return ALP_ERR_BUSY;
	case ALP_CC3501E_RESP_ERR_RADIO:
	case ALP_CC3501E_RESP_ERR_PROTOCOL:
	case ALP_CC3501E_RESP_ERR_INTERNAL:
	default:
		return ALP_ERR_IO;
	}
}

/* #2035: whether a MULTI-BYTE reply (resp_payload_len > 1, i.e. the status
 * byte plus real data) is allowed to read back ALL-ZERO without that being
 * the #1378 dead-phase alias (a dead bus phase clocks back literal 0x00 for
 * every byte, and 0x00 is also ALP_CC3501E_RESP_OK -- see the caller).
 *
 * STRUCTURAL DEFAULT: an opcode NOT listed here is assumed UNABLE to
 * legitimately reply all-zero, so the caller below treats an all-zero reply
 * from it as the dead-phase alias.  This inverts the old shape (a
 * hand-maintained list of PROTECTED opcodes, which left every new opcode
 * unprotected until someone remembered to add it -- exactly how GET_MAC
 * shipped able to hand back a false 00:00:00:00:00:00 "success").  A new
 * opcode is now safe by construction; it only loses that protection by
 * earning a place below, and it earns that place only with a documented,
 * positive reason its reply CAN legitimately be all-zero -- reading the
 * struct definitions in <alp/protocol/cc3501e.h>, not a guess.
 *
 * Deliberately NOT consulted for single-byte (bare-status) replies: RESP_OK
 * alone IS the entire legitimate success reply for most opcodes in this
 * protocol (PING, the WIFI_*_STOP/DISCONNECT family, the SOCK_CONNECT/BIND/
 * LISTEN/CLOSE family, most BLE_* calls, GPIO_CONFIGURE/WRITE/SET_INTERRUPT,
 * ...), so a bare RESP_OK is structurally indistinguishable from the
 * dead-phase alias for THOSE opcodes -- defaulting to "protected" there
 * would reject the ordinary success case of nearly every opcode in the
 * table.  Telling the two apart needs a firmware fact that ONE PARTICULAR
 * opcode's handler can never legitimately answer bare OK, which is why that
 * check stays its own narrow, explicitly-per-opcode mechanism (see the
 * WIFI_CONNECT_STA / WIFI_AP_START handling at the call site) instead of
 * folding into this one. */
bool cc3501e_reply_may_be_all_zero(alp_cc3501e_cmd_t cmd)
{
	switch (cmd) {
	case ALP_CC3501E_CMD_WIFI_STATUS:
		/* alp_cc3501e_wifi_status_t: state=DISCONNECTED(0), fail_reason=
		 * NONE(0), rssi_dbm=0 (documented "@warning NOT A MEASUREMENT...
		 * always 0 on the wire", <alp/protocol/cc3501e.h>), reserved=0.
		 * "Never connected since boot" is a real, common device state and
		 * reads back byte-identical to this. */
		return true;
	case ALP_CC3501E_CMD_WIFI_GET_RSSI:
		/* 0 dBm is a LEGAL int8 RSSI reading (see the wifi_status_t
		 * rssi_dbm doc comment) -- there is no in-band sentinel to tell it
		 * apart from a dead phase. */
		return true;
	case ALP_CC3501E_CMD_WIFI_GET_IP:
		/* 0.0.0.0 legitimately means "no IP assigned yet" (STA not
		 * associated / AP not started). */
		return true;
	case ALP_CC3501E_CMD_DIAG_GET_STATS:
		/* Zero counters right after boot are a real state, not a dead
		 * link. */
		return true;
	case ALP_CC3501E_CMD_SOCK_RECV:
		/* alp_cc3501e_sock_recv_resp_t is explicit: "data_len == 0 means
		 * no data was available (non-blocking semantics; the status is
		 * still ALP_CC3501E_RESP_OK)", and `from` is genuinely zeroed for
		 * a STREAM socket. */
		return true;
	case ALP_CC3501E_CMD_OTA_STATUS:
		/* alp_cc3501e_ota_status_t: state=IDLE(0), bytes_written=0,
		 * total_len=0, pending=NONE(0) is the real, common "no OTA
		 * session has ever run on this device" state. */
		return true;
	case ALP_CC3501E_CMD_OTA_UPDATE_MODE:
		/* alp_cc3501e_ota_update_mode_t: mode==0 ("left/never entered
		 * update mode") is documented in <alp/protocol/cc3501e.h> as
		 * byte-identical to the dead-phase alias and NOT structurally
		 * defeatable here -- mode==1 IS proof (a dead phase can never
		 * forge it), so the host confirm loop already treats mode==0 as
		 * needing corroboration, not as proof either way.  Do not add a
		 * canary here; the header explains why this needs a wire-level
		 * fix instead (#1696). */
		return true;
	case ALP_CC3501E_CMD_GPIO_READ:
		/* GPIO level 0 (pad sampled LOW) is an ordinary reading, not a
		 * fault. */
		return true;
	case ALP_CC3501E_CMD_SPI1_TRANSFER:
		/* alp_cc3501e_spi1_transfer_resp_t: "a len == 0 transfer with
		 * flags == 0 is a standalone CS deassert" is documented normal
		 * usage, and seq legitimately starts at 0 -- len=0/flags=0/seq=0
		 * is that reply, byte-identical to an all-zero dead phase. */
		return true;
	default:
		return false;
	}
}

/* Blocker-1 fix (#2035 follow-up): whether opcode @p cmd's reply carries real
 * payload data beyond the status byte -- i.e. whether its UNPADDED reply
 * length is > 1.  This is what cc3501e_reply_verdict() below actually needs
 * to know to pick the right dead-phase mechanism, and it is a fixed fact of
 * the wire format documented in <alp/protocol/cc3501e.h>, NOT something the
 * wire's declared payload_len can tell it: EVERY reply, bare-status or not,
 * is zero-padded up to an @ref ALP_CC3501E_REPLY_PAD (8 B) multiple with the
 * pad folded into that declared length (see cc3501e_events.c's
 * cc3501e_events_poll() for the sibling host-side consequence), so a real
 * bare-status reply (unpadded length 1) and a real short-data reply (e.g.
 * WIFI_GET_RSSI's unpadded length 2) both show up on the wire as
 * payload_len == 8 -- indistinguishable by length alone.  Using payload_len
 * itself to choose the mechanism (the pre-fix code) therefore always took
 * the "has data" branch for every real reply, which silently starved every
 * bare-status opcode of its own, more permissive heuristic -- see the
 * dead_phase dispatch below for the concrete failure.
 *
 * STRUCTURAL DEFAULT, same polarity as cc3501e_reply_may_be_all_zero() above:
 * an opcode NOT listed here is assumed bare-status (unpadded length 1),
 * because that is what most opcodes in this table actually are -- PING, the
 * WIFI_*_STOP/DISCONNECT family, the SOCK_CONNECT/BIND/LISTEN/CLOSE family,
 * most BLE_* calls, GPIO_CONFIGURE/WRITE/SET_INTERRUPT, RESET,
 * WIFI_SCAN_START/STOP, WIFI_CONNECT_STA/AP_START (handled by the narrow
 * per-opcode check at the call site instead), OTA_BEGIN/WRITE/FINISH/ABORT/
 * PROMOTE, STREAM_WRITE, SPI1_RELEASE, CAM_ENABLE/DISABLE, POWER_POLICY,
 * DIAG_LOG_LEVEL, and GET_PENDING_EVENTS (its DATA is genuinely optional --
 * "an empty list ... means nothing was queued", see the enum comment -- so
 * an all-zero reply is its ordinary "nothing pending" case, not evidence of
 * a dead phase).  A new opcode earns a place below only by carrying a real,
 * documented reply struct wider than the status byte -- read the struct
 * definitions in <alp/protocol/cc3501e.h>, not a guess. */
static bool cc3501e_reply_carries_data(alp_cc3501e_cmd_t cmd)
{
	switch (cmd) {
	case ALP_CC3501E_CMD_GET_VERSION:
	case ALP_CC3501E_CMD_GET_MAC:
	case ALP_CC3501E_CMD_GET_DIAG_INFO:
	case ALP_CC3501E_CMD_GET_CAPABILITIES:
	case ALP_CC3501E_CMD_WIFI_GET_RSSI:
	case ALP_CC3501E_CMD_WIFI_GET_IP:
	case ALP_CC3501E_CMD_WIFI_STATUS:
	case ALP_CC3501E_CMD_SOCK_OPEN:
	case ALP_CC3501E_CMD_SOCK_SEND:
	case ALP_CC3501E_CMD_SOCK_RECV:
	case ALP_CC3501E_CMD_OTA_STATUS:
	case ALP_CC3501E_CMD_OTA_UPDATE_MODE:
	case ALP_CC3501E_CMD_GPIO_READ:
	case ALP_CC3501E_CMD_SPI1_CONFIGURE:
	case ALP_CC3501E_CMD_SPI1_TRANSFER:
	case ALP_CC3501E_CMD_BLE_GATT_READ:
	case ALP_CC3501E_CMD_DIAG_GET_STATS:
		return true;
	default:
		return false;
	}
}

/* v4.0 (#2035): verify a reply's CRC-16/CCITT-FALSE trailer.
 *
 * Span per <alp/protocol/cc3501e.h>: the reply's own 4 header bytes (already
 * read and saved into @p reply_hdr before the payload phase overwrites the
 * scratch buffer it was read into) plus every payload byte EXCEPT the
 * trailing @ref ALP_CC3501E_CRC_BYTES, which the wire carries as the CRC
 * itself, LE16, at the last 2 bytes of the (possibly padded) payload.
 *
 * A payload too short to even hold a CRC (@p payload_len < CRC_BYTES) is
 * rejected outright -- a real 4.0 reply always has room, so an underlength
 * one is itself evidence of desync, not a shape this function should try to
 * make sense of. */
static bool cc3501e_reply_crc_ok(const uint8_t  reply_hdr[ALP_CC3501E_HEADER_BYTES],
                                 const uint8_t *payload,
                                 uint16_t       payload_len)
{
	if (payload == NULL || payload_len < ALP_CC3501E_CRC_BYTES) return false;
	const uint16_t covered  = (uint16_t)(payload_len - ALP_CC3501E_CRC_BYTES);
	uint16_t       crc      = alp_crc16_ccitt_false(reply_hdr, ALP_CC3501E_HEADER_BYTES);
	crc                     = alp_crc16_ccitt_false_update(crc, payload, covered);
	const uint16_t wire_crc = (uint16_t)payload[covered] | ((uint16_t)payload[covered + 1u] << 8);
	return crc == wire_crc;
}

/* v4.0 (#2035): the pure reply-decision -- given an ALREADY-READ reply header
 * and payload, decide the alp_status_t this reply represents.  No I/O, no
 * transport lock: cc3501e_request_locked() below is the only real caller, and
 * pulling the decision out into its own function is what lets
 * tests/zephyr/chips/src/test_cc3501e.c exercise every wire shape (legacy
 * major-3, 4.0-with-valid-CRC, 4.0-with-corrupted-CRC, an all-zero dead
 * phase under each) directly with fabricated frames, with no SPI bus needed
 * -- declared non-static in cc3501e_internal.h for exactly that reason, same
 * pattern as cc3501e_reply_may_be_all_zero() above.
 *
 * SHAPE DETECTION.  Which wire dialect a reply uses is keyed off @p
 * fw_proto_major, EXCEPT at fw_proto_major == 0 (the version gate has not
 * run yet -- the one caller here is cc3501e_get_version() from inside
 * cc3501e_reset() itself, per the migration-order note in
 * <alp/protocol/cc3501e.h>): there, the status byte decides its own shape,
 * because that is the one piece of information available before the major is
 * known.  resp == ALP_CC3501E_RESP_OK (0x5A) can only mean "this is a 4.0
 * reply" -- 0x5A is not a legacy status byte at all -- and anything else
 * (0x00 or any legacy error code) is read as the legacy, no-CRC shape. */
alp_status_t cc3501e_reply_verdict(alp_cc3501e_cmd_t cmd,
                                   uint8_t           fw_proto_major,
                                   const uint8_t     reply_hdr[ALP_CC3501E_HEADER_BYTES],
                                   const uint8_t    *payload,
                                   uint16_t          payload_len)
{
	if (payload == NULL || payload_len < 1u) return ALP_ERR_IO; /* always >= 1 status byte */
	const uint8_t resp = payload[0];

	bool major4_shape;
	if (fw_proto_major >= (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR) {
		major4_shape = true;
	} else if (fw_proto_major == (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR_LEGACY) {
		major4_shape = false;
	} else {
		major4_shape = (resp == ALP_CC3501E_RESP_OK);
	}

	if (major4_shape && !cc3501e_reply_crc_ok(reply_hdr, payload, payload_len)) {
		/* Covers a corrupted trailer AND a dead all-zero payload phase: the
		 * CRC of an all-zero span is never the wire's all-zero "CRC", so this
		 * alone already defeats #1378 for every 4.0 reply, on top of (not
		 * instead of) the all-zero heuristic below. */
		return ALP_ERR_IO;
	}

	/* #1378 / #2035: a dead bus phase reads back literal 0x00 for every byte
	 * it clocks -- this repo's own silicon finding (see hal/ti/
	 * cc3501e_hw_ti_wifi.c's cc3501e_hw_wifi_lazy_start(), "the host then
	 * reads 0x00000000 from a dead link").  0x00 is ALSO the legacy
	 * ALP_CC3501E_RESP_OK_LEGACY, so on the legacy (fw_proto_major 3) wire a
	 * header that read intact followed by a payload phase that dies in the
	 * inter-phase gap is silently indistinguishable from a real, successful
	 * all-zero reply: ALP_OK must require positive evidence the device framed
	 * a reply, not merely the absence of evidence that it did not.  Kept even
	 * on the major4_shape path (belt and braces, costs nothing once the CRC
	 * above already passed) rather than being gated on !major4_shape.
	 *
	 * Two shapes, two mechanisms (a single content-based check cannot cover
	 * both -- see cc3501e_reply_may_be_all_zero()'s doc comment).  Dispatched
	 * on cc3501e_reply_carries_data(cmd), NOT on payload_len: the firmware
	 * zero-pads every reply's payload up to an ALP_CC3501E_REPLY_PAD (8 B)
	 * multiple with the pad folded INTO the declared payload_len (see
	 * cc3501e_reply_carries_data()'s doc comment and cc3501e_events.c), so a
	 * genuine bare-status reply and a genuine short-data reply both arrive
	 * with the SAME wire payload_len -- payload_len cannot tell them apart.
	 * (This used to be gated on `payload_len == 1u`, which no real reply
	 * from real firmware is ever declared as -- always >= REPLY_PAD -- so
	 * that branch was dead against the wire and every bare-status opcode
	 * fell into shape 2 below and was wrongly treated as PROTECTED, i.e. a
	 * real legacy RESP_OK_LEGACY (0x00) reply from e.g. PING or
	 * GET_PENDING_EVENTS read back as all-zero and was rejected as a dead
	 * phase.  Fixed here; test_cc3501e_reply_verdict_major3_bare_status_is_padded
	 * in tests/zephyr/chips/src/test_cc3501e.c fabricates the real 8-byte
	 * padded shape a bridge actually sends and pins this down.)
	 *
	 *  1. !cc3501e_reply_carries_data(cmd) (bare status, no real data).
	 *     RESP_OK alone is the legitimate success reply for most opcodes
	 *     here, so "all-zero" carries no signal by itself; only a firmware
	 *     fact that ONE opcode's handler can never legitimately answer bare
	 *     OK makes it self-evidently the dead-phase alias.  Still a narrow,
	 *     per-opcode allowlist for exactly that reason:
	 *
	 *     WIFI_CONNECT_STA (0x12) -- #1378.  Its firmware handler
	 *     (handle_worker_routed_payload's WORKER_IDLE case,
	 *     cc3501e-bridge-firmware:src/protocol.c) UNCONDITIONALLY acks a
	 *     fresh submit with RESP_ERR_BUSY.  Rejecting the alias here avoids
	 *     handing the caller a false "submitted", which is exactly the #1376
	 *     false-connect mechanism.  cc3501e_wifi_connect() no longer trusts
	 *     this ack in either direction (it only trusts the independent
	 *     WIFI_STATUS latch -- see cc3501e_wifi.c), so for that opcode this
	 *     is defense in depth.
	 *
	 *     WIFI_AP_START (0x14) -- #1385.  Same handler, same unconditional
	 *     BUSY submit ack, AND the ONE path that could otherwise return a
	 *     bare RESP_OK for this opcode is unreachable to the host: the drain
	 *     (cc3501e-bridge-firmware:src/worker.c's worker_run_pending()) calls
	 *     worker_reset() for exactly CONNECT_STA and AP_START *before*
	 *     cc3501e_bridge_ready() re-arms the link, so the WORKER_DONE branch
	 *     that would reply RESP_OK is wiped while the host is still held off
	 *     and can never be collected.  Unlike CONNECT_STA this is NOT defense
	 *     in depth: this rejection was cc3501e_wifi_ap_start()'s ONLY route to
	 *     ALP_OK, so that wrapper no longer polls it -- it submits
	 *     WIFI_AP_START exactly once and reports ALP_ERR_TIMEOUT
	 *     unconditionally (see cc3501e_wifi.c), since there is no reply this
	 *     opcode can ever frame as success.  Tracked in #1696 -- re-check
	 *     whether an AP-side latch can be added now the wire has a CRC.
	 *     (#1385 is CLOSED.)
	 *
	 *     OTA_PROMOTE (0x46) is deliberately NOT on this list, despite being
	 *     the sharpest case named in #1378/#1385: handle_ota_promote()
	 *     (cc3501e-bridge-firmware:src/protocol_ota.c) returns
	 *     hw_to_resp(cc3501e_hw_ota_promote()), and the TI HAL's
	 *     cc3501e_hw_ota_promote() (hal/ti/cc3501e_hw_ti_ota.c) arms the
	 *     deferred swap-reboot and returns CC3501E_HW_OK UNCONDITIONALLY -- a
	 *     bare RESP_OK (reply_data_len 0 -> payload len 1) is that opcode's
	 *     ONLY success reply.  Rejecting it here would make
	 *     cc3501e_ota_promote() always report ALP_ERR_IO and break firmware
	 *     promotion outright.  On the legacy wire this alias is still only
	 *     closeable by host-side confirmation against OTA_STATUS (0x44); on
	 *     the 4.0 wire the CRC check above now covers it like every other
	 *     opcode, so the residual risk here is legacy-only.  Tracked in
	 *     #1696; weigh against #1123, which is also OTA state confirmation.
	 *
	 *  2. cc3501e_reply_carries_data(cmd) (status + real data).  Requiring
	 *     the status byte AND every data byte to be simultaneously zero is a
	 *     much rarer coincidence than a bare zero status alone, so THIS
	 *     shape defaults to PROTECTED -- see cc3501e_reply_may_be_all_zero()
	 *     for the (deliberately small, each individually justified) list of
	 *     opcodes exempted from that default.  This is the shape #2035's
	 *     cc3501e_wifi_get_mac() bug lived in: GET_MAC's reply is 1 status
	 *     byte + 6 MAC bytes, so the old bare-status-only allowlist above
	 *     never even looked at it. */
	bool all_zero = true;
	for (uint16_t i = 0; i < payload_len; i++) {
		if (payload[i] != 0x00u) {
			all_zero = false;
			break;
		}
	}
	bool dead_phase;
	if (!cc3501e_reply_carries_data(cmd)) {
		dead_phase = all_zero && (cmd == ALP_CC3501E_CMD_WIFI_CONNECT_STA ||
		                          cmd == ALP_CC3501E_CMD_WIFI_AP_START);
	} else {
		dead_phase = all_zero && !cc3501e_reply_may_be_all_zero(cmd);
	}
	if (dead_phase) return ALP_ERR_IO;

	/* The status byte's OWN major for resp_to_status(): at fw_proto_major==0
	 * this reply already told us its shape above, so hand resp_to_status() the
	 * major THAT shape implies (LEGACY for a 0x00 reply, this driver's own
	 * PROTOCOL_MAJOR for a 0x5A reply) rather than the still-unknown 0 --
	 * otherwise a legitimate legacy 0x00 OK would fall through resp_to_status's
	 * strict (non-legacy) branch and misreport ALP_ERR_IO. */
	const uint8_t status_major = major4_shape ? (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR
	                                          : (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR_LEGACY;
	return resp_to_status(resp, status_major);
}

/* ---- transport-transaction lock (issue #1116) -----------------------------
 *
 * cc3501e_request() is the ONLY place the 4-phase SPI exchange runs, and
 * five independent callers (Wi-Fi, BLE, GPIO proxy, the console companion,
 * the OTA path) share one cc3501e_t.  Without serialisation here, two
 * concurrent transactions interleave on the CS-less link and desync it, or
 * one caller reads back another caller's reply -- see the tx_scratch /
 * rx_scratch fields cc3501e_request reads and writes with no protection
 * before this fix.
 *
 * Compiler-builtin atomics on a plain bool, not an OS mutex: this TU is
 * OS-agnostic (chips/cc3501e/cc3501e_core.c builds into both the Zephyr
 * module and the plain-CMake / Yocto libalp_chips.a -- see CMakeLists.txt's
 * ALP_SDK_CHIP_LIST comment: "none of the chips/<id>/<id>.c cores include a
 * Zephyr/vendor header"), so it cannot call k_mutex_lock.  Same rationale
 * and the same __atomic_* primitives as src/common/alp_slot_claim.h, which
 * the dispatcher pools use for the identical portability reason.
 *
 * The acquire is BOUNDED, mirroring src/zephyr/v2n_supervisor.c's
 * alp_z_v2n_supervisor_acquire() for the structurally identical "one
 * shared transport, many portable backends" problem: a caller stuck behind
 * a wedged transaction gets ALP_ERR_BUSY back instead of hanging forever.
 * CONFIG_ALP_SDK_CC3501E_REQUEST_LOCK_TIMEOUT_MS is a real Kconfig knob on
 * Zephyr (zephyr/kconfigs/chips.kconfig) -- the Zephyr build injects every
 * CONFIG_* symbol as a compiler macro for every TU in the zephyr_library,
 * including this OS-agnostic core, so no <zephyr/...> include is needed to
 * see it.  Non-Zephyr backends have no Kconfig, so they fall back to the
 * #ifndef default below (same "portable constant, Zephyr-overridable"
 * shape already used by CC3501E_PHASE_SETTLE_US etc. in this file). */
#ifndef CONFIG_ALP_SDK_CC3501E_REQUEST_LOCK_TIMEOUT_MS
#define CONFIG_ALP_SDK_CC3501E_REQUEST_LOCK_TIMEOUT_MS 100u
#endif

static bool cc3501e_lock_try(cc3501e_t *ctx)
{
	bool expected = false;
	return __atomic_compare_exchange_n(
	    &ctx->request_lock, &expected, true, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

/* Bounded acquire: try once (the common uncontended case costs nothing),
 * then poll once per millisecond via the portable alp_delay_ms (which
 * yields the CPU on every OS backend, unlike alp_delay_us's busy-wait --
 * needed so a contending thread actually gets scheduled) until the
 * Kconfig-bounded budget elapses. */
/* Attention-line arming hook (#130).  Weak no-op here; the companion console
 * overrides it when CONFIG_ALP_SDK_CC3501E_EVENT_IRQ is on.
 *
 * The attention wire IS the READY wire, and READY is raised on every bridge
 * re-arm -- so while the host is transacting, an edge means "a transaction just
 * finished", not "an event is pending".  Draining on those edges livelocked the
 * device on silicon (the app booted, entered its soak and went silent), and
 * merely masking the line across each drain only bounded it: a seconds-long
 * radio op still produced an edge whose drain contended with the op, turning
 * WIFI_SCAN into a timeout.
 *
 * The host's own request lock is the exact boundary that disambiguates it.
 * While the lock is HELD a transaction is in flight and every edge is flow
 * control, so the interrupt is masked.  While it is FREE the host is idle,
 * nothing is re-arming, and any rising edge can only be the firmware asking for
 * attention.  No pulse-width qualification and no second wire needed -- the
 * host's state supplies the missing bit. */
__attribute__((weak)) void cc3501e_attn_set_armed(bool armed)
{
	(void)armed;
}

/* STICKY application intent.  The ISR masks the line and every request masks it
 * again, so something must put it back -- and lock_release() must not do it
 * unconditionally (re-arming there drops a drain into the middle of a radio op
 * and hung the device after WIFI_SCAN).  The application's LAST
 * cc3501e_attn_arm() call is the authority.
 *
 * Without this the line is masked by the FIRST edge and never re-armed: bench
 * 2026-08-26 measured the ISR frozen at exactly 1 across a 40 s armed window
 * while the firmware kept pulsing and events sat in the ring (#130). */
static volatile bool cc3501e_attn_desired;

/* The transport layer may hold an internal back-off that suppresses re-arming
 * (it cannot tell an attention pulse from ordinary flow control).  An EXPLICIT
 * application arm must not be swallowed by it -- bench 2026-08-27: one
 * boot-time edge latched the back-off, the application's later
 * cc3501e_attn_arm(true) returned without arming, and the line stayed masked for
 * the whole window (ISR frozen at 1, arm calls counted but ineffective). */
__attribute__((weak)) void cc3501e_attn_clear_backoff(void)
{
}

void cc3501e_attn_arm(cc3501e_t *ctx, bool armed)
{
	(void)ctx;
	cc3501e_attn_desired = armed;
	if (armed) {
		cc3501e_attn_clear_backoff();
	}
	cc3501e_attn_set_armed(armed);
}

void cc3501e_attn_rearm_if_desired(cc3501e_t *ctx)
{
	(void)ctx;
	if (cc3501e_attn_desired) {
		cc3501e_attn_set_armed(true);
	}
}

static alp_status_t cc3501e_lock_acquire(cc3501e_t *ctx)
{
	cc3501e_attn_set_armed(false); /* a transaction is starting -- see the hook */
	if (cc3501e_lock_try(ctx)) return ALP_OK;
	for (uint32_t waited_ms = 0u; waited_ms < CONFIG_ALP_SDK_CC3501E_REQUEST_LOCK_TIMEOUT_MS;
	     waited_ms++) {
		alp_delay_ms(1u);
		if (cc3501e_lock_try(ctx)) return ALP_OK;
	}
	return ALP_ERR_BUSY;
}

static void cc3501e_lock_release(cc3501e_t *ctx)
{
	__atomic_store_n(&ctx->request_lock, false, __ATOMIC_RELEASE);
	/* Deliberately does NOT re-arm.  Releasing the lock means this REQUEST
	 * finished, not that the host is idle: cc3501e_wifi_connect() submits and
	 * then polls WIFI_STATUS, so the lock is free in the gaps of one operation.
	 * Re-arming there put a drain in the middle of a radio op and hung the
	 * device after WIFI_SCAN (bench 2026-08-26).  Only the application knows
	 * when it is genuinely done -- it re-arms via cc3501e_attn_arm(). */
	/* Re-arm ONLY inside an explicit cc3501e_attn_arm(ctx, true) window.
	 * During ordinary traffic the application has not armed, so this is a
	 * no-op and the WIFI_SCAN hang the comment above describes stays fixed. */
	cc3501e_attn_rearm_if_desired(ctx);
}
alp_status_t cc3501e_sync(cc3501e_t *ctx, uint32_t timeout_ms)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;

	/* MOSI is don't-care while syncing; 0xFF reads as a reserved-range
	 * ("no-op probe") header on the slave, which re-arms its header phase
	 * (firmware P0-2) so it keeps driving the 0xA5 marker -- making this walk
	 * non-destructive. */
	uint8_t tx = 0xFFu;
	uint8_t rx = 0u;

	/* Worst case, clock through one full in-flight request+reply frame to
	 * reach the slave's parked header boundary; "parked" = a run of two
	 * header-widths of 0xA5 (rejects a stray 0xA5 byte inside reply data). */
	const uint32_t walk_max = 2u * (uint32_t)(ALP_CC3501E_HEADER_BYTES + ALP_CC3501E_MAX_PAYLOAD);
	const uint32_t run_need = 2u * (uint32_t)ALP_CC3501E_HEADER_BYTES;
	const uint32_t attempts = (timeout_ms > 0u) ? timeout_ms : 1u;

	/* Serialise against cc3501e_request() (issue #1116).  This walk clocks
	 * the SAME CS-less bus, one byte at a time, up to walk_max times -- a
	 * concurrent 4-phase request would interleave with it and desync the
	 * link exactly as two concurrent requests would.  cc3501e_request()'s
	 * doxygen now advertises thread-safety, so a caller may legitimately
	 * re-sync from a second thread; that has to be safe.
	 *
	 * Held across the WHOLE walk, not per attempt: re-aligning to the
	 * slave's header boundary is only meaningful if nothing else moves the
	 * bus underneath us.  A request that lands mid-sync therefore gets
	 * ALP_ERR_BUSY from its own bounded acquire rather than corrupting the
	 * recovery -- which is the honest answer, since the link is by
	 * definition not usable until the sync completes. */
	alp_status_t lrc = cc3501e_lock_acquire(ctx);
	if (lrc != ALP_OK) return lrc;

	alp_status_t rc = ALP_ERR_TIMEOUT;
	for (uint32_t a = 0u; a < attempts && rc == ALP_ERR_TIMEOUT; a++) {
		uint32_t run = 0u;
		for (uint32_t w = 0u; w < walk_max; w++) {
			if (alp_spi_transceive(ctx->bus, &tx, &rx, 1u) != ALP_OK) {
				rc = ALP_ERR_IO;
				break;
			}
			if (rx == ALP_CC3501E_SYNC_IDLE) {
				if (++run >= run_need) { /* aligned at a clean header boundary */
					rc = ALP_OK;
					break;
				}
			} else {
				run = 0u;
			}
		}
		if (rc == ALP_ERR_TIMEOUT) {
			alp_delay_ms(1u); /* let the slave drain any in-flight frame + re-arm header phase */
		}
	}

	cc3501e_lock_release(ctx);
	return rc;
}

/* Inter-phase settle (CS-less lockstep): time given to the CC3501E SPI-slave ISR
 * to arm the next fixed-count transfer (request payload, reply payload) before
 * the host clocks it.  ~µs is enough; 200 µs is comfortably safe and negligible
 * vs the per-request budget.  The r2 bridge (CS + host-IRQ) removes the need. */
/* 200 us suits the CALLBACK/DMA slave, which re-arms in its completion ISR.
 * A POLLED slave (OTA update mode) only re-arms when its service loop next
 * enters SPI_transfer, and READY stays HIGH for a moment after a transfer
 * completes -- so the host could clock the request-PAYLOAD phase into a slave
 * that was not listening yet.  Header-only frames (e.g. OTA_STATUS) have no
 * such phase, which is exactly why STATUS kept working while every WRITE
 * returned -5 (silicon 2026-08-21).  Widen it; the cost is a fixed per-phase
 * delay only when READY is not already observed HIGH. */
/* Back to 200 us.  Widening this to 2000 us to give a POLLED slave time to arm
 * REGRESSED normal mode -- it applies to every phase, and update-mode ENTRY
 * (which runs on the ordinary DMA bridge) then timed out at -4.  The polled
 * request-PAYLOAD race is real but must be fixed on the DEVICE side, where it
 * can be scoped to the polled path, not by slowing every host phase. */
/* 40, measured -- was 200.  Profiling one socket frame on silicon showed the
 * settles, not the wire, dominate: at 200 a frame was 1.53 ms against ~164 us of
 * actual wire time for a 281 B reply.  Dropping to 40 took a 262144 B stream
 * from 167 kB/s to 231 kB/s with the link healthy throughout.
 *
 * 20 is TOO LOW -- it gives 0 B/s.  And note the gate before the REPLY HEADER is
 * deliberately NOT this constant: it is a hardcoded 200 us because it waits for
 * the slave to DISPATCH and stage its reply, which is a different and much
 * longer job than re-arming the next phase.  Folding it into this constant was
 * tried and killed the link outright (PING -> -5). */
/* 250 us, not 40.  This is the blind fallback gap cc3501e_reply_gate() uses
 * when READY is unusable -- and on the measured unit it always is: READY is
 * CC3501E GPIO_17 -> Alif P2_6, and P2_6 is SPI1_SCLK_A, so enabling its input
 * buffer degrades the very link it is meant to gate.  g_ready_line_proven
 * therefore stays false and every phase falls back to this constant.
 *
 * REVISION SCOPE.  This comment used to say "on E1M-AEN801 board rev 2626-R2
 * it always is", while the bench line five paragraphs down names serial
 * 2617-0001 -- which is board rev r1, not R2.  The measurement was never taken
 * on an R2 module; no R2 module exists on this bench.  What is NOT revision-
 * specific is the conflict itself: P2_6 carrying SPI1_SCLK_A is an Alif pad
 * function, true of the silicon regardless of how any module is built.  So the
 * gate falls back to this constant on ANY board that routes READY to P2_6 and
 * drives SPI1 -- which covers r1 as measured, and R2 unless its netlist moves
 * READY, which has not been checked.  Read the module's own revision before
 * assuming either way.
 *
 * At 40 us the request-PAYLOAD phase intermittently out-ran the slave's
 * re-arm: the header transfer declared N bytes, the payload transfer landed
 * short or shifted, and the firmware rejected the frame with RESP_ERR_INVALID
 * because req_len != sizeof(alp_cc3501e_sock_send_t) + data_len -- surfacing
 * to the caller as `send failed (-1)` on a transfer the peer had in fact
 * received in full (alp-sdk#1746, cc3501e-bridge-firmware#90).
 *
 * Bench-measured on E1M-AEN801 serial 2617-0001 (board rev r1), protocol 7,
 * bare accept() listener, 10 consecutive `alp companion sock tcp-get`:
 *     40 us  -> 6/10 end-to-end, 4/10 `send failed (-1)`
 *    250 us  -> 10/10 end-to-end, 0 failures
 *
 * COST: this gap is paid per payload phase, so it is a throughput tax on
 * anything that streams (OTA_WRITE's 260 B chunks, bulk socket sends) -- see
 * #1677 before treating any throughput number taken at 40 us as comparable.
 * The value is empirical, not derived; the real fix is a readable READY line,
 * which needs a board revision that does not put it on SPI1_SCLK_A. */
#define CC3501E_PHASE_SETTLE_US 250u

/* READY gate for the r2 SS0 + host-IRQ bridge.  When ctx->ready_pin is
 * populated (the CC35 GPIO17 -> Alif P2_6 line is wired + opened as an input),
 * wait for it HIGH -- the slave drives it HIGH when its SPI slave is armed+idle
 * -- before clocking a reply phase, instead of a fixed settle gap.  This tracks
 * the slave's actual re-arm rather than guessing, so slow (Wi-Fi/BLE) replies
 * no longer need a conservative fixed delay.  Opt-in + degrades safely: a NULL
 * ready_pin (CS-less r1 boards) or a line that never asserts falls back to the
 * fixed gap.  See project_cc3501e_link_topology. */

/* Latched the first time READY is ever observed HIGH.  Until that happens -- and
 * permanently on a board that never asserts it -- the gate keeps its historical
 * behaviour (short burst, then the fixed gap), so an unwired line cannot stall
 * every phase.  Once the line has proven itself wired AND driven, the gate
 * becomes AUTHORITATIVE and waits for the slave to re-arm however long the
 * device op takes.  That is the difference between surviving a flash blackout
 * and clocking into a dead slave: a burst of 64 reads is ~microseconds, while a
 * psa_fwu erase/write is orders of magnitude longer. */
static bool g_ready_line_proven;

/* Set while the peer is in OTA update mode (polled slave).  A polled slave only
 * re-arms when its service loop next enters SPI_transfer, so READY is still HIGH
 * for a moment AFTER a phase completes.  A LEVEL gate therefore returns
 * immediately and the host clocks the next phase into a slave that is not
 * listening -- which is why header-only frames (OTA_STATUS) worked while every
 * payload-bearing OTA_WRITE returned -5 and the stream died at off=256 (silicon
 * 2026-08-21).  With this set the gate waits for a LOW->HIGH EDGE instead, i.e.
 * for the slave to actually drop and re-raise READY around its re-arm. */
static bool g_peer_polled;

bool cc3501e_peer_is_polled(void)
{
	return g_peer_polled;
}

void cc3501e_set_peer_polled(bool on)
{
	g_peer_polled = on;
}

/* Authoritative-wait budget once the line is proven.  This bounds ONE PHASE, and
 * its only job is to not clock into a slave that has not re-armed -- a re-arm is
 * microseconds.  It must NOT be sized to outlast a device-side flash blackout:
 * waiting out a multi-minute psa_fwu erase is the CALLER's hold-off loop's job.
 * Sizing this at 5 s was measured on silicon 2026-08-21 to make a single
 * 4-phase cc3501e_ota_status cost up to 20 s while the caller's budget charged
 * it 250 ms, turning a nominal 600 s BEGIN confirmation into a ~13 HOUR wait
 * that read as "BEGIN never returns". */
#define CC3501E_READY_WAIT_US 250000u
#define CC3501E_READY_POLL_US 200u
/* How long to wait for a polled peer to DROP ready before giving up on the
 * edge and treating the line as level-only. */
#define CC3501E_READY_EDGE_US 20000u
/* Tight spin for the READY drop; see cc3501e_reply_gate(). */
#define CC3501E_READY_LOW_SPINS 64u

/* A POLLED slave (OTA update mode) re-arms only when its service loop next enters
 * SPI_transfer -- microseconds of processing, not an ISR -- so the host's fallback
 * settle has to cover that.  200 us is right for the DMA slave and too short here:
 * header-only frames survived while every payload-bearing OTA_WRITE lost its bytes
 * and the stream died at off=256 (silicon 2026-08-21).  Widening the settle for
 * EVERY peer regressed update-mode ENTRY to -4 (that handshake runs on the ordinary
 * DMA bridge), so it is scoped to polled peers only.  Independent of READY, which
 * is not readable on this bench (READY probe: rc=0 level=0). */
/* WAS 5000u.  That value was sized against a symptom, not a cause: "every
 * payload-bearing OTA_WRITE lost its bytes and the stream died at off=256" is
 * exactly the polled RX-FIFO desync that spi_fifo_reset() (firmware
 * transport_hw_ti_spi.c) now clears at the start of EVERY frame.  With the cause
 * fixed, this gate is back to doing only its stated job -- covering the slave's
 * re-arm, which this file's own comment describes as "microseconds".  At 5000us
 * it cost 4 gates x 5 ms = 20 ms per frame, ~2 frames per 256 B chunk. */
#define CC3501E_POLLED_SETTLE_US 200u

static void cc3501e_reply_gate(const cc3501e_t *ctx, uint32_t fallback_us)
{
	if (g_peer_polled && fallback_us < CC3501E_POLLED_SETTLE_US) {
		fallback_us = CC3501E_POLLED_SETTLE_US;
	}
	if (ctx->ready_pin != NULL) {
		bool           level     = false;
		const uint32_t budget_us = g_ready_line_proven ? CC3501E_READY_WAIT_US : 0u;
		uint32_t       waited_us = 0u;
		if (g_ready_line_proven) {
			/* Edge, not level -- and NOT only in polled mode.  The slave drops
			 * READY in its transfer-complete ISR and raises it again once the
			 * NEXT phase is armed.  Sampling the level alone races that drop and
			 * returns on the STALE high, so the host clocks into an un-armed
			 * slave.  Bench 2026-08-24: the moment P2_6 became readable the
			 * level-only gate returned with zero delay and the link fell to
			 * 21-32 good soak PINGs; with this edge wait the same build runs
			 * ping_fail=0 over 441 PINGs.
			 *
			 * A fixed spin count, not a CC3501E_READY_POLL_US (200 us) ladder:
			 * a slave that re-armed before we looked must cost ~nothing rather
			 * than the full CC3501E_READY_EDGE_US bound.  The count is WALL-TIME
			 * sensitive -- it was retuned to 160 when the same build ran on the
			 * 400 MHz M55-HP, and 8 is too few even at 160 MHz. */
			for (uint32_t i = 0; i < CC3501E_READY_LOW_SPINS; ++i) {
				if (alp_gpio_read(ctx->ready_pin, &level) == ALP_OK && !level) {
					break;
				}
			}
		}
		for (;;) {
			/* Opportunistic burst: catches an already-armed slave with no delay. */
			for (uint32_t i = 0; i < 64u; ++i) {
				if (alp_gpio_read(ctx->ready_pin, &level) == ALP_OK && level) {
					g_ready_line_proven = true;
					return;
				}
			}
			if (waited_us >= budget_us) {
				break;
			}
			alp_delay_us(CC3501E_READY_POLL_US);
			waited_us += CC3501E_READY_POLL_US;
		}
	}
	alp_delay_us(fallback_us);
}

/* The actual 4-phase exchange, WITHOUT taking ctx's transport lock -- the
 * caller must already hold it.  Split out of cc3501e_request() (issue
 * #1116 follow-up) so poll_by_repeat() below can bracket its OWN extra
 * ctx->rx_scratch[0] sentinel write/peek in the SAME lock acquisition as
 * the request itself, instead of leaving them either side of a
 * lock-acquire-per-call boundary where a second caller's request could
 * interleave with the sentinel write/peek even though every individual
 * cc3501e_request() call is itself now atomic. */
static alp_status_t cc3501e_request_locked(cc3501e_t        *ctx,
                                           alp_cc3501e_cmd_t cmd,
                                           const uint8_t    *tx_payload,
                                           size_t            tx_len,
                                           uint8_t          *rx_buf,
                                           size_t            rx_cap,
                                           size_t           *rx_len,
                                           uint8_t           req_seq)
{
	alp_status_t s;

	/*
     * 3-wire deterministic framing (this HW rev wires only SCLK/MOSI/MISO
     * -- no CS, no host IRQ; CS + IRQ arrive next rev).  Each transfer's
     * length is derived from a header already exchanged, so master + slave
     * stay in lockstep without a CS edge.  Matches the firmware SPI-slave
     * state machine in cc3501e-bridge-firmware:hal/ti/transport_hw_ti_spi.c.
     *
     *   1. send request header (4)        3. read reply header (4)
     *   2. send request payload (tx_len)  4. read reply payload (status+data)
     */
	/* Gate READY before the REQUEST header too.  After the slave sends a reply it
	 * re-arms its header phase in its ISR; a spaced request (soak loop, bring-up)
	 * has ample idle time so the header always landed on an armed slave.  But a
	 * TIGHT back-to-back loop -- streaming via cc3501e_stream_write -- clocks the
	 * next header the instant the prior reply is read, racing that re-arm: the
	 * first frame acks, then every following frame desyncs (bench 2026-07-04:
	 * dma_stream_iters stuck at 1).  READY tracks the actual header-arm; on a
	 * CS-less r1 board with no ready_pin the fallback is the same short settle the
	 * other phases use. */
	cc3501e_reply_gate(ctx, CC3501E_PHASE_SETTLE_US);
	/* req_seq rides flags bits 3..7 (proto v8).  Callers that have no retry
	 * loop pass ALP_CC3501E_REQ_SEQ_NONE, which the firmware treats as "no
	 * identity" and never latches -- so the masking here is the only place
	 * the seq touches the wire, and a v7 firmware simply ignores the bits. */
	const uint8_t flags =
	    (uint8_t)(ALP_CC3501E_FLAG_RESP_REQUIRED | (uint8_t)((req_seq & ALP_CC3501E_REQ_SEQ_MASK)
	                                                         << ALP_CC3501E_FLAG_REQ_SEQ_SHIFT));
	/* v4.0 (#2035): append a CRC-16/CCITT-FALSE trailer to the REQUEST too,
	 * but ONLY once this ctx has negotiated fw_proto_major 4 -- a request
	 * built before the gate has run (fw_proto_major 0) or against a
	 * fw_proto_major-3 peer stays BYTE-IDENTICAL to 3.1, no trailer, because
	 * legacy firmware validates `HEADER + payload_len == req_len` per-handler
	 * and rejects an unexpected trailer with RESP_ERR_INVALID (see the
	 * migration-order note in <alp/protocol/cc3501e.h>). */
	const bool     want_req_crc = (ctx->fw_proto_major >= (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR);
	const uint16_t wire_tx_len =
	    (uint16_t)(tx_len + (want_req_crc ? (size_t)ALP_CC3501E_CRC_BYTES : 0u));
	encode_header(ctx->tx_scratch, cmd, flags, wire_tx_len);
	s = alp_spi_transceive(ctx->bus, ctx->tx_scratch, ctx->rx_scratch, ALP_CC3501E_HEADER_BYTES);
	if (s != ALP_OK) goto out;

	/* IN-BAND ARMED CHECK -- the software stand-in for the READY line.
	 *
	 * An armed slave drives ALP_CC3501E_SYNC_IDLE (0xA5) on MISO for the whole
	 * request-header phase (arm_request_header -> arm_transfer(..., sync_idle,
	 * ...)).  The host already clocks those 4 bytes; it just used to throw the
	 * MISO data away and carry on into the payload phases regardless.
	 *
	 * That is what INDUCES the desync.  If the slave has not re-armed yet it is
	 * not listening, so the header lands nowhere -- and then the host shoves up
	 * to 512 payload bytes at a slave that is mid-arm, which shifts it and
	 * corrupts every following frame until it happens to realign.
	 *
	 * Silicon-measured 2026-08-24 over one socket stream: with the marker
	 * present, 438 transactions and only 28 header failures (93.6% good); with it
	 * absent, 1808 transactions and 1808 failures -- 100%.  It is a perfect
	 * predictor, and it is free.
	 *
	 * So: stop at the header.  Report the SAME ALP_ERR_IO the bad-header path
	 * below already reports, so caller behaviour is unchanged -- this just
	 * reaches that verdict one aligned 4-byte transfer earlier, without having
	 * clocked payload into an unarmed slave.
	 *
	 * This matters because READY (CC35 GPIO17 -> Alif P2_6) is an OPEN CONNECTION
	 * on the bench unit -- 0 edges in 20000 samples taken during live traffic --
	 * so the fixed inter-phase settles are the only other interlock, and they
	 * cannot be tightened (250 us desyncs the link outright). */
	if (ctx->rx_scratch[0] != ALP_CC3501E_SYNC_IDLE ||
	    ctx->rx_scratch[1] != ALP_CC3501E_SYNC_IDLE ||
	    ctx->rx_scratch[2] != ALP_CC3501E_SYNC_IDLE ||
	    ctx->rx_scratch[3] != ALP_CC3501E_SYNC_IDLE) {
		s = ALP_ERR_IO;
		goto out;
	}
	if (wire_tx_len > 0) {
		/* Inter-phase settle (CS-less lockstep): the slave arms the request-PAYLOAD
		 * transfer in its SPI ISR only AFTER the header transfer completes.  Clocking
		 * the payload back-to-back (no gap) races that re-arm -> the payload bytes are
		 * dropped + the frame desyncs.  Header-only 3.1-shaped requests (PING / the
		 * argless worker ops) have no payload phase so they were fine; payload
		 * requests (OTA_WRITE, CONNECT, GPIO_WRITE) need this gap (root-caused on
		 * silicon 2026-06-19, where OTA streaming timed out per-chunk without it).
		 * Under a fw_proto_major-4 peer EVERY request has a payload phase now (at
		 * minimum the 2-byte CRC trailer, want_req_crc above), so this gate applies
		 * unconditionally on that wire even to what used to be argless.
		 *
		 * This was the ONE phase still on a bare fixed delay while phases 1, 3 and 4
		 * consult READY.  It is also the phase that clocks the most bytes (a 260 B
		 * OTA_WRITE), so it is the worst one to send blind at a slave that has not
		 * re-armed.  Gate it like the others; the delay stays as the fallback. */
		cc3501e_reply_gate(ctx, CC3501E_PHASE_SETTLE_US);
		const uint8_t *tx_ptr = tx_payload;
		if (want_req_crc) {
			/* Build payload+CRC as one contiguous buffer.  ctx->tx_scratch[0..3]
			 * still holds the header this exchange just sent -- alp_spi_transceive
			 * does not touch its own TX buffer -- so the CRC is computed from it
			 * BEFORE this reuses the same scratch to carry the payload phase. */
			uint16_t crc = alp_crc16_ccitt_false(ctx->tx_scratch, ALP_CC3501E_HEADER_BYTES);
			crc          = alp_crc16_ccitt_false_update(crc, tx_payload, tx_len);
			if (tx_len > 0) memcpy(ctx->tx_scratch, tx_payload, tx_len);
			ctx->tx_scratch[tx_len]      = (uint8_t)(crc & 0xFFu);
			ctx->tx_scratch[tx_len + 1u] = (uint8_t)((crc >> 8) & 0xFFu);
			tx_ptr                       = ctx->tx_scratch;
		}
		s = alp_spi_transceive(ctx->bus, tx_ptr, ctx->rx_scratch, wire_tx_len);
		if (s != ALP_OK) goto out;
	}

	/* Wait for the slave to dispatch + arm its reply before we read: the
	 * READY gate tracks it via the host-IRQ line when wired, else a fixed gap. */
	cc3501e_reply_gate(ctx, 200u);

	/* Dummies for the read transactions (MOSI is don't-care on a read). */
	memset(ctx->tx_scratch, 0xFF, sizeof(ctx->tx_scratch));

	/* 3. Reply header -> learn the reply payload length. */
	s = alp_spi_transceive(ctx->bus, ctx->tx_scratch, ctx->rx_scratch, ALP_CC3501E_HEADER_BYTES);
	if (s != ALP_OK) goto out;
	uint16_t resp_payload_len = decode_header_payload_len(ctx->rx_scratch);
	/* Desync detection (no CS to recover on): a valid reply header ECHOES the
     * request opcode (protocol_build_reply sets reply[0]=cmd) and declares a
     * payload in [1..MAX].  An all-0xA5 header means the slave is parked at a
     * frame boundary (we were misaligned); any other mismatch is lockstep
     * drift.  Either way, re-establish byte alignment so the NEXT request lands
     * clean, and report IO so the caller retries (the soak/bring-up loops do). */
	const bool hdr_ok = (ctx->rx_scratch[0] == (uint8_t)cmd) && (resp_payload_len >= 1u) &&
	                    (resp_payload_len <= ALP_CC3501E_MAX_PAYLOAD);
	if (!hdr_ok) {
		/* Do NOT byte-walk cc3501e_sync() here: on the 4-byte fixed-count lockstep
		 * the 1-byte walk PARKS the slave (proven on silicon -- reply_hdr stuck at
		 * 0xA5A5A5A5, link never recovers).  Return IO so the caller re-issues a
		 * clean 4-byte transaction instead (re-aligns when the slave is aligned). */
		s = ALP_ERR_IO;
		goto out;
	}
	/* v4.0 (#2035): save the reply's own header bytes NOW -- the payload-phase
	 * transceive below reuses ctx->rx_scratch and overwrites them -- so
	 * cc3501e_reply_verdict() can still CRC the header + payload together
	 * afterwards (span per <alp/protocol/cc3501e.h>: the 4 header bytes plus
	 * every payload byte except the trailing 2 CRC bytes). */
	uint8_t reply_hdr[ALP_CC3501E_HEADER_BYTES];
	memcpy(reply_hdr, ctx->rx_scratch, ALP_CC3501E_HEADER_BYTES);

	/* Same READY gate before the reply PAYLOAD phase (the slave re-arms it in
	 * its ISR only after the reply-header transfer completes). */
	cc3501e_reply_gate(ctx, CC3501E_PHASE_SETTLE_US);
	/* 4. Reply payload: status byte followed by the response data. */
	s = alp_spi_transceive(ctx->bus, ctx->tx_scratch, ctx->rx_scratch, resp_payload_len);
	if (s != ALP_OK) goto out;

	{
		const size_t data_len = (size_t)resp_payload_len - 1u;
		if (data_len > 0u && rx_buf != NULL) {
			const size_t n = (data_len > rx_cap) ? rx_cap : data_len;
			memcpy(rx_buf, &ctx->rx_scratch[1], n);
			if (rx_len != NULL) *rx_len = n;
		}
		/* v4.0 (#2035): the actual verdict -- major-aware status decode, the
		 * CRC check on a fw_proto_major-4 wire, and the #1378 dead-phase
		 * guard -- is cc3501e_reply_verdict(), a pure function with no I/O so
		 * it is independently unit-testable (see cc3501e_internal.h). */
		s = cc3501e_reply_verdict(
		    cmd, ctx->fw_proto_major, reply_hdr, ctx->rx_scratch, resp_payload_len);
	}

out:
	return s;
}

alp_status_t cc3501e_request(cc3501e_t        *ctx,
                             alp_cc3501e_cmd_t cmd,
                             const uint8_t    *tx_payload,
                             size_t            tx_len,
                             uint8_t          *rx_buf,
                             size_t            rx_cap,
                             size_t           *rx_len,
                             uint32_t          timeout_ms)
{
	(void)timeout_ms; /* Reserved for a future IRQ-driven wait (next HW rev). */
	if (rx_len != NULL) *rx_len = 0;
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	/* MAJOR-4 fix: the request-build below (cc3501e_request_locked()'s
	 * want_req_crc) appends a 2-byte CRC trailer once this ctx has negotiated
	 * fw_proto_major 4, so wire_tx_len there becomes tx_len +
	 * ALP_CC3501E_CRC_BYTES -- a tx_len this check let through all the way up
	 * to ALP_CC3501E_MAX_PAYLOAD would encode a wire length LARGER than the
	 * protocol's own maximum into the header's length field.  Tighten the
	 * ceiling by the trailer size whenever it is actually going to be
	 * appended, matching cc3501e_ble_write_descriptor()'s own
	 * ALP_CC3501E_MAX_PAYLOAD bound at chips/cc3501e/cc3501e_ble.c -- that
	 * bound is now wrong against a major-4 peer without this. */
	const uint16_t max_tx = (ctx->fw_proto_major >= (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR)
	                            ? (uint16_t)(ALP_CC3501E_MAX_PAYLOAD - ALP_CC3501E_CRC_BYTES)
	                            : (uint16_t)ALP_CC3501E_MAX_PAYLOAD;
	if (tx_len > max_tx) return ALP_ERR_INVAL;
	if (tx_payload == NULL && tx_len > 0) return ALP_ERR_INVAL;

	/* Serialise the whole exchange (issue #1116): every phase in
	 * cc3501e_request_locked() reads or writes ctx->tx_scratch /
	 * ctx->rx_scratch, shared by every caller of this ctx.  Acquired AFTER
	 * the pure param checks above (so a bad call fails fast without
	 * touching the lock). */
	alp_status_t s = cc3501e_lock_acquire(ctx);
	if (s != ALP_OK) return s;
	/* Single-shot: no retry loop, so nothing here is a repeat of anything and
	 * the frame must not be latchable.  ALP_CC3501E_REQ_SEQ_NONE says exactly
	 * that on the wire (proto v8).  Retryable callers go through
	 * poll_by_repeat(), which allocates a real seq. */
	s = cc3501e_request_locked(
	    ctx, cmd, tx_payload, tx_len, rx_buf, rx_cap, rx_len, ALP_CC3501E_REQ_SEQ_NONE);
	cc3501e_lock_release(ctx);
	return s;
}

alp_status_t cc3501e_get_version(cc3501e_t *ctx, uint16_t *version_out)
{
	if (version_out == NULL) return ALP_ERR_INVAL;
	uint8_t      reply[2] = { 0 };
	size_t       got      = 0;
	alp_status_t s =
	    cc3501e_request(ctx, ALP_CC3501E_CMD_GET_VERSION, NULL, 0, reply, sizeof(reply), &got, 100);
	if (s != ALP_OK) return s;
	if (got < sizeof(reply)) return ALP_ERR_IO;
	*version_out = (uint16_t)reply[0] | ((uint16_t)reply[1] << 8);
	return ALP_OK;
}

alp_status_t cc3501e_get_capabilities(cc3501e_t *ctx, uint32_t *caps_out)
{
	if (caps_out == NULL) return ALP_ERR_INVAL;
	*caps_out = 0u;

	/* Reply DATA = alp_cc3501e_capabilities_t { caps(LE32) | reserved(LE32) }.
	 * Read only the first word: a future firmware may put more in `reserved`,
	 * and ignoring what we do not understand is exactly what makes widening it
	 * a MINOR-class change (ADR 0033). */
	uint8_t      reply[sizeof(alp_cc3501e_capabilities_t)] = { 0 };
	size_t       got                                       = 0;
	alp_status_t s = cc3501e_request(ctx,
	                                 ALP_CC3501E_CMD_GET_CAPABILITIES,
	                                 NULL,
	                                 0,
	                                 reply,
	                                 sizeof(reply),
	                                 &got,
	                                 CC3501E_REQ_TMO_MS);
	if (s != ALP_OK) return s;
	if (got < 4u) return ALP_ERR_IO; /* short reply -- firmware/wire gap */
	*caps_out = (uint32_t)reply[0] | ((uint32_t)reply[1] << 8) | ((uint32_t)reply[2] << 16) |
	            ((uint32_t)reply[3] << 24);
	return ALP_OK;
}

alp_status_t cc3501e_stream_write(cc3501e_t *ctx, const uint8_t *data, size_t len)
{
	if (ctx == NULL || (data == NULL && len > 0u)) return ALP_ERR_INVAL;
	if (len > (size_t)(ALP_CC3501E_MAX_PAYLOAD - ALP_CC3501E_HEADER_BYTES)) {
		return ALP_ERR_INVAL;
	}
	/* One framed bulk frame: the request PAYLOAD phase clocks @len bytes in a
	 * single transfer, which takes the host DMA path when @len >= the SPI DMA
	 * threshold (CONFIG_SPI_DW_ALIF_DMA_MIN_LEN).  The firmware sinks + acks it,
	 * so the link stays framed -- send these back-to-back for a bulk stream. */
	return cc3501e_request(ctx, ALP_CC3501E_CMD_STREAM_WRITE, data, len, NULL, 0u, NULL, 200u);
}

/* Poll-by-repeat backoff: how long to wait between BUSY repeats.
 *
 * This was a FLAT 50 ms, and it is the single biggest cost on every
 * worker-routed op.  The firmware's worker model is submit-then-collect: the
 * dispatch runs in the SPI callback (SWI/HWI context) and cannot call the radio
 * or IP stacks, so handle_worker_routed_* ALWAYS answers RESP_ERR_BUSY to the
 * submit and the host must come back for the result.  A flat gap therefore
 * charges 50 ms to every such op no matter how fast the worker actually
 * finished -- and most finish in well under a millisecond.  Measured on
 * silicon: a 487 B CMD_SOCK_RECV (one frame is capped at
 * MAX_PAYLOAD - recv_resp header - status = 487 B) costs two round trips plus
 * one gap, i.e. ~50 ms, which is ~9.7 kB/s against a 14 MHz link.
 *
 * So START short and BACK OFF exponentially to the old ceiling.  The ceiling
 * matters: cc3501e_ota_update's flush hold-off polls THROUGH a flash blackout,
 * where the device answers from an ISR the flash op has stopped, so every frame
 * clocked in that window goes into a dead slave.  Backing off to 50 ms keeps
 * the blackout frame count essentially unchanged (a 600 s hold-off gains ~6
 * extra frames in total) while collecting a ready result in ~1 ms. */
#define CC3501E_POLL_GAP_MIN_MS 1u
#define CC3501E_POLL_GAP_MS     50u

alp_status_t poll_by_repeat(cc3501e_t        *ctx,
                            alp_cc3501e_cmd_t cmd,
                            const uint8_t    *tx_payload,
                            size_t            tx_len,
                            uint8_t          *rx_buf,
                            size_t            rx_cap,
                            size_t           *rx_len,
                            uint32_t          timeout_ms)
{
	/* Same param checks cc3501e_request() runs, done ONCE here (cmd/
	 * tx_payload/tx_len are fixed across every retry below, unlike the
	 * transport lock which is per-attempt) rather than calling the public
	 * wrapper per iteration -- see the sentinel-race comment below. */
	if (rx_len != NULL) *rx_len = 0;
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	/* Same MAJOR-4 ceiling as cc3501e_request() above -- see its comment. */
	const uint16_t max_tx = (ctx->fw_proto_major >= (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR)
	                            ? (uint16_t)(ALP_CC3501E_MAX_PAYLOAD - ALP_CC3501E_CRC_BYTES)
	                            : (uint16_t)ALP_CC3501E_MAX_PAYLOAD;
	if (tx_len > max_tx) return ALP_ERR_INVAL;
	if (tx_payload == NULL && tx_len > 0) return ALP_ERR_INVAL;

	/* Budget is coarse-grained in CC3501E_POLL_GAP_MS slices; always make at
	 * least one attempt even with a zero timeout. */
	uint32_t     remaining   = (timeout_ms > 0u) ? timeout_ms : 1u;
	uint32_t     next_gap_ms = CC3501E_POLL_GAP_MIN_MS;
	alp_status_t s;
	/* ONE seq for this whole logical command, allocated BEFORE the loop and
	 * re-sent unchanged on every attempt below -- that constancy is what lets
	 * the firmware answer a repeat from its latch instead of re-executing the
	 * operation (proto v8, cc3501e-bridge-firmware#102).  Allocating inside
	 * the loop would make every retry look like a new command, i.e. exactly
	 * the bug this exists to fix.
	 *
	 * Skips 0, which is reserved for "no identity": the space is 1..31 and
	 * this pre-increments, so a fresh ctx's first retryable command is seq 1
	 * (same shape as sock_send_seq / spi1_seq). */
	ctx->req_seq = (ctx->req_seq >= ALP_CC3501E_REQ_SEQ_LAST) ? 1u : (uint8_t)(ctx->req_seq + 1u);
	const uint8_t req_seq = ctx->req_seq;
	for (;;) {
		/* Sentinel + peek bracketed in the SAME lock hold as the request
		 * itself (issue #1116 follow-up): both touch the shared
		 * ctx->rx_scratch[0] this ctx's other callers can also write, so
		 * lock-acquire-per-cc3501e_request()-call alone isn't enough --
		 * a second caller's request could still land between this
		 * sentinel write and this attempt's own request, or between this
		 * request and the peek below, corrupting the disambiguation this
		 * retry loop depends on. */
		s = cc3501e_lock_acquire(ctx);
		if (s != ALP_OK) return s;
		/* Re-zero per attempt, not just once before the loop: an attempt
		 * that copied out n bytes and then mapped to BUSY/IO would
		 * otherwise leave that stale count visible to a caller who reads
		 * *rx_len after this function finally returns TIMEOUT. */
		if (rx_len != NULL) *rx_len = 0;
		/* Sentinel: pre-set rx_scratch[0] to a byte the peek below never
		 * matches (0xFF).  Only a real reply payload overwrites it with the
		 * resp byte; a BUSY that comes from the transport (alp_spi_transceive
		 * -EBUSY) rather than resp_to_status() then leaves the sentinel, so it
		 * can never masquerade as RESP_ERR_STATE. */
		ctx->rx_scratch[0] = 0xFFu;
		s = cc3501e_request_locked(ctx, cmd, tx_payload, tx_len, rx_buf, rx_cap, rx_len, req_seq);
		/* resp_to_status() maps BOTH RESP_ERR_BUSY (worker still running --
		 * genuinely retryable) and RESP_ERR_STATE (a deterministic firmware
		 * reject -- e.g. BLE_GATT_REGISTER's NimBLE ordering guard) to the
		 * same ALP_ERR_BUSY, since that is the correct final answer for
		 * both.  But only the FORMER is worth re-polling: retrying the
		 * latter just repeats the same reject until the budget is gone
		 * (register-while-advertising must fail promptly, not after burning
		 * the whole poll window).  Only a RESP_ERR_STATE reply writes 0x09
		 * into rx_scratch[0] (the sentinel above rules out a transport BUSY),
		 * so the peek disambiguates safely -- read here, still under the
		 * lock, not after release. */
		const bool terminal_reject =
		    (s == ALP_ERR_BUSY && ctx->rx_scratch[0] == ALP_CC3501E_RESP_ERR_STATE);
		cc3501e_lock_release(ctx);
		if (terminal_reject) {
			return s; /* terminal reject -- do not retry */
		}
		if (s != ALP_ERR_BUSY && s != ALP_ERR_IO) {
			return s; /* OK or a non-retryable error -- done. */
		}
		if (remaining == 0u) {
			return ALP_ERR_TIMEOUT;
		}
		uint32_t gap = (remaining < next_gap_ms) ? remaining : next_gap_ms;
		alp_delay_ms(gap);
		remaining -= gap;
		/* Double until the ceiling: fast for a result that is already staged,
		 * unchanged for a device that is genuinely away. */
		if (next_gap_ms < CC3501E_POLL_GAP_MS) {
			next_gap_ms =
			    (next_gap_ms * 2u > CC3501E_POLL_GAP_MS) ? CC3501E_POLL_GAP_MS : next_gap_ms * 2u;
		}
	}
}
