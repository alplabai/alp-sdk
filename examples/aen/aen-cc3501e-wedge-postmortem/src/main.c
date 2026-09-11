/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-cc3501e-wedge-postmortem -- reproduces the CC3501E WIFI_CONNECT_STA
 * wedge and captures forensic evidence WHILE the link is still down, for the
 * E1M-AEN801 (Alif Ensemble E8, M55-HE), bench RAM-run via J-Link.
 *
 * WHY THIS APP EXISTS -- separating three live mechanisms
 * ---------------------------------------------------------
 * Issuing WIFI_CONNECT_STA over the CC3501E SPI bridge wedges the link. A
 * PING one second earlier answers on the first attempt; the connect itself
 * gets no reply at all, and the link then stays dead through a host-driven
 * reset pulse. Only a full carrier power cycle revives it. Reproduced three
 * times out of three.
 *
 * Three mechanisms are still live, and this app exists to tell them apart:
 *
 *   1. A task inside the CC3501E firmware is hung -- the M33 has stopped
 *      servicing SPI, but the chip is otherwise alive. A WARM reset (nRESET
 *      pulse, rails up -- @ref cc3501e_hard_reset) should revive it, because
 *      nothing about the chip's own power state is broken.
 *   2. The host's own reset sequence never actually reboots the chip, so
 *      every "reset did not revive it" observation -- in this app and in
 *      the previous bench run -- proves nothing at all. This is the
 *      mechanism PHASE A below exists to rule OUT before PHASE C's evidence
 *      can mean anything.
 *   3. The wedge is outside CC3501E firmware state entirely -- the Alif
 *      SPI1/DMA side, or a CC3501E always-on power domain WIFI_EN cannot
 *      gate -- in which case no firmware change fixes it, and not even a
 *      full power cycle (from this app's own WIFI_EN control) would revive
 *      the link.
 *
 * THE ORDER IS LOAD-BEARING -- do not reorder the phases below.
 * -----------------------------------------------------------------
 *
 * PHASE A -- positive control, in a healthy session, BEFORE anything is
 * wedged. Brings the bridge up, PINGs until it answers, reads
 * GET_DIAG_INFO's uptime_ms, issues a host hard reset ALONE (no wedge has
 * happened yet), PINGs again, and reads uptime_ms again. Comparing the two
 * uptime_ms values ALONE is NOT a safe test: cc3501e_hard_reset()'s own
 * ~3.5 s blind boot settle runs both inside this app's STEP 1 bring-up and
 * inside PHASE A's own reset, so the nominal margin between "the chip
 * rebooted" and "it didn't" shrinks to about 18 ms -- one failed PING retry
 * flips it. Instead this app brackets the reset with the HOST's own
 * k_uptime_get() and compares uptime_after against uptime_before PLUS the
 * host-measured elapsed time: without a reboot the chip's own clock keeps
 * running through that whole window, so uptime_after lands near that sum;
 * with a real reboot it lands near the post-reset settle alone, a margin of
 * about 3.5 SECONDS, not 18 milliseconds. This is the single most important
 * verdict this app prints, because if a host reset does not drive uptime_ms
 * back down in a HEALTHY session, mechanism 2 above is confirmed and every
 * later "the wedge survived a reset" observation -- in PHASE C below, and
 * in the bench run that motivated this app -- is meaningless. Whoever reads
 * the console must see that conclusion spelled out, not have to infer it
 * from raw numbers.
 *
 * PHASE B -- wedge it. Issues WIFI_CONNECT_STA with whatever credentials
 * are configured (see CREDENTIALS below), exactly as
 * aen-cc3501e-socket-throughput's STEP 3 does, apart from the connect
 * timeout budget -- see WEDGEPM_CONNECT_TIMEOUT_MS below for why this app
 * deliberately uses a MUCH SHORTER one than every sibling. The connect is
 * EXPECTED to fail -- that is the point of this app, not a bug to chase.
 * This app never WIDENS the timeout hunting for a pass; it DOES
 * deliberately SHRINK it, for the opposite reason -- not to help the
 * association succeed, but to stop cc3501e_wifi_connect()'s OWN poll loop
 * re-framing the link while the firmware works (see WHAT THIS APP DOES NOT
 * DO below).
 *
 * A short connect budget ALONE is not enough, though: the firmware's own
 * connect body keeps running -- and keeps holding the host off -- for up
 * to 30 s of L2 association plus 10 s of DHCP, regardless of what this
 * app's own timeout returns (see WEDGEPM_QUIET_WAIT_MS below). So once
 * cc3501e_wifi_connect() returns, PHASE B goes SILENT: no requests at all,
 * nothing clocked on the bus, until past that firmware worst case plus a
 * reinitialisation margin. THE SHORT BUDGET AND THE LONG WAIT ARE NOT IN
 * TENSION -- they solve two different problems. The short budget stops
 * THIS APP'S OWN driver code from clocking hundreds of re-framing
 * WIFI_STATUS polls at a slave that may already be wedging; the silent
 * wait then lets that identical span of time pass with the bus completely
 * IDLE, so a HEALTHY board gets the same legitimate window to associate,
 * get a DHCP lease, and re-arm that it would have had without any of this
 * app's shortening. Only THEN does PHASE B issue exactly ONE confirming
 * PING. If it answers, the board was never actually wedged this run --
 * this app prints `WEDGE NOT REPRODUCED` and stops before PHASE C or the
 * recovery ladder runs at all, because there is nothing to diagnose and
 * nothing to recover from.
 *
 * PHASE C -- the payload, reached only when PHASE B's confirming PING
 * (above) did NOT answer, i.e. the wedge actually reproduced this run. Its
 * OWN first PING is a SECOND, INDEPENDENT confirmation before anything else
 * runs -- PHASE B's confirming PING is single-shot and can fail spuriously
 * on a healthy board, so a success HERE stops the run as WEDGE NOT
 * REPRODUCED too, exactly like PHASE B's own success path (this app's own
 * review record, third pass, #2035, Fix 3). Only once BOTH pings have
 * failed does PHASE C capture, in the wedged state, BEFORE any recovery is
 * attempted:
 *   - the receive scratch buffer left by that PING (only when ping_rc
 *     proves the driver's request path was actually entered -- see
 *     WEDGEPM_REPORT_SCRATCH below), reported against the wire-level
 *     patterns THIS REPOSITORY DOCUMENTS, each with its genuinely ambiguous
 *     origin(s) spelled out rather than a single mechanism guessed from a
 *     tail alone -- not a claim that the list is exhaustive;
 *   - the READY pin level, read directly as a GPIO input and printed for
 *     completeness only -- this repo documents that exact pad as an OPEN
 *     CONNECTION on the bench unit, so it carries NO evidentiary weight and
 *     never appears in the verdict;
 *   - a host hard reset ALONE, then a PING -- reports whether it answered;
 *   - a full power-off, a hold of >= WEDGEPM_POWER_OFF_HOLD_MS (2000, see
 *     that macro's own comment for why 2 seconds is the floor, not a round
 *     number), then a power-on + reset, then a PING -- reports whether it
 *     answered, and whether the power cycle even reached the wire. A single
 *     failed PING here gets WEDGEPM_COLD_BOOT_REBOOTS extra hard-reset
 *     attempts before this app concludes anything from it -- the same
 *     healthy-path allowance aen-cc3501e-gpio's own gpio_wait_for_link()
 *     uses, because a healthy cold boot on this part can need a second hard
 *     reset (this app's own review record, third pass, #2035, Fix 2).
 * Finally prints ONE verdict line naming which of the three mechanisms
 * above the combined PHASE A + PHASE C evidence points to.
 *
 * CREDENTIALS -- never committed
 * ---------------------------------
 * Follows the convention aen-cc3501e-companion-tour's file header
 * establishes (read that app's TOUR_WIFI_SSID comment for the fuller
 * version of this note), the same convention aen-cc3501e-socket-throughput
 * copies too: credentials arrive as build-time defines, with EMPTY
 * defaults, and this app skips PHASE B and PHASE C entirely when the SSID
 * is empty -- there is nothing to wedge the link with, so there is no
 * post-mortem to take. This app uses its OWN macro names (WEDGEPM_*), not
 * the tour's or the throughput app's, so no two of these examples ever
 * fight over a shared -D on a combined build. No real SSID or passphrase is
 * embedded anywhere in this file -- see the Tunables section below for the
 * exact override syntax.
 *
 * WHAT THIS APP DOES NOT DO
 * ----------------------------
 * It never retries the connect hunting for a pass and never WIDENS
 * cc3501e_wifi_connect()'s timeout looking for one -- it deliberately
 * SHRINKS it instead (see WEDGEPM_CONNECT_TIMEOUT_MS) to stop its OWN poll
 * loop re-framing the link while the firmware works, then waits QUIETLY
 * instead (see WEDGEPM_QUIET_WAIT_MS) rather than clocking the bus at all
 * while that span passes. It never runs PHASE C, or any recovery step, when
 * PHASE B's own confirming PING answers -- a board that was never wedged
 * has nothing to take a post-mortem of (see PHASE B above). It never
 * attempts recovery before PHASE C has captured every piece of evidence it
 * captures -- capturing the evidence AFTER a recovery attempt would destroy
 * exactly the state this app exists to observe. And it never asserts
 * MECHANISM 1 (a hung firmware task) from a warm-reset revival ALONE --
 * bench precedent (cc3501e_recover()'s own doc comment,
 * chips/cc3501e/cc3501e_core.c) records every observed wedge of one class
 * clearing with a warm reset while a firmware-side snapshot showed the
 * firmware had stayed perfectly healthy throughout, so this app names BOTH
 * candidate origins wherever it reports a warm-reset revival.
 *
 * Build target: alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he -- see this
 * app's README.md for the full west build invocation and the overlay's own
 * header for why the SRAM0 memory placement is load-bearing, not
 * decorative: this app's own PING + READY probes ride the identical SPI1
 * link every other AEN801 CC3501E bench app measures, and a faster
 * cache-on/DTCM path would risk reproducing a DIFFERENT failure than the
 * one this app exists to characterise.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "alp/chips/cc3501e.h"
#include "cc3501e_bridge.h" /* cc3501e_bridge_bringup() -- the SoM bring-up template */

/*
 * ======================================================================
 * Tunables -- timeouts and the (build-time) Wi-Fi credentials the file
 * header above explains.
 * ======================================================================
 */

/* How many times a PING retry loop waits for the bridge to answer. Same
 * shape and same reasoning as every aen-cc3501e-* sibling's own retry loop
 * (e.g. aen-cc3501e-socket-throughput's SOCKTP_PING_RETRIES): the bring-up
 * / reset call this app makes already waited out its own boot budget once,
 * so a loop here only absorbs residual ramp/boot jitter. Reused for every
 * PING-after-a-reset step in this file (PHASE A's positive control, and
 * both of PHASE C's recovery attempts), not just the initial STEP 2 -- a
 * single named constant so a bench operator tuning one retry budget tunes
 * all of them together. */
#define WEDGEPM_PING_RETRIES 25u
#define WEDGEPM_PING_GAP_MS  200u

/* Minimum gap PHASE A requires between uptime_ms AFTER a host hard reset
 * and the NO-REBOOT PREDICTION (uptime_before + the host's own measured
 * elapsed time across the reset) before calling it a proven reboot -- see
 * the PHASE A VERDICT logic below and the file header's PHASE A paragraph.
 * A genuine reboot lands roughly cc3501e_hard_reset()'s own ~3.5 s blind
 * boot settle BELOW the prediction; a reset that never reached the chip
 * lands roughly 0 ms below it (the chip's own clock just kept running).
 * 1000 ms sits comfortably inside that ~3.5 s gap, with headroom for
 * scheduling/SPI jitter, and is nowhere near the ~18 ms false-positive
 * margin comparing uptime_after against uptime_before ALONE used to
 * leave -- see this app's own review record (Blocker 1) for that failure
 * mode: a single failed first PING after a real reboot could push
 * uptime_after above uptime_before under the old comparison, on a HEALTHY
 * board, and falsely confirm mechanism 2. */
#define WEDGEPM_RESET_PROOF_MARGIN_MS 1000u

/* PHASE B's own connect timeout budget -- DELIBERATELY SHORT, unlike every
 * other aen-cc3501e-* sibling's own connect call (which floors comfortably
 * above the firmware's worst-case 30 s L2 association + 10 s DHCP = 40 s
 * budget -- see aen-cc3501e-socket-throughput's SOCKTP_CONNECT_TIMEOUT_MS,
 * derived from hal/ti/cc3501e_hw_ti_wifi.c).
 *
 * This app is NOT trying to let the association succeed -- see the file
 * header's WHAT THIS APP DOES NOT DO -- it is trying to let PHASE C observe
 * the link close to the MOMENT it wedges. cc3501e_wifi_connect()'s own
 * WIFI_STATUS poll loop re-issues a status request every
 * CC3501E_WIFI_STATUS_POLL_GAP_MS (50 ms, chips/cc3501e/cc3501e_wifi.c) for
 * the WHOLE budget it is given, and each attempt clocks bytes at a slave
 * the driver's own comment says takes on a PERMANENT 1-byte frame offset
 * that cannot self-correct from exactly that kind of re-framing
 * (chips/cc3501e/cc3501e_core.c). A 55 s budget -- the siblings' own -- would
 * mean PHASE C characterises the link after roughly 1100 of those
 * re-framing transactions have already run against the wedge, not the
 * wedge itself. This is a SHORTENING to capture the moment, not the
 * WIDENING-to-hunt-for-a-pass the file header argues against -- that
 * reasoning does not carry over here; it runs the other way.
 *
 * Shortening THIS budget alone is not the whole fix, though: it stops this
 * app's own poll loop from re-framing the link, but a HEALTHY board still
 * needs the firmware's full association+DHCP window before it is fair to
 * call it unresponsive. See WEDGEPM_QUIET_WAIT_MS immediately below for how
 * this app gets that window back without reintroducing the re-framing this
 * budget exists to avoid. */
#define WEDGEPM_CONNECT_TIMEOUT_MS 2000u

/* How long PHASE B waits, QUIETLY -- issuing no requests at all, nothing
 * clocked on the bus -- after cc3501e_wifi_connect() returns
 * (WEDGEPM_CONNECT_TIMEOUT_MS above), before issuing the single confirming
 * PING that decides whether the wedge actually reproduced this run. This is
 * a DIFFERENT knob from WEDGEPM_CONNECT_TIMEOUT_MS and NOT in tension with
 * it -- see the file header's PHASE B paragraph for why shrinking one and
 * lengthening the other both serve the same goal.
 *
 * Derived exactly like SOCKTP_CONNECT_TIMEOUT_MS
 * (aen-cc3501e-socket-throughput's own sibling budget, see that app's own
 * comment): the firmware's own worst case for one connect is 30 s of L2
 * association (hal/ti/cc3501e_hw_ti_wifi.c's `osi_SyncObjWait(&wifi_event_
 * sync, 30u * OSI_WAIT_FOR_SECOND)`) plus 10 s of DHCP
 * (`CC3501E_STA_DHCP_TRIES * CC3501E_STA_DHCP_POLL_US` = 50 * 200 ms) = 40 s
 * total. This app deliberately reuses the SAME 15 s reinitialisation margin
 * the sibling budget adds on top of that 40 s (55 s - 40 s = 15 s), rather
 * than inventing a fresh number, because the underlying question is
 * identical: how long does a HEALTHY board need before it is fair to call
 * it unresponsive. The difference from the sibling budget is not the total,
 * it is WHO clocks the bus while that time passes -- the sibling's own
 * cc3501e_wifi_connect() call polls WIFI_STATUS every 50 ms for the whole
 * span; this app's wait issues nothing at all, so this app never hands a
 * wedging link the extra re-framing traffic WEDGEPM_CONNECT_TIMEOUT_MS
 * exists to avoid. */
#define WEDGEPM_QUIET_WAIT_MS 55000u

/*
 * Wi-Fi STA credentials for the wedging CONNECT in PHASE B. DELIBERATELY
 * EMPTY by default -- never hardcode bench credentials in a public
 * example. Set them at build time WITHOUT editing this file, e.g.:
 *
 *   west build ... -- -DEXTRA_CFLAGS="-DWEDGEPM_WIFI_SSID=\\\"myssid\\\" \
 *                                     -DWEDGEPM_WIFI_PASS=\\\"mypass\\\""
 *
 * When WEDGEPM_WIFI_SSID is empty, main() prints one line saying so and
 * returns before PHASE B or PHASE C touch the radio at all -- there is
 * nothing to wedge the link with, so there is nothing to take a
 * post-mortem of.
 */
#ifndef WEDGEPM_WIFI_SSID
#define WEDGEPM_WIFI_SSID ""
#endif
#ifndef WEDGEPM_WIFI_PASS
#define WEDGEPM_WIFI_PASS ""
#endif
/* Security: 0 = open, 1 = WPA2-PSK, 2 = WPA3-SAE (matches
 * CC3501E_WIFI_CONNECT_SEC_* in <alp/chips/cc3501e/wifi.h>). */
#ifndef WEDGEPM_WIFI_SECURITY
#define WEDGEPM_WIFI_SECURITY 1u
#endif

/* Minimum power-off hold before PHASE C's full power-cycle recovery
 * attempt, in milliseconds. NOT the same as the 50 ms discharge gate
 * cc3501e_reset() itself carries (chips/cc3501e/cc3501e_core.c, the "COLD-
 * BOOT POWER SEQUENCE" comment above its alp_delay_ms(50u) discharge
 * call) -- that gate's OWN comment warns a short gate risks a brown-out
 * that skips the chip's chain-of-trust re-initialisation rather than a
 * clean boot, i.e. 50 ms is already the tight end of what that path
 * trusts. This app holds power off for its OWN 2000 ms on top of that,
 * via cc3501e_power_off() (which has no built-in delay of its own -- it
 * just drops the rails and returns) before ever calling cc3501e_reset(),
 * so the rails see roughly 2000 ms + cc3501e_reset()'s own 50 ms of real
 * discharge time, not 50 ms alone. Do not shrink this back toward
 * cc3501e_reset()'s own 50 ms -- that is exactly the brown-out risk its
 * own comment warns about. */
#define WEDGEPM_POWER_OFF_HOLD_MS 2000u

/* Extra hard-reset attempts PHASE C's own full power-cycle step gets before
 * concluding anything from a failed PING (Fix 2, this app's own review
 * record, third pass, #2035) -- mirrors aen-cc3501e-gpio's own
 * GPIO_LINK_REBOOTS exactly. A single cold boot failing on this part is a
 * KNOWN-BENIGN outcome, not evidence for mechanism 3: chips/cc3501e/
 * cc3501e_core.c's COLD-BOOT POWER SEQUENCE comment documents the Puya-flash
 * cold-boot bug, where the FIRST boot after a cold power-on can mis-read
 * flash and never launch the vendor image, and a SECOND boot (rails kept
 * up, same as a plain hard reset) then comes up clean. This app's own PHASE
 * C power-cycle step IS the first cold boot; a failed PING after it gets the
 * same extra-attempts budget aen-cc3501e-gpio's gpio_wait_for_link() treats
 * as the healthy-path allowance before it draws any conclusion. */
#define WEDGEPM_COLD_BOOT_REBOOTS 3u

/*
 * ======================================================================
 * PING + rx_scratch capture -- see the file header's PHASE C for what this
 * is used for.
 * ======================================================================
 */

/** PING with a bounded retry loop, printing the attempt count. Same shape
 *  as every aen-cc3501e-* sibling's own PING loop (e.g.
 *  aen-cc3501e-socket-throughput's STEP 2) -- reused here for every PING
 *  this app issues after a reset, not just the first one, via the @p label
 *  prefix so the console line names which phase's PING this is. */
static bool wedgepm_ping_wait(cc3501e_t *fw, const char *label)
{
	for (unsigned i = 0u; i < WEDGEPM_PING_RETRIES; i++) {
		if (cc3501e_ping(fw) == ALP_OK) {
			printk("%s: PING ok after %u attempt%s\n", label, i + 1u, (i == 0u) ? "" : "s");
			return true;
		}
		k_msleep(WEDGEPM_PING_GAP_MS);
	}
	printk("%s: PING never answered after %u attempts\n", label, (unsigned)WEDGEPM_PING_RETRIES);
	return false;
}

/**
 * Extra hard-reset ladder for PHASE C's post-power-cycle recovery step
 * (Fix 2, this app's own review record, third pass, #2035) -- mirrors
 * aen-cc3501e-gpio's own gpio_wait_for_link(): see WEDGEPM_COLD_BOOT_REBOOTS'
 * own comment for why a single failed PING after a cold boot cannot support
 * a conclusion on this part. The caller has already issued the FIRST reset
 * (cc3501e_reset()'s own cold power-on) and already tried one PING round --
 * this only runs the EXTRA rounds, one @ref cc3501e_hard_reset per round,
 * up to WEDGEPM_COLD_BOOT_REBOOTS times. Returns true once a PING succeeds.
 */
static bool wedgepm_cold_boot_retry(cc3501e_t *fw)
{
	bool revived = false;
	for (unsigned reboot = 0u; !revived && reboot < WEDGEPM_COLD_BOOT_REBOOTS; ++reboot) {
		printk("PHASE C: PING never answered after the power-cycle reset -- a single cold "
		       "boot failing is KNOWN-BENIGN on this part (see WEDGEPM_COLD_BOOT_REBOOTS's "
		       "own comment), issuing hard reset %u/%u and retrying\n",
		       reboot + 1u,
		       (unsigned)WEDGEPM_COLD_BOOT_REBOOTS);
		(void)cc3501e_hard_reset(fw);
		revived = wedgepm_ping_wait(fw, "PHASE C (after power-cycle retry)");
	}
	return revived;
}

/*
 * Print every known wire-level origin for an rx_scratch[1..3] == 0xA5A5A5
 * tail -- see wedgepm_report_scratch()'s own doc comment (this app's own
 * review record, Blocker 2) for why byte 0, the one byte that would narrow
 * these down, is exactly what got overwritten. This app's own review
 * record (Major -- second pass, #2035) found the ORIGINAL two-way split
 * here still asserted a mechanism from each candidate; neither assertion
 * held up, so none of the origins below are attributed to a mechanism --
 * the snapshot cannot choose between them. */
static void wedgepm_print_tail_a5(uint8_t rs1, uint8_t rs2, uint8_t rs3)
{
	printk("PHASE C:   tail = %02X %02X %02X (0xA5A5A5 pattern) -- at least THREE known "
	       "wire-level origins produce this, and this single snapshot cannot choose between "
	       "them:\n",
	       rs1,
	       rs2,
	       rs3);
	printk("PHASE C:     (a) REQUEST-HEADER in-band armed check failed (cc3501e_core.c, "
	       "the IN-BAND ARMED CHECK comment) -- the slave had not yet re-armed its header "
	       "phase when the host clocked it. The driver's own comment does not say WHY the "
	       "slave was not yet armed at that moment.\n");
	printk("PHASE C:     (b) the slave's STALE TRANSMIT FIFO echoing a PREVIOUS phase's "
	       "bytes -- a SLAVE-SIDE signature, not a host framing fault: every host transceive "
	       "returns whatever the slave last loaded into its TX FIFO, not fresh data for THIS "
	       "phase.\n");
	printk("PHASE C:     (c) REPLY-HEADER hdr_ok check failed (cc3501e_core.c, the desync-"
	       "detection comment above hdr_ok) with byte 0 unrecoverable -- consistent with "
	       "(not proof of) the driver's own 'parked at a frame boundary' all-0xA5 signature; "
	       "requires the slave to have been ARMED and to have replied with SOMETHING.\n");
}

/* Same shape as wedgepm_print_tail_a5() -- see its doc comment -- for an
 * rx_scratch[1..3] == 0xFFFFFF tail. This app's own review record (third
 * pass, #2035, Fix 4) adds origin (c): examples/aen/aen-evk-demo/src/main.c's
 * own cc35_describe_rx_scratch() documents a THIRD origin for an all-0xFF
 * read this file's earlier revisions omitted -- an undriven line. None of
 * the three below is a hung firmware task, unlike the previous revision of
 * this file, which mapped this pattern to "lockstep drift" and mechanism 1
 * -- but see this file's own list-completeness caveat (wedgepm_report_
 * scratch()'s doc comment) before treating that as exhaustive. */
static void wedgepm_print_tail_ff(uint8_t rs1, uint8_t rs2, uint8_t rs3)
{
	printk("PHASE C:   tail = %02X %02X %02X (0xFFFFFF pattern) -- at least THREE documented "
	       "origins produce this, none of them a hung firmware task. This single snapshot "
	       "cannot choose between them:\n",
	       rs1,
	       rs2,
	       rs3);
	printk("PHASE C:     (a) the Puya-flash cold-boot bug (cc3501e_core.c's COLD-BOOT "
	       "POWER SEQUENCE comment): the vendor image had not launched yet -- reqhdr_rx "
	       "reads 0xFFFFFFFF until the workaround's second boot lands it. A transient "
	       "BOOT-TIME condition, not a wedge in an already-running firmware.\n");
	printk("PHASE C:     (b) a data-in mis-sampling fault on the ALIF MASTER's own MISO "
	       "capture point (docs/cc3501e-bridge.md's RX_SAMPLE_DLY note: 'over the on-SoM "
	       "traces mis-samples') -- a HOST-SIDE timing fault, unrelated to CC3501E firmware "
	       "state entirely.\n");
	printk("PHASE C:     (c) an UNDRIVEN line (examples/aen/aen-evk-demo/src/main.c's own "
	       "cc35_describe_rx_scratch()) -- distinct from an all-0x00 read: nothing is "
	       "pulling the line low either, not even a dead phase.\n");
}

/* Same shape again for an rx_scratch[1..3] == 0x000000 tail -- the ONE
 * pattern the previous revision of this file did not print an origin for
 * at all (this app's own review record, second pass, #2035). */
static void wedgepm_print_tail_00(uint8_t rs1, uint8_t rs2, uint8_t rs3)
{
	printk("PHASE C:   tail = %02X %02X %02X (0x000000 pattern) -- at least TWO known "
	       "origins produce this. This single snapshot cannot choose between them:\n",
	       rs1,
	       rs2,
	       rs3);
	printk("PHASE C:     (a) the slave STUCK MID-REPLY on a transfer it armed EARLIER and "
	       "never completed -- this app's own src/cc3501e_bridge.h documents the exact "
	       "signature (the pre-existing SOCK_RECV desync defect): the in-band idle marker "
	       "reads [.. 00 00 00] forever instead of A5 A5 A5 A5. A stall LEFT OVER from an "
	       "earlier, unrelated transfer -- not necessarily anything about THIS request.\n");
	printk("PHASE C:     (b) the #1378 dead-phase alias (cc3501e_core.c): a dead bus phase "
	       "clocks back literal 0x00 for every byte. This snapshot alone cannot tell it "
	       "apart from (a).\n");
}

/*
 * Fix 4, this app's own review record, third pass, #2035: the ONE pattern
 * examples/aen/aen-evk-demo/src/main.c's own cc35_describe_rx_scratch()
 * documents as actually MEANING the link is wedged -- a STALE REPLY HEADER,
 * the slave answering from ONE TRANSFER BEHIND (a real header, just not
 * this request's) -- and the one this file's earlier revisions had no
 * branch for at all, falling through to "no further classification"
 * instead. That sibling check reads rs[0] as an opcode AND rs[2..3] as a
 * declared payload length; here rs[0] is unavailable -- it is the driver's
 * own 0xDA poison, not wire data (see wedgepm_report_scratch()'s own doc
 * comment) -- so only the length field is checkable. @p rs2 / @p rs3 are
 * rx_scratch[2] / [3], the LE16 declared-length field the sibling check
 * also reads; a value that decodes to a plausible payload length is still
 * meaningful evidence on its own, just not a full confirmation of the
 * sibling's stricter two-field check. */
static bool wedgepm_tail_looks_like_stale_header(uint8_t rs2, uint8_t rs3)
{
	uint16_t declared_len = (uint16_t)rs2 | ((uint16_t)rs3 << 8);
	return declared_len >= 1u && declared_len <= ALP_CC3501E_MAX_PAYLOAD;
}

/** Prints the stale-reply-header interpretation -- see
 *  wedgepm_tail_looks_like_stale_header()'s own doc comment for what this
 *  checks and why it is narrower than the sibling check it is adapted
 *  from. */
static void wedgepm_print_tail_stale_header(uint8_t rs1, uint8_t rs2, uint8_t rs3)
{
	uint16_t declared_len = (uint16_t)rs2 | ((uint16_t)rs3 << 8);
	printk("PHASE C:   tail = %02X %02X %02X -- bytes [2..3] decode as a PLAUSIBLE reply "
	       "length field (declared_len=%u, in range [1, %u]) -- examples/aen/aen-evk-demo's "
	       "own cc35_describe_rx_scratch() documents this shape as the ONE pattern that "
	       "actually MEANS the link is wedged: a STALE REPLY HEADER, the slave answering "
	       "from ONE TRANSFER BEHIND. Byte 0 is unavailable here (the driver's own 0xDA "
	       "poison, not wire data), so this cannot confirm the opcode field the sibling "
	       "check also reads -- the length field alone is still the strongest single "
	       "signal any of this app's tail patterns print.\n",
	       rs1,
	       rs2,
	       rs3,
	       (unsigned)declared_len,
	       (unsigned)ALP_CC3501E_MAX_PAYLOAD);
}

/*
 * Report a captured 4-byte rx_scratch[0..3] snapshot in words a bench log
 * can be read without chips/cc3501e/cc3501e_core.c open beside it. Adapted
 * from examples/aen/aen-evk-demo/src/main.c's own cc35_describe_rx_scratch()
 * (its BLE_ENABLE failure probe is the precedent for reading ctx->rx_scratch
 * directly for diagnostics -- see the doc comment on cc3501e_t::rx_scratch in
 * <alp/chips/cc3501e/core.h>, which blesses exactly this use). This is
 * therefore reading a PUBLIC, documented struct field, not reaching into a
 * private one.
 *
 * @p ping_rc GATES the whole interpretation (this app's own review record,
 * Major 4): cc3501e_request() -- which cc3501e_ping() calls straight through
 * -- short-circuits ALP_ERR_NOT_READY on a context that failed to
 * (re-)initialise, WITHOUT touching rx_scratch at all (chips/cc3501e/
 * cc3501e_core.c). A snapshot taken after THAT short-circuit is leftover
 * residue from whatever the previous call left behind -- reporting it as
 * fresh wire evidence is the same stale-console failure mode this campaign
 * already burned a bench run on.
 *
 * When the driver's request path WAS entered, the FIRST check is the one
 * this app's own file header warns about: cc3501e_request_locked()'s `out:`
 * label unconditionally overwrites rx_scratch[0] to
 * ALP_CC3501E_RX_SCRATCH_NO_STATUS (0xDA) on EVERY pre-decode failure --
 * which a failed PING against a wedged link always is. Bytes [1..3] then
 * carry whatever raw wire pattern SOME earlier phase actually read -- but,
 * per Blocker 2 above, not uniquely WHICH phase; wedgepm_print_tail_a5() /
 * _ff() / _00() / _stale_header() each list every origin THIS REPOSITORY
 * DOCUMENTS for their pattern explicitly, instead of naming one mechanism
 * from a tail alone. These are the origins alp-sdk and its sibling examples
 * happen to document, not a claim that the list is complete -- a pattern
 * with no branch below falls through to the generic "no further
 * classification" line, which is a statement about this app's own
 * knowledge, not proof that no wire-level explanation exists. */
static void wedgepm_report_scratch(const uint8_t rs[ALP_CC3501E_HEADER_BYTES], alp_status_t ping_rc)
{
	printk("PHASE C: rx_scratch[0..3] = %02X %02X %02X %02X\n", rs[0], rs[1], rs[2], rs[3]);

	if (ping_rc == ALP_ERR_NOT_READY) {
		printk("PHASE C:   STALE, not interpreted -- ping_rc is ALP_ERR_NOT_READY, meaning "
		       "cc3501e_ping() short-circuited BEFORE entering the driver's request path "
		       "(cc3501e_request(), cc3501e_core.c) and never touched rx_scratch this call. "
		       "The 4 bytes above are residue from an earlier request, not evidence about "
		       "the current wedge.\n");
		return;
	}

	if (rs[0] != ALP_CC3501E_RX_SCRATCH_NO_STATUS) {
		printk("PHASE C:   decoded -- byte 0 (%02X) is the real status byte "
		       "cc3501e_reply_verdict() returned (ping_rc=%d, printed above); bytes [1..3] "
		       "are the reply header's flags/length fields carried over from the "
		       "reply-header phase (cc3501e_core.c's reply_hdr copy), NOT payload data -- no "
		       "further wire-mechanism evidence here.\n",
		       rs[0],
		       (int)ping_rc);
		return;
	}

	printk("PHASE C:   driver marker 0xDA (ALP_CC3501E_RX_SCRATCH_NO_STATUS) -- byte 0 was "
	       "overwritten on a pre-decode exit; bytes [1..3] are the only wire evidence left.\n");
	bool tail_a5 = (rs[1] == ALP_CC3501E_SYNC_IDLE) && (rs[2] == ALP_CC3501E_SYNC_IDLE) &&
	               (rs[3] == ALP_CC3501E_SYNC_IDLE);
	bool tail_00 = (rs[1] == 0x00u) && (rs[2] == 0x00u) && (rs[3] == 0x00u);
	bool tail_ff = (rs[1] == 0xFFu) && (rs[2] == 0xFFu) && (rs[3] == 0xFFu);
	if (tail_a5) {
		wedgepm_print_tail_a5(rs[1], rs[2], rs[3]);
	} else if (tail_ff) {
		wedgepm_print_tail_ff(rs[1], rs[2], rs[3]);
	} else if (tail_00) {
		wedgepm_print_tail_00(rs[1], rs[2], rs[3]);
	} else if (wedgepm_tail_looks_like_stale_header(rs[2], rs[3])) {
		wedgepm_print_tail_stale_header(rs[1], rs[2], rs[3]);
	} else {
		printk("PHASE C:   tail bytes do not match any of the repeated-byte patterns "
		       "(0xA5A5A5 / 0x000000 / 0xFFFFFF) or a plausible stale-header length field -- "
		       "no classification in THIS APP'S list applies. That is a statement about "
		       "what this app and its sibling examples document, not proof no wire-level "
		       "explanation exists.\n");
	}
}

/*
 * ======================================================================
 * PHASE A helpers -- reset_cause name lookup, still printed in every PHASE
 * A GET_DIAG_INFO line for the transcript (see the file header's PHASE A
 * paragraph), plus the frames_ok cross-check that REPLACED the reset_cause
 * cross-check the second pass added (this app's own review record, third
 * pass, #2035, Fix 1) -- see that fix's own comment at the call site for
 * why reset_cause cannot do the discriminating job on this firmware.
 * ======================================================================
 */

/** Human name for a @ref alp_cc3501e_reset_cause_t value, so the console
 *  reads without <alp/protocol/cc3501e.h> open beside it. */
static const char *wedgepm_reset_cause_name(uint8_t cause)
{
	switch (cause) {
	case ALP_CC3501E_RESET_UNKNOWN:
		return "UNKNOWN";
	case ALP_CC3501E_RESET_POWER_ON:
		return "POWER_ON";
	case ALP_CC3501E_RESET_NRST_PIN:
		return "NRST_PIN";
	case ALP_CC3501E_RESET_SOFT:
		return "SOFT";
	case ALP_CC3501E_RESET_WATCHDOG:
		return "WATCHDOG";
	case ALP_CC3501E_RESET_BROWNOUT:
		return "BROWNOUT";
	case ALP_CC3501E_RESET_BLE_STACK:
		return "BLE_STACK";
	case ALP_CC3501E_RESET_WIFI_STACK:
		return "WIFI_STACK";
	default:
		return "?";
	}
}

/* ---------------------------------------------------------------------- */

int main(void)
{
	printk("\n=== AEN801 CC3501E wedge post-mortem ===\n");

	/*
	 * STATIC, explicitly zero-initialised cc3501e_t -- same discipline as
	 * every aen-cc3501e-* sibling, for the identical reason: this type
	 * embeds several ALP_CC3501E_MAX_PAYLOAD scratch buffers (~32 KB
	 * total). Declared automatic it asks main() for a multi-kilobyte
	 * stack frame, and Zephyr's stack-overflow check fires on the very
	 * first `sub sp` -- before this function's own first printk. Measured
	 * on the M55-HE 2026-09-10 by aen-cc3501e-handshake-probe; this app
	 * inherits the same fix rather than rediscovering the same failure.
	 */
	static cc3501e_t fw = { 0 };

	/*
	 * ---------------------------------------------------------------
	 * STEP 1 -- bring the bridge up, byte-identical to aen-evk-demo
	 * phase 8 and every aen-cc3501e-* sibling (same src/cc3501e_bridge.{c,h}
	 * template, same overlay).
	 * ---------------------------------------------------------------
	 */
	alp_status_t rc = cc3501e_bridge_bringup(&fw);
	printk("STEP 1: bridge bring-up (WIFI_EN high, nRESET pulsed, SPI1 @ %u Hz) -> %d\n",
	       (unsigned)CC3501E_BRIDGE_SPI_FREQ_HZ,
	       (int)rc);
	if (rc != ALP_OK) {
		printk("  ** bring-up did NOT return ALP_OK -- nothing downstream of it can work. "
		       "Stopping here. **\n");
		return 0;
	}

	/*
	 * ---------------------------------------------------------------
	 * STEP 2 -- confirm the link answers before spending any further
	 * budget on it.
	 * ---------------------------------------------------------------
	 */
	if (!wedgepm_ping_wait(&fw, "STEP 2")) {
		printk("  ** PING never answered -- check WIFI_EN power, the SPI1 pinmux, and that "
		       "the CC3501E is running its firmware. Stopping here. **\n");
		return 0;
	}

	/*
	 * =================================================================
	 * PHASE A -- positive control, in a HEALTHY session, before anything
	 * is wedged. See the file header for why this has to run first and
	 * what it proves (or disproves).
	 * =================================================================
	 */
	printk("\n=== PHASE A: positive control (host reset in a healthy session) ===\n");

	alp_cc3501e_diag_info_t diag_before = { 0 };
	alp_status_t            diag_rc     = cc3501e_diag_info(&fw, &diag_before);
	bool                    have_before = (diag_rc == ALP_OK);
	printk("PHASE A: GET_DIAG_INFO (before reset) -> %d%s\n",
	       (int)diag_rc,
	       have_before ? "" : " -- uptime_ms UNKNOWN (v1 firmware, or a link fault)");
	if (have_before) {
		printk("PHASE A: uptime_ms BEFORE host reset = %u\n", (unsigned)diag_before.uptime_ms);
		/* fw_version -- printed so a future operator on a DIFFERENT firmware
		 * image can tell which one they have (this app itself assumes the
		 * current bench image, see the file header's "one thing you do not
		 * need to worry about" note). reset_cause is printed too, but see
		 * the frames_ok cross-check further down for why it is NOT used as
		 * a discriminator here: on this firmware a pin reset and a
		 * power-on reset both read back POWER_ON. */
		printk("PHASE A: fw_version BEFORE = 0x%04X, reset_cause BEFORE = %s (%u)\n",
		       (unsigned)diag_before.fw_version,
		       wedgepm_reset_cause_name(diag_before.reset_cause),
		       (unsigned)diag_before.reset_cause);
	}

	/* frames_ok cross-check for the uptime_ms arithmetic below -- see the
	 * comment at cause_disagrees_with_uptime's computation, further down,
	 * for the full reasoning (this app's own review record, third pass,
	 * #2035, Fix 1). Read BEFORE the reset here; read again AFTER it,
	 * alongside PHASE A's own post-reset PING, further down. */
	cc3501e_diag_stats_t stats_before      = { 0 };
	alp_status_t         stats_rc_before   = cc3501e_diag_stats(&fw, &stats_before);
	bool                 have_stats_before = (stats_rc_before == ALP_OK);
	printk("PHASE A: DIAG_GET_STATS (before reset) -> %d%s\n",
	       (int)stats_rc_before,
	       have_stats_before ? "" : " -- frames_ok UNKNOWN (v1 firmware, or a link fault)");
	if (have_stats_before) {
		printk("PHASE A: frames_ok BEFORE host reset = %u\n", (unsigned)stats_before.frames_ok);
	}

	printk("PHASE A: issuing a host hard reset (nRESET pulse, rails up) -- no wedge has "
	       "happened yet\n");
	/* Bracket the reset with the HOST's own clock -- see WEDGEPM_RESET_PROOF_MARGIN_MS's
	 * doc comment and this app's own review record (Blocker 1) for why comparing
	 * diag_after.uptime_ms against diag_before.uptime_ms ALONE is not a safe test. */
	int64_t      host_ms_before_reset = k_uptime_get();
	alp_status_t hr_rc                = cc3501e_hard_reset(&fw);
	printk("PHASE A: cc3501e_hard_reset() -> %d (this return code only proves ctx/reset_pin "
	       "are non-NULL -- cc3501e_hard_reset() always returns ALP_OK after its own delays, "
	       "it does not confirm a pulse actually reached the chip)\n",
	       (int)hr_rc);

	bool healthy_ping_after = wedgepm_ping_wait(&fw, "PHASE A (after reset)");

	alp_cc3501e_diag_info_t diag_after       = { 0 };
	bool                    have_after       = false;
	cc3501e_diag_stats_t    stats_after      = { 0 };
	bool                    have_stats_after = false;
	if (healthy_ping_after) {
		diag_rc    = cc3501e_diag_info(&fw, &diag_after);
		have_after = (diag_rc == ALP_OK);
		printk("PHASE A: GET_DIAG_INFO (after reset) -> %d%s\n",
		       (int)diag_rc,
		       have_after ? "" : " -- uptime_ms UNKNOWN (v1 firmware, or a link fault)");
		if (have_after) {
			printk("PHASE A: uptime_ms AFTER host reset  = %u\n", (unsigned)diag_after.uptime_ms);
			printk("PHASE A: fw_version AFTER  = 0x%04X, reset_cause AFTER  = %s (%u)\n",
			       (unsigned)diag_after.fw_version,
			       wedgepm_reset_cause_name(diag_after.reset_cause),
			       (unsigned)diag_after.reset_cause);
		}

		alp_status_t stats_rc_after = cc3501e_diag_stats(&fw, &stats_after);
		have_stats_after            = (stats_rc_after == ALP_OK);
		printk("PHASE A: DIAG_GET_STATS (after reset) -> %d%s\n",
		       (int)stats_rc_after,
		       have_stats_after ? "" : " -- frames_ok UNKNOWN (v1 firmware, or a link fault)");
		if (have_stats_after) {
			printk("PHASE A: frames_ok AFTER host reset  = %u\n", (unsigned)stats_after.frames_ok);
		}
	}
	int64_t  host_ms_after_reset = k_uptime_get();
	uint32_t host_elapsed_ms     = (uint32_t)(host_ms_after_reset - host_ms_before_reset);

	/*
	 * THE SINGLE MOST IMPORTANT VERDICT THIS APP PRINTS -- see the file
	 * header. Everything PHASE C observes about "the wedge survived a
	 * reset" is uninterpretable unless this says the reset actually
	 * reached the chip. Compares uptime_after against the NO-REBOOT
	 * PREDICTION (uptime_before + the host-measured elapsed time above),
	 * not against uptime_before alone -- see WEDGEPM_RESET_PROOF_MARGIN_MS.
	 */
	uint32_t predicted_if_no_reboot_ms =
	    have_before ? (diag_before.uptime_ms + host_elapsed_ms) : 0u;
	bool phase_a_reset_proven =
	    have_before && have_after &&
	    (diag_after.uptime_ms + WEDGEPM_RESET_PROOF_MARGIN_MS < predicted_if_no_reboot_ms);

	/*
	 * frames_ok cross-check (Fix 1, this app's own review record, third
	 * pass, #2035) -- REPLACES the reset_cause cross-check the second pass
	 * added. That check is UNSATISFIABLE on this firmware: verified in
	 * cc3501e-bridge-firmware's hal/ti/cc3501e_hw_ti_log.c, around the
	 * PowerWFF3_RESET_PIN_POR case -- a pin reset and a power-on reset are
	 * FOLDED into the SAME ALP_CC3501E_RESET_POWER_ON value there, per the
	 * firmware's own comment ("pin reset and POR are indistinguishable
	 * here"), and nothing in the firmware ever emits a distinct pin-reset
	 * code. PHASE A's own reset is an nRESET pulse, so reset_cause reads
	 * POWER_ON both BEFORE and AFTER on every single healthy run -- the old
	 * check treated that UNCHANGED reading as a disagreement, which cleared
	 * phase_a_comparable on every healthy board and made mechanisms 1 and 3
	 * below unreachable. reset_cause is still printed above (worth having
	 * in the transcript) but is no longer used as a discriminator.
	 *
	 * frames_ok (DIAG_GET_STATS, cc3501e_diag_stats()) does work as one: it
	 * is the firmware's own count of frames it has answered, and collapses
	 * to a handful after a genuine reboot, while a chip that never
	 * rebooted keeps counting upward through every PING this app has
	 * already issued (bring-up's STEP 2, PHASE A's own pre-reset
	 * diagnostics). Read on both sides of PHASE A's reset (above), it is
	 * INDEPENDENT corroboration for the same question the uptime
	 * arithmetic asks -- if the two genuinely disagree, that IS a real
	 * conflict and UNDETERMINED is the right call.
	 *
	 * Deliberately NOT the warm-boot counter in GET_DIAG_INFO's reserved
	 * field: that field is documented for a SOFTWARE-triggered reset and
	 * is not proven for a pin reset (this app's own review record, third
	 * pass, #2035).
	 */
	bool stats_comparable = have_stats_before && have_stats_after;
	bool frames_collapsed = stats_comparable && (stats_after.frames_ok < stats_before.frames_ok);
	bool cause_disagrees_with_uptime =
	    stats_comparable && (phase_a_reset_proven != frames_collapsed);
	/* Whether PHASE A actually proved anything -- used below AND by the
	 * final VERDICT block (Fix 2, this app's own review record, second
	 * pass, #2035): missing readings or a disagreeing corroboration signal
	 * both mean "cannot compare", and neither may be silently read as
	 * mechanism 2 confirmed. */
	bool phase_a_comparable = have_before && have_after && !cause_disagrees_with_uptime;

	if (have_before && have_after && !cause_disagrees_with_uptime) {
		printk("\nPHASE A: uptime_ms BEFORE=%u, AFTER=%u, host-measured elapsed across the "
		       "reset=%u ms, predicted AFTER if the reset did NOT reach the chip=%u, proof "
		       "margin=%u ms\n",
		       (unsigned)diag_before.uptime_ms,
		       (unsigned)diag_after.uptime_ms,
		       (unsigned)host_elapsed_ms,
		       (unsigned)predicted_if_no_reboot_ms,
		       (unsigned)WEDGEPM_RESET_PROOF_MARGIN_MS);
		printk("PHASE A VERDICT: %s\n",
		       phase_a_reset_proven
		           ? "AFTER is at least the proof margin BELOW the no-reboot prediction -- a "
		             "real reboot happened. The host's reset genuinely reaches the chip; a "
		             "later 'reset did not revive it' observation in PHASE C is real "
		             "evidence, not a false negative."
		           : "AFTER is NOT that far below the no-reboot prediction. The host's reset "
		             "does NOT drive uptime_ms back down in a HEALTHY session -- mechanism 2 "
		             "(the host's own reset sequence never actually reboots the chip) is "
		             "CONFIRMED, and every 'reset did not revive it' observation below (and "
		             "in the earlier bench run that motivated this app) is MEANINGLESS.");
	} else if (have_before && have_after) {
		/* cause_disagrees_with_uptime: two independent signals disagree.
		 * Pick NEITHER -- see this block's own comment above. */
		printk("\nPHASE A VERDICT: UNDETERMINED -- frames_ok and the uptime_ms arithmetic "
		       "DISAGREE about whether the host's reset reached the chip (frames_ok BEFORE = "
		       "%u, AFTER = %u -- %s; the uptime arithmetic alone said %s). frames_ok can "
		       "only collapse via a genuine reboot, so this is a real conflict between two "
		       "independent signals, not a rounding difference -- this app picks NEITHER "
		       "rather than assert a confident but possibly wrong verdict. Every 'reset did "
		       "not revive it' observation below should be treated with that uncertainty.\n",
		       (unsigned)stats_before.frames_ok,
		       (unsigned)stats_after.frames_ok,
		       frames_collapsed ? "collapsed, a reboot happened"
		                        : "did NOT collapse, no reboot happened",
		       phase_a_reset_proven ? "a reboot happened" : "no reboot happened");
	} else {
		printk("\nPHASE A VERDICT: could not compare uptime_ms (before=%s, after=%s) -- "
		       "cannot confirm the host reset reaches the chip. Every 'reset did not "
		       "revive it' observation below should be treated with that uncertainty.\n",
		       have_before ? "read" : "UNREAD",
		       have_after ? "read" : "UNREAD");
	}

	/*
	 * =================================================================
	 * PHASE B -- wedge it. See the file header: the connect is EXPECTED
	 * to fail. Skipped entirely (and so is PHASE C) when no credentials
	 * are configured -- see CREDENTIALS in the file header.
	 * =================================================================
	 */
	if (WEDGEPM_WIFI_SSID[0] == '\0') {
		printk("\nPHASE B: WIFI_CONNECT skipped (WEDGEPM_WIFI_SSID empty -- set it at build "
		       "time, see this file's header, to reproduce the wedge). Nothing to take a "
		       "post-mortem of. Stopping here.\n");
		return 0;
	}

	/* Never print the SSID itself -- this transcript gets pasted into issues and
	 * changelog fragments (this app's own review record, Minor 9). Length only. */
	printk("\n=== PHASE B: wedge it (WIFI_CONNECT_STA -> SSID <%u chars, redacted>, sec %u) "
	       "===\n",
	       (unsigned)strlen(WEDGEPM_WIFI_SSID),
	       (unsigned)WEDGEPM_WIFI_SECURITY);
	printk("PHASE B: this connect is EXPECTED to fail -- that is the point of this app, not "
	       "a bug to chase. Deliberately SHORT timeout budget (%u ms, not the ~55 s siblings "
	       "use) -- see WEDGEPM_CONNECT_TIMEOUT_MS's own comment for why.\n",
	       (unsigned)WEDGEPM_CONNECT_TIMEOUT_MS);
	alp_status_t connect_rc = cc3501e_wifi_connect(&fw,
	                                               WEDGEPM_WIFI_SSID,
	                                               (uint8_t)WEDGEPM_WIFI_SECURITY,
	                                               WEDGEPM_WIFI_PASS,
	                                               WEDGEPM_CONNECT_TIMEOUT_MS);
	printk("PHASE B: WIFI_CONNECT_STA -> %d\n", (int)connect_rc);

	/*
	 * Quiet wait, then EXACTLY ONE confirming PING -- see WEDGEPM_QUIET_
	 * WAIT_MS's own comment and the file header's PHASE B paragraph for
	 * why this is long while the connect budget above is short, and why
	 * the two are not in tension: the budget above stops THIS app's own
	 * driver code re-framing the link; this wait then lets that same span
	 * of time pass with the bus completely IDLE, so a healthy board gets
	 * the identical window it would have had without any of this app's
	 * shortening. Nothing is issued on the bus until the single PING below
	 * (Fix 1, this app's own review record, second pass, #2035).
	 */
	printk("PHASE B: waiting %u ms QUIETLY (no requests issued at all) -- past the "
	       "firmware's own worst-case association+DHCP window -- before a single confirming "
	       "PING\n",
	       (unsigned)WEDGEPM_QUIET_WAIT_MS);
	k_msleep(WEDGEPM_QUIET_WAIT_MS);

	alp_status_t confirm_rc = cc3501e_ping(&fw);
	printk("PHASE B: confirming PING (single attempt, after the quiet wait) -> %d\n",
	       (int)confirm_rc);
	if (confirm_rc == ALP_OK) {
		printk("\n=== WEDGE NOT REPRODUCED ===\n"
		       "PHASE B's confirming PING answered after the quiet wait -- a HEALTHY board "
		       "re-arms inside this exact window (see WEDGEPM_QUIET_WAIT_MS), so this run "
		       "never actually wedged the link. Stopping HERE, before PHASE C or any "
		       "recovery step runs: there is nothing wedged to take a post-mortem of, and "
		       "running the recovery ladder against a healthy board would print a confident "
		       "but false mechanism 1.\n");
		return 0;
	}

	/*
	 * =================================================================
	 * PHASE C -- the payload. Captured in the WEDGED state, in this
	 * exact order, BEFORE any recovery is attempted. See the file header
	 * for why the order matters. Reached only because the confirming PING
	 * above did NOT answer -- the wedge is confirmed to have reproduced
	 * this run.
	 * =================================================================
	 */
	printk("\n=== PHASE C: post-mortem, wedged state, BEFORE any power cycle ===\n");

	/* 1. rx_scratch after a PING request-header transceive. This PING is
	 *    EXPECTED to fail (the link is wedged) -- its return code is
	 *    printed but is not itself the evidence; ctx->rx_scratch is.
	 *
	 *    Fix 3, this app's own review record, third pass, #2035: PHASE B's
	 *    OWN confirming PING (above) is single-shot, and a healthy board
	 *    can fail it SPURIOUSLY -- if that happens, this PING is the first
	 *    chance to notice the board was never actually wedged. Two
	 *    INDEPENDENT pings both have to fail before this app diagnoses
	 *    anything; a success HERE must terminate the run exactly like a
	 *    success at PHASE B's own confirming PING does, not fall through
	 *    into the recovery ladder and an ambiguous mechanism-1 verdict
	 *    against a board that was never wedged. */
	alp_status_t ping_rc = cc3501e_ping(&fw);
	if (ping_rc == ALP_OK) {
		printk("PHASE C: PING -> %d (answered)\n", (int)ping_rc);
		printk("\n=== WEDGE NOT REPRODUCED ===\n"
		       "PHASE C's own first PING answered -- PHASE B's confirming PING must have "
		       "failed SPURIOUSLY, not because the link was actually wedged. Two independent "
		       "pings both have to fail before this app diagnoses anything (this app's own "
		       "review record, third pass, #2035, Fix 3). Stopping HERE, before the recovery "
		       "ladder runs against a board that was never wedged.\n");
		return 0;
	}
	uint8_t scratch[ALP_CC3501E_HEADER_BYTES];
	memcpy(scratch, fw.rx_scratch, sizeof(scratch));
	printk("PHASE C: PING -> %d\n", (int)ping_rc);
	wedgepm_report_scratch(scratch, ping_rc);

	/* 2. READY pin level, read directly as a GPIO input. fw.ready_pin is
	 *    populated by cc3501e_bridge_bringup() (see src/cc3501e_bridge.c)
	 *    when the board wires it -- this board does (P2_6, see the app
	 *    overlay). BUT: chips/cc3501e/cc3501e_core.c documents this EXACT
	 *    pad (CC35 GPIO17 -> Alif P2_6) as an OPEN CONNECTION on the bench
	 *    unit -- 0 edges in 20000 samples taken during live traffic -- and
	 *    the app opens it ALP_GPIO_PULL_NONE (src/cc3501e_bridge.c), so a
	 *    read here samples a FLOATING input. A non-NULL fw.ready_pin proves
	 *    only that a pad object exists, not that the level means anything.
	 *    Printed for completeness only -- it carries NO evidentiary weight
	 *    and does NOT appear in the VERDICT below (this app's own review
	 *    record, Major 5). */
	bool ready_high    = false;
	bool ready_read_ok = false;
	if (fw.ready_pin != NULL) {
		alp_status_t ready_rc = alp_gpio_read(fw.ready_pin, &ready_high);
		ready_read_ok         = (ready_rc == ALP_OK);
		if (ready_read_ok) {
			printk("PHASE C: READY pin = %s (sampled from a pad this repo documents as an "
			       "OPEN CONNECTION on this bench unit -- floating, PULL_NONE -- carries NO "
			       "evidentiary weight, not used in the VERDICT)\n",
			       ready_high ? "HIGH" : "LOW");
		} else {
			printk("PHASE C: READY pin read failed -> %d\n", (int)ready_rc);
		}
	} else {
		printk("PHASE C: no READY pin populated on this board (fw.ready_pin is NULL) -- "
		       "cannot read it\n");
	}

	/* 3. Host hard reset ALONE -- no power cycle yet -- then a PING. */
	printk("\nPHASE C: host hard reset (nRESET pulse, rails up) -- still wedged so far\n");
	alp_status_t warm_rc = cc3501e_hard_reset(&fw);
	printk("PHASE C: cc3501e_hard_reset() -> %d (this return code only proves ctx/reset_pin "
	       "are non-NULL -- cc3501e_hard_reset() always returns ALP_OK after its own delays, "
	       "it does not confirm a pulse actually reached the chip)\n",
	       (int)warm_rc);
	bool warm_revived = wedgepm_ping_wait(&fw, "PHASE C (after warm reset)");
	printk("PHASE C: warm reset alone %s the link\n", warm_revived ? "REVIVED" : "did NOT revive");
	if (warm_revived) {
		/* Fix 5, this app's own review record, second pass, #2035: a warm-
		 * reset revival has a SECOND documented origin besides a cleared
		 * hung task -- name it here too, not just in the final VERDICT. */
		printk("PHASE C:   a warm-reset revival alone does NOT prove mechanism 1 -- bench "
		       "precedent (cc3501e_recover()'s own doc comment, chips/cc3501e/"
		       "cc3501e_core.c, #1691) records every observed wedge of one class clearing "
		       "with `warm-reset -> 0  PING -> 0` while a firmware-side snapshot taken right "
		       "after showed the firmware had been perfectly healthy THE WHOLE TIME "
		       "(housekeeping ticks advancing, the slave armed, READY high) -- meaning the "
		       "reset only re-synced the HOST's own SPI framing state, not anything in "
		       "firmware. See the VERDICT below.\n");
	}

	/* 4. Full power-off, hold >= WEDGEPM_POWER_OFF_HOLD_MS, power-on +
	 *    reset, then a PING. Only attempted if the warm reset above did
	 *    not already revive the link -- once it answers, capturing the
	 *    heavier recovery on top would tell this app nothing more about
	 *    WHICH mechanism it was. */
	bool cold_revived          = false;
	bool power_cycle_attempted = true; /* only meaningful when the branch below runs */
	/* Hoisted to function scope (Fix 6, this app's own review record, second
	 * pass, #2035) so the final VERDICT block below can tell an
	 * ALP_ERR_VERSION reset -- which DID reach the wire -- apart from a
	 * genuine no-wire failure, instead of repeating the same false "NOT
	 * ATTEMPTED ON THE WIRE" claim this fix removes from the PHASE C print
	 * just below. Only meaningful when power_cycle_attempted is false AND
	 * warm_revived is false -- i.e. exactly when the VERDICT's own
	 * !power_cycle_attempted branch reads it. */
	alp_status_t power_cycle_on_rc = ALP_OK;
	if (!warm_revived) {
		printk("\nPHASE C: full power-off/hold/power-on -- see WEDGEPM_POWER_OFF_HOLD_MS's "
		       "own comment for why >= %u ms matters\n",
		       (unsigned)WEDGEPM_POWER_OFF_HOLD_MS);
		alp_status_t off_rc = cc3501e_power_off(&fw);
		printk("PHASE C: cc3501e_power_off() -> %d\n", (int)off_rc);
		k_msleep(WEDGEPM_POWER_OFF_HOLD_MS);
		power_cycle_on_rc = cc3501e_reset(&fw);
		printk("PHASE C: cc3501e_reset() (power-on + reset) -> %d\n", (int)power_cycle_on_rc);
		/* This app's own review record, Major 3: cc3501e_power_off() marks the ctx
		 * un-initialised, and if the FOLLOWING cc3501e_reset() does not return ALP_OK the
		 * ctx STAYS un-initialised -- every cc3501e_ping() below then short-circuits
		 * ALP_ERR_NOT_READY (cc3501e_core.c) without issuing a single SPI transaction. A
		 * "did NOT revive" verdict built on THAT is a host-side boolean, not wire evidence. */
		power_cycle_attempted = (off_rc == ALP_OK) && (power_cycle_on_rc == ALP_OK);
		if (!power_cycle_attempted) {
			if (power_cycle_on_rc == ALP_ERR_VERSION) {
				/* Fix 6, this app's own review record, second pass, #2035:
				 * ALP_ERR_VERSION (chips/cc3501e/cc3501e_core.c) means
				 * cc3501e_reset()'s own GET_VERSION round-tripped -- the
				 * chip DID answer on the wire -- but its firmware major
				 * protocol version disagrees with this host's, so
				 * cc3501e_reset() correctly refuses it and marks the link
				 * down. The PING below still short-circuits ALP_ERR_NOT_
				 * READY, same as the other no-wire cases, but NOT because
				 * nothing reached the chip -- do not read a following PING
				 * failure here as "the power cycle never reached the wire". */
				printk("PHASE C: ** power cycle DID reach the wire (off_rc=%d) -- "
				       "GET_VERSION inside cc3501e_reset() answered, so the chip came "
				       "back, but its firmware major protocol version disagrees with "
				       "this host's and cc3501e_reset() correctly refuses it "
				       "(on_rc=%d, ALP_ERR_VERSION) and marks the link down. The PING "
				       "below still short-circuits, same symptom as the true no-wire "
				       "cases below, but NOT the same cause **\n",
				       (int)off_rc,
				       (int)power_cycle_on_rc);
			} else {
				/* Fix 5, this app's own review record, third pass, #2035: no
				 * ALP_ERR_BUSY sub-case here -- cc3501e_reset() (cc3501e_core.c)
				 * returns only ALP_ERR_NOT_READY, ALP_ERR_NOSUPPORT, ALP_OK, or
				 * ALP_ERR_VERSION (handled above); a lock-acquire miss inside its
				 * own GET_VERSION read is restored and reported as ALP_OK, not
				 * surfaced to this caller. A branch documenting an unreachable
				 * code is dead sub-text, not a real case. */
				printk("PHASE C: ** power cycle NOT ATTEMPTED ON THE WIRE (off_rc=%d, "
				       "on_rc=%d) -- the PING below, if it fails, short-circuits "
				       "before clocking the bus and proves nothing **\n",
				       (int)off_rc,
				       (int)power_cycle_on_rc);
			}
		}
		cold_revived = wedgepm_ping_wait(&fw, "PHASE C (after power-cycle)");
		/* Fix 2, this app's own review record, third pass, #2035: only retry
		 * when the reset genuinely reached the wire (power_cycle_attempted)
		 * -- retrying a reset that never landed (ALP_ERR_VERSION / a lock
		 * contention, see the ** prints above) would not be a SECOND cold
		 * boot, it would just repeat the same non-attempt. */
		if (!cold_revived && power_cycle_attempted) {
			cold_revived = wedgepm_cold_boot_retry(&fw);
		}
		printk("PHASE C: full power-cycle %s the link\n",
		       cold_revived ? "REVIVED" : "did NOT revive");
	} else {
		printk("\nPHASE C: warm reset already revived the link -- skipping the full "
		       "power-cycle recovery attempt, it would add no further evidence\n");
	}

	/*
	 * =================================================================
	 * VERDICT -- one line naming which of the three mechanisms the file
	 * header lists the combined evidence points to.
	 * =================================================================
	 */
	printk("\n=== VERDICT ===\n");
	if (!phase_a_comparable) {
		/* Fix 2, this app's own review record, second pass, #2035: a MISSING
		 * reading or a reset_cause/uptime DISAGREEMENT both mean PHASE A
		 * proved nothing -- neither may be silently read as mechanism 2
		 * confirmed. See the PHASE A VERDICT line above for which case this
		 * run hit. */
		printk("VERDICT UNDETERMINED -- PHASE A's positive control did not prove whether the "
		       "host's reset reaches the chip (%s -- see the PHASE A VERDICT line above). "
		       "Every 'reset did/did not revive it' observation in PHASE C above is "
		       "UNINTERPRETABLE until this is fixed; re-run this app once PHASE A itself "
		       "passes cleanly.\n",
		       (have_before && have_after)
		           ? "frames_ok and the uptime arithmetic disagreed"
		           : "GET_DIAG_INFO could not be read before and/or after the reset");
	} else if (!phase_a_reset_proven) {
		printk("MECHANISM 2 (the host's own reset sequence never actually reboots the chip) "
		       "-- PHASE A's positive control did not show uptime_ms fall across a host "
		       "reset in a healthy session. Every 'reset did/did not revive it' observation "
		       "in PHASE C above is UNINTERPRETABLE until this is fixed; re-run this app "
		       "once the host reset path itself is confirmed to work.\n");
	} else if (warm_revived) {
		/* Fix 5, this app's own review record, second pass, #2035: do NOT
		 * let a warm-reset revival alone assert mechanism 1 -- see the
		 * PHASE C printk right after the warm-reset PING above for the
		 * bench precedent this names. */
		printk("MECHANISM 1 vs HOST-SIDE RESYNC -- AMBIGUOUS, NOT confirmed by this run "
		       "alone -- a WARM reset alone (nRESET pulse, rails up) revived the link, with "
		       "no full power cycle needed, but that observation by itself does not prove "
		       "the CC3501E firmware was ever hung. Two documented origins produce EXACTLY "
		       "this result and this run's evidence cannot separate them: (a) MECHANISM 1, "
		       "a hung firmware task the reset genuinely cleared; (b) the bench-documented "
		       "host-side resync (cc3501e_recover()'s own doc comment, chips/cc3501e/"
		       "cc3501e_core.c, #1691) -- every observed wedge of that class cleared with a "
		       "warm reset while the firmware stayed perfectly healthy throughout, meaning "
		       "the reset only re-synced the HOST's own SPI framing state.\n");
	} else if (cold_revived) {
		printk("MECHANISM 1 (a task inside the CC3501E firmware is hung, deep enough that "
		       "only a full cold boot clears it) -- the warm reset alone did NOT revive "
		       "the link, but a full power-off/hold/power-on did.\n");
	} else if (!power_cycle_attempted && power_cycle_on_rc == ALP_ERR_VERSION) {
		/* Fix 6, this app's own review record, second pass, #2035: the SAME
		 * false "NOT ATTEMPTED ON THE WIRE" claim the PHASE C print above
		 * already fixed would otherwise repeat here -- ALP_ERR_VERSION means
		 * the power cycle's own GET_VERSION round-tripped, so the chip DID
		 * come back. What is still true is that this run cannot rule
		 * MECHANISM 3 in or out: the version mismatch marks the link down
		 * before a PING could ever prove the recovered chip's SPI/DMA path
		 * itself is healthy. */
		printk("VERDICT UNDETERMINED -- the full power-off/hold/power-on DID reach the chip "
		       "(cc3501e_reset()'s own GET_VERSION round-tripped, on_rc=ALP_ERR_VERSION -- "
		       "see the ** line above), so this is NOT the no-wire case. But the firmware "
		       "major protocol version mismatch marks the link down before a PING could "
		       "prove the recovered chip's SPI/DMA path is actually healthy, so MECHANISM 3 "
		       "still cannot be ruled in or out from this run. Re-run against firmware whose "
		       "major protocol version this host accepts.\n");
	} else if (!power_cycle_attempted) {
		/* This app's own review record, Major 3: do NOT let a failed off_rc/on_rc drive
		 * the most expensive verdict this app can print. */
		printk("VERDICT UNDETERMINED -- the full power-off/hold/power-on recovery attempt "
		       "was NOT ATTEMPTED ON THE WIRE (see the ** line above: off_rc/on_rc). "
		       "MECHANISM 3 requires ruling out a power cycle that DID reach the chip and "
		       "still did not revive it, and this run never got that far. Re-run once "
		       "cc3501e_power_off()/cc3501e_reset() both return ALP_OK.\n");
	} else {
		printk("MECHANISM 3 (the wedge sits outside CC3501E firmware state entirely -- "
		       "the Alif SPI1/DMA side, or a CC3501E always-on power domain this app's own "
		       "WIFI_EN control cannot gate) -- even a full power-off/hold/power-on + reset "
		       "did NOT revive the link.\n");
	}

	return 0;
}
