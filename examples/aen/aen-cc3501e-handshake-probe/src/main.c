/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-cc3501e-handshake-probe -- discriminates competing explanations for a
 * CC3501E link failure seen on real silicon today, for the E1M-AEN801 (Alif
 * Ensemble E8, M55-HE), bench RAM-run via J-Link.
 *
 * The failure this app exists to explain
 * ---------------------------------------
 * After the CC3501E coprocessor was reflashed to a wire-protocol-4.0 image
 * that requires a CRC-16/CCITT-FALSE trailer on every frame, phase 8 of
 * examples/aen/aen-evk-demo produced:
 *
 *   CC3501E: bridge bring-up (WIFI_EN high, nRESET pulsed, SPI1 @ 25000000 Hz) -> 0
 *   CC3501E: PING (0x00) -> -5 after 25 attempt(s) of 25 (200 ms apart)
 *
 * -5 is ALP_ERR_IO. It does not discriminate: resp_to_status() in
 * chips/cc3501e/cc3501e_core.c maps ALP_CC3501E_RESP_ERR_PROTOCOL (0x07) --
 * a CORRECTLY RECEIVED error reply -- onto the SAME ALP_ERR_IO a dead link
 * produces. The demo cannot tell them apart: it never prints
 * fw_proto_major, and it gates GET_VERSION behind a successful PING (so a
 * PING failure short-circuits the phase before GET_VERSION ever runs).
 *
 * Do NOT modify aen-evk-demo to add this instrumentation. Its binary is a
 * proven constant across three bench runs and that constancy is itself
 * evidence; this is a separate app for exactly that reason.
 *
 * A first version of THIS app was itself unsound (bench run + review,
 * 2026-09-10) and its lesson is why the code below looks the way it does:
 * it printed "HYPOTHESIS C (image not booting)" about a part that was
 * DEMONSTRABLY answering -- a human had to read ctx->rx_scratch over SWD by
 * hand after the run to find a structurally valid GET_DIAG_INFO reply
 * header (`04 00 08 00`: echoed opcode 0x04, declared payload length 8)
 * sitting in DTCM, which a dead/undriven link can never produce (that reads
 * as all 0x00 or all 0xFF). Every fix below traces back to that one failure:
 * this app must never again print a confident verdict that the wire itself
 * disproves, and it must never again need a human with a debugger to see
 * what the wire actually said.
 *
 * The states this app can now report
 * -------------------------------------
 * BRIDGE_ABSENT     -- step 1 says the SoM does not present the bridge at
 *                       all (no pins / SPI controller). Not a link fault.
 * VERSION_SKEW       -- cc3501e_reset()'s own GET_VERSION attempt got an
 *                       ANSWER, but with a major this host refuses (major 0
 *                       = pre-versioning-scheme firmware; anything else
 *                       outside {3, 4} = a real, unsupported skew). The
 *                       link is demonstrably alive; it is simply a version
 *                       this build will not talk further to.
 * HYPOTHESIS_C        -- bring-up succeeded, fw_proto_major never latched
 *                       (stayed 0), AND every wire dump collected during
 *                       this run looked undriven (all 0x00 or all 0xFF).
 *                       This is now the ONLY combination this app will call
 *                       "not booting/answering".
 * LINK_ALIVE_UNPARSED -- the wire showed unmistakable signs of life (a
 *                       parked-idle 0xA5 marker, or a structured candidate
 *                       header) but no step completed cleanly enough to
 *                       support any of the named hypotheses below. This is
 *                       the state the original bug's exact input set now
 *                       maps to -- see the walkthrough in this app's
 *                       README.
 * HYPOTHESIS_A        -- cc3501e_reset()'s own internal GET_VERSION missed
 *                       (fw_proto_major was still 0 after step 1), this
 *                       app's own direct GET_VERSION then got firmware
 *                       major 4, and the CRC-trailer PING that major
 *                       implies succeeded.
 * HYPOTHESIS_A_PLUS_B -- same precondition as A, but the CRC-trailer PING
 *                       STILL fails once major 4 is latched.
 * HYPOTHESIS_B        -- major 4 was ALREADY latched by step 1 (reset's own
 *                       GET_VERSION worked), yet the CRC-trailer PING
 *                       fails.
 * LEGACY_3_1_ACTIVE   -- firmware answers with major 3 (the pre-4.0 wire)
 *                       and PING succeeds against it: the link is fully
 *                       healthy, it is just still running the OLD image --
 *                       the OTA to 4.0 has not landed on this part yet.
 *                       Neither A nor B applies: major 3's PING has no
 *                       CRC-trailer payload phase at all.
 * HANDSHAKE_HEALTHY   -- major 4 latched, and the CRC-trailer PING against
 *                       it succeeds. Neither A nor B reproduces right now.
 *
 * See handshake_classify()'s own comment for the exact gates behind each of
 * these, and STEP 7's console line / this app's README for a worked
 * walkthrough of the ORIGINAL bug's own six numbers through this table.
 *
 * Build target: alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he, the same
 * target aen-evk-demo uses (including its SRAM0/DCACHE=n memory placement --
 * see the overlay's own header for why) -- see this app's README.md for the
 * full west build invocation. BENCH-VALIDATION app, not a customer teaching
 * example; never stops early on a failed step -- a failed step's own return
 * code IS the data this app exists to collect.
 */

#include <stdint.h>
#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/chips/cc3501e.h"
#include "cc3501e_bridge.h" /* cc3501e_bridge_bringup() -- the SoM bring-up template */

/* Bounded PING retry, matching aen-evk-demo's own CC35_PING_RETRIES /
 * CC35_PING_GAP_MS exactly (chips/cc3501e/cc3501e_core.c's file header
 * documents why the link needs this: the Puya-flash cold-boot workaround
 * can leave the coprocessor still settling for the first several requests
 * after nRESET releases). A single PING attempt cannot tell "the shape is
 * broken" (hypothesis B) from "the slave had not finished booting yet
 * during this one attempt" -- this app exists to discriminate hypotheses,
 * so it must not let a transient miss decide one for it. */
#define HANDSHAKE_PING_RETRIES 16u
#define HANDSHAKE_PING_GAP_MS  320u

/* 320, not the 200 this app first copied from the demo -- and the demo has
 * been moved to 320 too, so these still match.
 *
 * The gap MUST exceed the slave's reply-stall watchdog,
 * CC3501E_REPLY_STALL_MS = 250 in the bridge firmware's
 * hal/ti/transport_hw_ti_spi.c. That watchdog is the link's ONLY recovery
 * from a phase desync: there is no chip-select to resynchronise on, and
 * byte-walking to realign provably PARKS the slave (see cc3501e_sync()'s
 * warning in chips/cc3501e/cc3501e_core.c).
 *
 * At 200 ms every retry re-stamped the slave's deadline before it could
 * expire, so the retry loop STARVED the only self-heal the link has and a
 * one-off desync looked like a permanently dead part. Measured on silicon
 * 2026-09-10. Attempt count drops so the ~5 s bound is unchanged. */

/*
 * ---------------------------------------------------------------------
 * Wire-shape classification -- the fix for the ORIGINAL bug (see file
 * header). Dumped after every step that fails, this is what would have
 * shown "04 00 08 00" in the console transcript itself instead of needing
 * a human to read DTCM over SWD afterwards.
 * ---------------------------------------------------------------------
 */
typedef enum {
	WIRE_SHAPE_UNDRIVEN_LOW,  /* all 0x00 -- nothing is driving MISO */
	WIRE_SHAPE_UNDRIVEN_HIGH, /* all 0xFF -- nothing is driving MISO, opposite idle level */
	WIRE_SHAPE_PARKED_IDLE,   /* all ALP_CC3501E_SYNC_IDLE (0xA5) -- the slave IS armed and
	                           * driving the bus, just parked at a frame boundary (the
	                           * in-band armed check in cc3501e_request_locked()) */
	WIRE_SHAPE_STRUCTURED,    /* anything else -- real, non-uniform data; the slave answered
	                           * SOMETHING, whatever this call's own return code says */
} wire_shape_t;

static wire_shape_t wire_shape_classify(const uint8_t *hdr4)
{
	bool zero = true, ff = true, idle = true;
	for (int i = 0; i < 4; i++) {
		if (hdr4[i] != 0x00u) zero = false;
		if (hdr4[i] != 0xFFu) ff = false;
		if (hdr4[i] != (uint8_t)ALP_CC3501E_SYNC_IDLE) idle = false;
	}
	if (zero) return WIRE_SHAPE_UNDRIVEN_LOW;
	if (ff) return WIRE_SHAPE_UNDRIVEN_HIGH;
	if (idle) return WIRE_SHAPE_PARKED_IDLE;
	return WIRE_SHAPE_STRUCTURED;
}

/** Dump fw->rx_scratch/tx_scratch[0..7] as hex, plus a best-effort decode of
 *  the first 4 rx bytes as a candidate reply header. The length rule
 *  (frame[2] | frame[3] << 8) is duplicated from decode_header_payload_len()
 *  in chips/cc3501e/cc3501e_core.c, which is file-static and not part of the
 *  public API -- this is the SAME rule, not a reimplementation of anything
 *  else that function does.
 *
 *  Called after any step that fails. This is the single most valuable thing
 *  this app can print: it is what a human had to read over SWD by hand on
 *  2026-09-10 to learn that a run this app itself called "image not
 *  booting" had, in fact, echoed a perfectly structured GET_DIAG_INFO reply
 *  header. Returns the wire shape so the caller can fold it into the
 *  decision table (see handshake_classify()). */
static wire_shape_t dump_wire_bytes(const char *step_label, const cc3501e_t *fw)
{
	wire_shape_t shape         = wire_shape_classify(fw->rx_scratch);
	uint8_t      echoed_opcode = fw->rx_scratch[0];
	uint16_t     declared_len  = (uint16_t)fw->rx_scratch[2] | ((uint16_t)fw->rx_scratch[3] << 8);

	printk("  %s wire bytes: rx=%02x %02x %02x %02x %02x %02x %02x %02x  "
	       "tx=%02x %02x %02x %02x %02x %02x %02x %02x\n",
	       step_label,
	       (unsigned)fw->rx_scratch[0],
	       (unsigned)fw->rx_scratch[1],
	       (unsigned)fw->rx_scratch[2],
	       (unsigned)fw->rx_scratch[3],
	       (unsigned)fw->rx_scratch[4],
	       (unsigned)fw->rx_scratch[5],
	       (unsigned)fw->rx_scratch[6],
	       (unsigned)fw->rx_scratch[7],
	       (unsigned)fw->tx_scratch[0],
	       (unsigned)fw->tx_scratch[1],
	       (unsigned)fw->tx_scratch[2],
	       (unsigned)fw->tx_scratch[3],
	       (unsigned)fw->tx_scratch[4],
	       (unsigned)fw->tx_scratch[5],
	       (unsigned)fw->tx_scratch[6],
	       (unsigned)fw->tx_scratch[7]);
	switch (shape) {
	case WIRE_SHAPE_UNDRIVEN_LOW:
		printk("    -> UNDRIVEN (0x00 x4): consistent with an absent/dead/unpowered slave\n");
		break;
	case WIRE_SHAPE_UNDRIVEN_HIGH:
		printk("    -> UNDRIVEN (0xFF x4): same meaning as all-zero, opposite idle level\n");
		break;
	case WIRE_SHAPE_PARKED_IDLE:
		printk("    -> PARKED IDLE (0x%02x x4 = ALP_CC3501E_SYNC_IDLE): the slave IS armed "
		       "and driving the bus, just at a frame boundary -- ALIVE, whatever this "
		       "call's own return code says\n",
		       (unsigned)ALP_CC3501E_SYNC_IDLE);
		break;
	case WIRE_SHAPE_STRUCTURED:
	default:
		printk("    -> STRUCTURED: candidate reply header echoes opcode 0x%02x, declares "
		       "payload length %u -- real data on the wire, whatever this call's own "
		       "return code says\n",
		       (unsigned)echoed_opcode,
		       (unsigned)declared_len);
		break;
	}
	return shape;
}

/** Human-readable name for the request dialect a given fw_proto_major frames
 *  against -- printed alongside STEP 5's PING so a reader does not have to
 *  reconstruct want_req_crc's own gate (cc3501e_core.c) by hand. */
static const char *dialect_name(uint8_t major)
{
	if (major == (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR) {
		return "v4.0 CRC-16/CCITT-FALSE trailer request shape";
	}
	if (major == (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR_LEGACY) {
		return "legacy v3.1 shape -- no request payload phase at all";
	}
	if (major == 0u) {
		return "major 0 (not yet negotiated) -- want_req_crc defaults this CRC-less";
	}
	return "an unrecognised major -- want_req_crc only adds the trailer at "
	       "major >= ALP_CC3501E_PROTOCOL_MAJOR, so this still goes out CRC-less";
}

/*
 * ---------------------------------------------------------------------
 * The verdict decision table.
 *
 * This is the SECOND version of this table. The first called a
 * demonstrably-alive part "not booting" because it gated hypothesis C on
 * nothing but step 3's own return code -- see the file header for the full
 * story. Every gate below exists to close one specific way that happened:
 *
 *   1. C now ALSO requires the bring-up to have succeeded AND the wire to
 *      have looked undriven on every dump collected this run -- not just
 *      "step 3 failed". A part that answers with a structured or
 *      parked-idle header, however this app's own gates score the rest of
 *      the exchange, is provably alive and can never be C.
 *   2. The major is keyed on {0, 3, 4, other}, not just "4 vs everything
 *      else". Major 3 is a fully legitimate, fully usable dialect (the
 *      host is deliberately bilingual, see ALP_CC3501E_PROTOCOL_MAJOR_LEGACY
 *      in <alp/protocol/cc3501e.h>) -- a part still on 3.1 firmware (the
 *      reflash to 4.0 did not take) gets its own state instead of being
 *      silently folded into "major 0 means dead".
 *   3. ver_major (the number step 3 actually decoded) drives the major-
 *      keyed branch below directly, instead of being collected and thrown
 *      away.
 *   4. LINK_ALIVE_UNPARSED exists for exactly the case the original bug
 *      produced: the wire showed life, but neither the version gate nor
 *      the ping gate can support a specific named hypothesis about it.
 *
 * Inputs:
 *   bringup_rc      -- step 1's own return code. ALP_ERR_NOT_PRESENT_ON_
 *                       THIS_SOC and ALP_ERR_VERSION are handled here
 *                       directly (both are conclusive on their own; neither
 *                       needs steps 3-6's data, which is why main() skips
 *                       running them on these two paths).
 *   step2_major     -- fw_proto_major exactly as cc3501e_reset() (step 1)
 *                       left it, read BEFORE this app's own step 3 can
 *                       touch it. 0 means reset's own internal GET_VERSION
 *                       round trip missed.
 *   step3_ok         -- did this app's own direct cc3501e_get_version()
 *                       (step 3) complete?
 *   ver_major        -- the major THIS app's own step 3 decoded (only
 *                       meaningful when step3_ok).
 *   ping_ok          -- did the bounded PING retry (step 5) succeed?
 *   link_shows_life  -- true if ANY wire dump collected anywhere in this
 *                       run (steps 3, 5, 6a, 6b) classified as
 *                       WIRE_SHAPE_PARKED_IDLE or WIRE_SHAPE_STRUCTURED.
 * ---------------------------------------------------------------------
 */
typedef enum {
	VERDICT_BRIDGE_ABSENT,
	VERDICT_VERSION_SKEW,
	VERDICT_HYPOTHESIS_C,
	VERDICT_LINK_ALIVE_UNPARSED,
	VERDICT_HYPOTHESIS_A,
	VERDICT_HYPOTHESIS_A_PLUS_B,
	VERDICT_HYPOTHESIS_B,
	VERDICT_LEGACY_3_1_ACTIVE,
	VERDICT_HANDSHAKE_HEALTHY,
} handshake_verdict_t;

static handshake_verdict_t handshake_classify(bool         probed,
                                              alp_status_t bringup_rc,
                                              uint8_t      step2_major,
                                              bool         step3_ok,
                                              uint8_t      ver_major,
                                              bool         ping_ok,
                                              bool         link_shows_life)
{
	/* @p probed is exactly main()'s own "did steps 3-6 run at all" decision -- tying the
	 * table to that flag directly (rather than re-deriving it here from bringup_rc's
	 * value) means a FUTURE bringup_rc this switch has never seen cannot silently fall
	 * through into the steps-3-6 logic below and read step2_major/link_shows_life as if
	 * they meant something on a run that never touched the wire. */
	if (!probed) {
		return (bringup_rc == ALP_ERR_NOT_PRESENT_ON_THIS_SOC) ? VERDICT_BRIDGE_ABSENT
		                                                       : VERDICT_VERSION_SKEW;
	}

	/* From here bring-up succeeded (ALP_OK): the bridge exists, cc3501e_reset() ran to
	 * completion, and ctx->initialised is true whatever the major ended up being. */
	if (!step3_ok) {
		if (step2_major == 0u && !link_shows_life) return VERDICT_HYPOTHESIS_C;
		return VERDICT_LINK_ALIVE_UNPARSED;
	}

	/* step 3 succeeded: ver_major is a REAL, freshly-read major, synthetically latched
	 * into ctx->fw_proto_major (see STEP 4 in main()) right before the PING below. */
	if (ver_major != (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR &&
	    ver_major != (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR_LEGACY) {
		/* Firmware answered with a major this driver build has never negotiated --
		 * reset()'s own attempt missed, so its version gate never got to run either;
		 * this app's own direct call just found out the hard way. Alive, but not a
		 * version this host can safely talk further to. */
		return VERDICT_VERSION_SKEW;
	}
	if (ver_major == (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR_LEGACY) {
		/* Legacy (3.1) dialect: PING has no request payload phase at all on this
		 * wire, so neither A nor B -- both about the 4.0 CRC-trailer shape -- apply. */
		return ping_ok ? VERDICT_LEGACY_3_1_ACTIVE : VERDICT_LINK_ALIVE_UNPARSED;
	}

	/* ver_major == ALP_CC3501E_PROTOCOL_MAJOR (4): the shape A/B were built to discriminate. */
	const bool reset_missed = (step2_major == 0u);
	if (reset_missed) return ping_ok ? VERDICT_HYPOTHESIS_A : VERDICT_HYPOTHESIS_A_PLUS_B;
	return ping_ok ? VERDICT_HANDSHAKE_HEALTHY : VERDICT_HYPOTHESIS_B;
}

static const char *handshake_verdict_str(handshake_verdict_t v)
{
	switch (v) {
	case VERDICT_BRIDGE_ABSENT:
		return "BRIDGE ABSENT -- step 1 could not open the bridge's pins/SPI controller "
		       "on this board at all. Not a link fault: there is nothing to talk to yet";
	case VERDICT_VERSION_SKEW:
		return "VERSION SKEW -- the firmware ANSWERED a GET_VERSION request (the link is "
		       "demonstrably alive) with a major this host build refuses. Major 0 means "
		       "pre-versioning-scheme firmware (a raw v1..v9 integer); any other "
		       "unrecognised major is a genuine, unsupported skew";
	case VERDICT_HYPOTHESIS_C:
		return "HYPOTHESIS C (image not booting/answering) -- bring-up succeeded, major "
		       "never latched away from 0, and EVERY wire dump collected this run looked "
		       "undriven (all 0x00 or all 0xFF). A and B both require the firmware to be "
		       "alive enough to answer wrongly, which this run never observed on the wire";
	case VERDICT_LINK_ALIVE_UNPARSED:
		return "LINK ALIVE, REPLY UNPARSED -- the wire showed unmistakable signs of life "
		       "(a parked-idle marker or a structured candidate header, see the STEP dumps "
		       "above) but no step completed cleanly enough to support A, B, or a healthy "
		       "verdict. This is NOT hypothesis C: whatever is wrong here, the part is not "
		       "dead. This is the state the original bug's own six numbers map to";
	case VERDICT_HYPOTHESIS_A:
		return "HYPOTHESIS A (reset()'s own GET_VERSION missed) -- major was still 0 going "
		       "into step 3, this app's own GET_VERSION latched major 4, and the "
		       "now-CRC'd PING succeeded. Every earlier request was framed CRC-less "
		       "against strict v4.0 firmware and rejected with RESP_ERR_PROTOCOL (0x07)";
	case VERDICT_HYPOTHESIS_A_PLUS_B:
		return "HYPOTHESIS A, AND B IS ALSO LIVE -- reset's GET_VERSION missed (same as A "
		       "above), but the now-CRC'd PING STILL fails once major is latched to 4. A "
		       "explains the original failure; B needs its own follow-up on this link";
	case VERDICT_HYPOTHESIS_B:
		return "HYPOTHESIS B (the two-byte CRC-trailer-only PING shape is broken) -- major "
		       "was ALREADY 4 before this app ran (reset's own GET_VERSION succeeded), so "
		       "the PING that just failed carried the full new framing";
	case VERDICT_LEGACY_3_1_ACTIVE:
		return "LEGACY 3.1 STILL ACTIVE -- firmware answers with major 3 and PING succeeds "
		       "against it: the link is fully healthy, it is simply still running the OLD "
		       "image. The OTA to 4.0 has not landed on this part yet";
	case VERDICT_HANDSHAKE_HEALTHY:
	default:
		return "HANDSHAKE HEALTHY -- major 4 latched and the CRC-trailer PING against it "
		       "succeeds. Neither A nor B reproduces on this run";
	}
}

/** Human-readable name for a raw CC3501E response-status byte, for the
 *  step 6 diag readout -- printed alongside the raw hex, never in place of
 *  it (see cc3501e_diag_info()'s own doc comment: last_error is one of
 *  @ref alp_cc3501e_resp_t). Only the codes this app's verdict cares about
 *  are named; everything else prints as a bare hex value, which is still
 *  useful and does not need a name to be trustworthy. */
static const char *resp_name(uint8_t resp)
{
	switch (resp) {
	case ALP_CC3501E_RESP_OK:
		return "RESP_OK (0x5A)";
	case ALP_CC3501E_RESP_OK_LEGACY:
		return "RESP_OK_LEGACY (0x00)";
	case ALP_CC3501E_RESP_ERR_PROTOCOL:
		return "RESP_ERR_PROTOCOL (0x07) -- frame mis-parse: a CRC-less/mis-shaped "
		       "request the strict firmware could still read the HEADER of, but "
		       "rejected -- exactly what hypothesis A/B predict, not a dead link";
	case ALP_CC3501E_RESP_ERR_VERSION:
		return "RESP_ERR_VERSION (0x08)";
	default:
		return "(unnamed -- see alp_cc3501e_resp_t)";
	}
}

int main(void)
{
	printk("\n=== AEN801 CC3501E handshake probe -- BRIDGE_ABSENT / VERSION_SKEW / "
	       "HYPOTHESIS_C / LINK_ALIVE_UNPARSED / A / A+B / B / LEGACY_3_1_ACTIVE / "
	       "HANDSHAKE_HEALTHY ===\n");

	/*
	 * STATIC, not automatic, AND explicitly zero-initialised.
	 *
	 * Static because cc3501e_t embeds several ALP_CC3501E_MAX_PAYLOAD scratch
	 * buffers (~32 KB total): declared automatic it asks main() for a
	 * 32908-byte frame against CONFIG_MAIN_STACK_SIZE=4096, and Zephyr's
	 * stack-overflow check fires on the very first `sub sp` -- before this
	 * function's own first printk, so not even the header line escapes.
	 * Measured on the M55-HE 2026-09-10: USAGE FAULT, "Stack overflow
	 * (context area not valid)", faulting pc 0x00001554 = main+4.
	 * aen-evk-demo makes the same object static for the same reason.
	 *
	 * Explicitly zero-initialised, not left to rely on static storage
	 * defaulting to zeroed BSS: cc3501e_bridge_bringup() can return
	 * ALP_ERR_NOT_PRESENT_ON_THIS_SOC BEFORE ever calling cc3501e_init()
	 * (src/cc3501e_bridge.c, when a control pin or the SPI controller fails
	 * to open), in which case cc3501e_init()'s own memset never runs. The
	 * explicit initialiser here makes that safe by construction rather than
	 * by an accident of storage duration that a later refactor could quietly
	 * undo.
	 */
	static cc3501e_t fw = { 0 };

	/*
	 * ---------------------------------------------------------------
	 * STEP 1 -- bring the bridge up EXACTLY the way aen-evk-demo phase 8
	 * does (WIFI_EN high, nRESET pulsed, SPI1 @ 25 MHz), so the -5 this
	 * app is chasing is measured on the identical hardware path. This
	 * ALSO runs cc3501e_reset()'s own internal GET_VERSION -- whatever
	 * it does (or fails to do) to fw_proto_major is what step 2 reads.
	 * ---------------------------------------------------------------
	 */
	alp_status_t rc = cc3501e_bridge_bringup(&fw);
	printk("STEP 1: bridge bring-up (WIFI_EN high, nRESET pulsed, SPI1 @ %u Hz) -> %d\n",
	       (unsigned)CC3501E_BRIDGE_SPI_FREQ_HZ,
	       (int)rc);

	/*
	 * ---------------------------------------------------------------
	 * STEP 2 -- fw_proto_major/minor, immediately after bring-up, BEFORE
	 * this app's own step 3 can touch it. Read directly off the PUBLIC
	 * cc3501e_t struct fields (include/alp/chips/cc3501e/core.h) -- the
	 * same pattern aen-evk-demo's own phase 8 already uses. Safe to print
	 * on every path: `fw` is explicitly zeroed above, so this reads 0/0
	 * even on the ALP_ERR_NOT_PRESENT_ON_THIS_SOC path below.
	 * ---------------------------------------------------------------
	 */
	uint8_t step2_major = fw.fw_proto_major;
	uint8_t step2_minor = fw.fw_proto_minor;
	printk("STEP 2: fw_proto_major=%u fw_proto_minor=%u (as reset() left it -- 0 means "
	       "its own GET_VERSION round trip missed or the bridge never came up)\n",
	       (unsigned)step2_major,
	       (unsigned)step2_minor);

	/*
	 * STEPS 3-6 only run when bring-up returned ALP_OK. On
	 * ALP_ERR_NOT_PRESENT_ON_THIS_SOC there are no pins/SPI to probe; on
	 * ALP_ERR_VERSION cc3501e_reset() already cleared ctx->initialised, so
	 * every call below would return ALP_ERR_NOT_READY without ever
	 * touching the wire -- printing "(no reply)" six times about a link
	 * the driver has already conclusively characterised would bury the
	 * one line that matters (STEP 1/2's own numbers) under noise.
	 * handshake_classify() below gives both paths their own verdict from
	 * bringup_rc alone; see its own comment.
	 */
	const bool probed = (rc == ALP_OK);

	alp_status_t            ver_rc          = ALP_ERR_NOT_READY;
	uint16_t                version         = 0u;
	unsigned                ver_major       = 0u;
	unsigned                ver_minor       = 0u;
	bool                    latched_synth   = false;
	alp_status_t            ping_rc         = ALP_ERR_NOT_READY;
	unsigned                ping_attempts   = 0u;
	alp_status_t            di_rc           = ALP_ERR_NOT_READY;
	alp_cc3501e_diag_info_t diag            = { 0 };
	alp_status_t            ds_rc           = ALP_ERR_NOT_READY;
	cc3501e_diag_stats_t    stats           = { 0 };
	bool                    link_shows_life = false;

	if (!probed) {
		printk("STEPS 3-6: skipped -- bring-up did not return ALP_OK (%d), so "
		       "ctx->initialised is false and every call below would fail "
		       "ALP_ERR_NOT_READY without touching the wire; see STEP 1/2 for the "
		       "conclusive data on this path\n",
		       (int)rc);
	} else {
		/*
		 * -----------------------------------------------------------
		 * STEP 3 -- cc3501e_get_version() called DIRECTLY. cc3501e_reset()
		 * (step 1) zeroed fw_proto_major right before ITS OWN GET_VERSION
		 * attempt, and cc3501e_get_version() itself is a BARE round trip --
		 * it writes only *version_out, never ctx->fw_proto_major (that
		 * field is written ONLY inside cc3501e_reset(), see
		 * chips/cc3501e/cc3501e_core.c) -- so this call goes out under
		 * WHATEVER major step 1 left, same as any other caller. ALP_OK
		 * here proves the image is alive independent of what step 2
		 * showed; a failure gets its wire bytes dumped below.
		 * -----------------------------------------------------------
		 */
		ver_rc    = cc3501e_get_version(&fw, &version);
		ver_major = ALP_CC3501E_PROTOCOL_VERSION_MAJOR(version);
		ver_minor = ALP_CC3501E_PROTOCOL_VERSION_MINOR(version);
		printk("STEP 3: cc3501e_get_version() -> %d  raw=0x%04x  decoded=v%u.%u\n",
		       (int)ver_rc,
		       (unsigned)version,
		       ver_major,
		       ver_minor);
		if (ver_rc != ALP_OK) {
			wire_shape_t s = dump_wire_bytes("STEP 3", &fw);
			if (s == WIRE_SHAPE_PARKED_IDLE || s == WIRE_SHAPE_STRUCTURED) {
				link_shows_life = true;
			}
		}

		/*
		 * -----------------------------------------------------------
		 * STEP 4 -- the latch this whole experiment depends on.
		 * cc3501e_get_version() does NOT write ctx->fw_proto_major (see
		 * STEP 3's comment), so without this assignment step 5's PING
		 * would go out under whatever step 1 left -- identical framing
		 * to the demo's own already-failed PING, testing nothing new.
		 *
		 * This is a SYNTHETIC latch: this app writes the PUBLIC
		 * fw_proto_major/fw_proto_minor fields directly (same access
		 * pattern STEP 2 already uses to READ them) from step 3's own
		 * freshly decoded reply. It is NOT a negotiation the driver
		 * performed -- say so on the console line below, because a
		 * reader must not mistake this for cc3501e_reset() having
		 * identified the peer.
		 * -----------------------------------------------------------
		 */
		if (ver_rc == ALP_OK) {
			fw.fw_proto_major = (uint8_t)ver_major;
			fw.fw_proto_minor = (uint8_t)ver_minor;
			latched_synth     = true;
		}
		uint8_t step4_major = fw.fw_proto_major;
		printk("STEP 4: fw_proto_major=%u (%s)\n",
		       (unsigned)step4_major,
		       latched_synth ? "SYNTHETIC latch -- this app just wrote it from step 3's own "
		                       "decoded reply; NOT a negotiation cc3501e_reset() performed"
		                     : "unchanged from step 2 -- step 3 got no decodable reply to latch "
		                       "from, so nothing here was updated");

		/*
		 * -----------------------------------------------------------
		 * STEP 5 -- PING, bounded and retried like aen-evk-demo's own
		 * phase 8 (HANDSHAKE_PING_RETRIES / HANDSHAKE_PING_GAP_MS, see
		 * their definitions above). Framed against whatever step 4 just
		 * latched -- dialect_name() below says which shape that is, so
		 * the reader does not have to reconstruct want_req_crc by hand.
		 * -----------------------------------------------------------
		 */
		printk("STEP 5: PING framed under fw_proto_major=%u (%s)\n",
		       (unsigned)fw.fw_proto_major,
		       dialect_name(fw.fw_proto_major));
		for (; ping_attempts < HANDSHAKE_PING_RETRIES; ++ping_attempts) {
			ping_rc = cc3501e_ping(&fw);
			if (ping_rc == ALP_OK) {
				ping_attempts++; /* count the one that succeeded, not the ones before it */
				break;
			}
			k_msleep(HANDSHAKE_PING_GAP_MS);
		}
		printk("STEP 5: cc3501e_ping() -> %d after %u attempt(s) of %u (%u ms apart)\n",
		       (int)ping_rc,
		       ping_attempts,
		       HANDSHAKE_PING_RETRIES,
		       HANDSHAKE_PING_GAP_MS);
		if (ping_rc != ALP_OK) {
			wire_shape_t s = dump_wire_bytes("STEP 5", &fw);
			if (s == WIRE_SHAPE_PARKED_IDLE || s == WIRE_SHAPE_STRUCTURED) {
				link_shows_life = true;
			}
		}

		/*
		 * -----------------------------------------------------------
		 * STEP 6 -- GET_DIAG_INFO's last_error, plus DIAG_GET_STATS'
		 * frame counters (the driver exposes the two separately --
		 * alp_cc3501e_diag_info_t has no frame counters of its own,
		 * cc3501e_diag_stats_t is where frames_ok/frames_err live).
		 * -----------------------------------------------------------
		 */
		di_rc = cc3501e_diag_info(&fw, &diag);
		printk("STEP 6a: cc3501e_diag_info() -> %d  last_error=0x%02x %s\n",
		       (int)di_rc,
		       (unsigned)diag.last_error,
		       (di_rc == ALP_OK) ? resp_name(diag.last_error) : "(no reply)");
		if (di_rc != ALP_OK) {
			wire_shape_t s = dump_wire_bytes("STEP 6a", &fw);
			if (s == WIRE_SHAPE_PARKED_IDLE || s == WIRE_SHAPE_STRUCTURED) {
				link_shows_life = true;
			}
		}

		ds_rc = cc3501e_diag_stats(&fw, &stats);
		/*
		 * The corroborating tell ("frames arrived and were rejected, not
		 * lost") used to be gated on di_rc == ALP_OK && ds_rc == ALP_OK --
		 * but those two calls are subject to the SAME protocol rejection
		 * hypothesis A/B predict for every OTHER call, so requiring them
		 * to succeed made the tell structurally unreachable in exactly the
		 * scenario it exists to detect (today's actual run: every one of
		 * the six calls returned -5). The strict driver-confirmed form
		 * below is kept as the STRONGEST evidence when it IS available;
		 * link_shows_life (set from the wire dumps above, independent of
		 * any one call's own return code) is the fallback that still
		 * fires when it is not -- which is exactly what closes the
		 * original bug: STEP 6a's own dump above, not a human on SWD
		 * afterwards, is what proves the part is alive.
		 */
		const bool strict_corroboration =
		    (di_rc == ALP_OK && ds_rc == ALP_OK &&
		     diag.last_error == ALP_CC3501E_RESP_ERR_PROTOCOL && stats.frames_err > 0u);
		const char *corroboration =
		    strict_corroboration
		        ? "  <-- last_error=ERR_PROTOCOL + frames_err>0: frames ARRIVED and were "
		          "REJECTED, not lost -- corroborates A/B over C"
		        : (link_shows_life
		               ? "  <-- a wire dump above showed a parked-idle marker or a "
		                 "structured header even though the call it belongs to did not "
		                 "complete -- frames are reaching the firmware, not lost on a "
		                 "dead link, even though GET_DIAG_INFO/DIAG_GET_STATS could not "
		                 "confirm it their own official way"
		               : "");
		if (ds_rc == ALP_OK) {
			printk("STEP 6b: cc3501e_diag_stats() -> 0  frames_ok=%u frames_err=%u "
			       "has_worker_counters=%s%s\n",
			       (unsigned)stats.frames_ok,
			       (unsigned)stats.frames_err,
			       stats.has_worker_counters ? "true" : "false",
			       corroboration);
		} else {
			/* Same "(no reply)" discipline STEP 6a already applies: a -5 here
			 * means cc3501e_diag_stats() never wrote `stats`, so frames_ok/
			 * frames_err below are the struct's { 0 } initialiser, NOT a
			 * measurement -- printing them as if measured is exactly the
			 * original bug's mistake repeated one call later. */
			printk("STEP 6b: cc3501e_diag_stats() -> %d  (no reply -- frames_ok/"
			       "frames_err/has_worker_counters were never written)%s\n",
			       (int)ds_rc,
			       corroboration);
			wire_shape_t s = dump_wire_bytes("STEP 6b", &fw);
			if (s == WIRE_SHAPE_PARKED_IDLE || s == WIRE_SHAPE_STRUCTURED) {
				link_shows_life = true;
			}
		}
	}

	/*
	 * ---------------------------------------------------------------
	 * STEP 7 -- the verdict. See handshake_classify()'s own comment for
	 * the full decision table and its reasoning.
	 * ---------------------------------------------------------------
	 */
	bool                step3_ok = (ver_rc == ALP_OK);
	bool                ping_ok  = (ping_rc == ALP_OK);
	handshake_verdict_t verdict  = handshake_classify(
	    probed, rc, step2_major, step3_ok, (uint8_t)ver_major, ping_ok, link_shows_life);

	printk("STEP 7: VERDICT -- %s\n", handshake_verdict_str(verdict));
	printk("RESULT: verdict=%s step1_bringup=%d step2_major=%u step3_get_version=%d "
	       "step4_major=%u step5_ping=%d step5_attempts=%u step6_last_error=0x%02x "
	       "step6_frames_err=%u link_shows_life=%s\n",
	       (verdict == VERDICT_HYPOTHESIS_A)          ? "HYPOTHESIS_A"
	       : (verdict == VERDICT_HYPOTHESIS_A_PLUS_B) ? "HYPOTHESIS_A_PLUS_B"
	       : (verdict == VERDICT_HYPOTHESIS_B)        ? "HYPOTHESIS_B"
	       : (verdict == VERDICT_HYPOTHESIS_C)        ? "HYPOTHESIS_C"
	       : (verdict == VERDICT_LINK_ALIVE_UNPARSED) ? "LINK_ALIVE_UNPARSED"
	       : (verdict == VERDICT_LEGACY_3_1_ACTIVE)   ? "LEGACY_3_1_ACTIVE"
	       : (verdict == VERDICT_VERSION_SKEW)        ? "VERSION_SKEW"
	       : (verdict == VERDICT_BRIDGE_ABSENT)       ? "BRIDGE_ABSENT"
	                                                  : "HANDSHAKE_HEALTHY",
	       (int)rc,
	       (unsigned)step2_major,
	       (int)ver_rc,
	       (unsigned)fw.fw_proto_major,
	       (int)ping_rc,
	       ping_attempts,
	       (unsigned)diag.last_error,
	       (unsigned)stats.frames_err,
	       link_shows_life ? "true" : "false");

	return 0;
}
