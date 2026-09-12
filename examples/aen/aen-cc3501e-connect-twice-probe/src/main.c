/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-cc3501e-connect-twice-probe -- tests whether a WIFI_DISCONNECT issued
 * against a station that never associated clears the CC3501E vendor SDK's
 * WLAN_IF_DISCONNECT_IN_PROGRESS bit, for the E1M-AEN801 (Alif Ensemble E8,
 * M55-HE), bench RAM-run via J-Link.
 *
 * THE CHAIN THIS APP TESTS
 * ---------------------------
 * Read from the TI vendor SDK sources directly
 * (source/ti/net/wifi_stack/app_entry/wlan_if.c and .../cme/cme.c,
 * simplelink_wifi_sdk_10_10_01_08), not inferred:
 *
 *   1. Wlan_Disconnect() calls set_cond_in_process_wlan_discconnect(1),
 *      which sets WLAN_IF_DISCONNECT_IN_PROGRESS in the SDK's own
 *      g_oper_bitmap.
 *   2. On the STATION branch (the `default:` case of Wlan_Disconnect()'s
 *      switch, which is what a plain STA disconnect always takes) it
 *      returns WITHOUT clearing that bit -- only the AP branch and the
 *      `fail:` label clear it inline. Read the function yourself before
 *      touching this file: the station path's own `else { return ret; }`
 *      is the whole defect surface.
 *   3. The only other place that clears it is the WLAN_EVENT_DISCONNECT
 *      handler (cme.c, wlanDispatcherSendEvent(WLAN_EVENT_DISCONNECT, ...)).
 *   4. Wlan_Connect() gates on the SAME bitmap via
 *      set_cond_in_process_wlan_connect() -> is_wlan_oper_in_progress(),
 *      whose allow-mask for CONNECT is
 *      ROLE_UP_AP | ROLE_DOWN_AP | SET | GET | GET_EXCLUDE | SET_EXCLUDE.
 *      WLAN_IF_DISCONNECT_IN_PROGRESS is NOT in that mask.
 *   5. So while that bit stays set, every Wlan_Connect returns
 *      RET_OPER_IN_PROGRESS immediately -- which the bridge firmware maps
 *      to ALP_CC3501E_WIFI_FAIL_KICK.
 *
 * THE ONE UNPROVEN LINK -- and the only thing this app exists to test:
 * whether the supplicant emits WLAN_EVENT_DISCONNECT when asked to
 * disconnect a station that never associated. If it does, the bit clears
 * (mechanism 3 above fires) and none of this bites. If it does NOT, the bit
 * sticks for the rest of the session and every later connect dies at the
 * kick (mechanism 5).
 *
 * WHY THIS APP CAN OBSERVE THAT WITHOUT READING FIRMWARE STATE DIRECTLY:
 * cc3501e_wifi_connect() (chips/cc3501e/cc3501e_wifi.c) already issues a
 * WIFI_DISCONNECT at ENTRY whenever the WIFI_STATUS latch reads
 * CONN_FAILED from a previous attempt -- see that function's own #1435
 * comment. THAT disconnect is itself the call that would set the bit in
 * the first place if attempt one never associated. Attempt two's entry
 * clear is therefore exactly the real path this app exists to exercise --
 * do NOT try to bypass it or call cc3501e_wifi_disconnect() directly
 * around it; the whole point is to let cc3501e_wifi_connect()'s own,
 * already-shipped, entry-clear logic run twice back to back.
 *
 * WHY SCAN FIRST -- load-bearing, not decoration
 * --------------------------------------------------
 * On the scan-first ordering the link survives a failed connect --
 * measured, 2 of 2. Connect-first wedges it and nothing afterwards can be
 * read. Without the scan this app cannot report anything at all: STEP 3
 * below runs cc3501e_wifi_scan() and prints the record count BEFORE either
 * connect attempt, precisely because skipping it would risk losing the
 * WIFI_STATUS reads that are this app's entire payload.
 *
 * THE PREDICTION -- so the operator does not have to hold it in their head
 * ------------------------------------------------------------------------
 * If attempt one reports FAIL_TIMEOUT and attempt two reports FAIL_KICK,
 * the bit sticks and the hypothesis is CONFIRMED. If both report the SAME
 * reason, it does not stick and the hypothesis is DEAD. If attempt one
 * SUCCEEDS, this run says nothing about any of this -- there was no
 * failure to leave the bit set behind in the first place. STEP 6 below
 * prints exactly this and nothing more; the reasoning belongs to whoever
 * reads the transcript, not to this app.
 *
 * SECURITY VALUE ON THE WIRE -- do not mix the two encodings up
 * --------------------------------------------------------------
 * cc3501e_wifi_connect()'s own `sec_type` parameter goes on the wire as
 * <alp/protocol/cc3501e.h>'s alp_cc3501e_wifi_connect_t::security field:
 * 0 = open, 1 = WPA2-PSK, 2 = WPA3-SAE (CC3501E_WIFI_CONNECT_SEC_OPEN /
 * _WPA2_PSK / _WPA3_SAE in <alp/chips/cc3501e/wifi.h>). This is a DIFFERENT
 * encoding from the scan-RESULT security enum in that same header
 * (cc3501e_wifi_sec_t), where 1 means WEP -- this app never touches that
 * second enum (it only prints scan record COUNT, not per-record security),
 * but a reader porting this pattern elsewhere should not conflate the two.
 *
 * CREDENTIALS -- never committed
 * ---------------------------------
 * Build-time defines, empty defaults, this app's OWN macro prefix
 * (CONNTWICE_*) so a combined build cannot collide with
 * aen-cc3501e-companion-tour's TOUR_*, aen-cc3501e-socket-throughput's
 * SOCKTP_*, aen-cc3501e-bringup's CC3501E_WIFI_*, or
 * aen-cc3501e-wedge-postmortem's WEDGEPM_*. No SSID or passphrase is
 * embedded anywhere in this file. When CONNTWICE_WIFI_SSID is empty, main()
 * says so and stops before either connect attempt runs -- there would be
 * nothing to disconnect and nothing to observe.
 *
 * WHAT THIS APP NEVER DOES
 * ---------------------------
 * Never OTA_*, never RESET, never a soft-AP -- the only opcodes this file
 * issues are WIFI_SCAN_START (via cc3501e_wifi_scan(), STEP 3),
 * WIFI_CONNECT_STA (via cc3501e_wifi_connect(), STEPs 4 and 5, which itself
 * issues WIFI_DISCONNECT at entry when required -- see above), and
 * WIFI_STATUS (via cc3501e_wifi_status(), read after each attempt). Never
 * runs hardware beyond the bench RAM-run this file's own header line names.
 *
 * Build target: alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he -- same
 * overlay memory placement and CONFIG_DCACHE=n as
 * aen-cc3501e-command-sweep; see this app's README.md for the full west
 * build invocation and that sibling's overlay for why the memory placement
 * is load-bearing, not decorative.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "alp/chips/cc3501e.h"
#include "cc3501e_bridge.h" /* cc3501e_bridge_bringup() -- the SoM bring-up template */

/*
 * ======================================================================
 * Tunables -- timeouts and the (build-time) Wi-Fi credentials the file
 * header above explains.
 * ======================================================================
 */

/* Same shape and reasoning as every aen-cc3501e-* sibling's own PING retry
 * loop (e.g. aen-cc3501e-socket-throughput's SOCKTP_PING_RETRIES): the
 * bring-up call already waited out its own boot budget once, so this loop
 * only absorbs residual ramp/boot jitter. */
#define CONNTWICE_PING_RETRIES 25u
#define CONNTWICE_PING_GAP_MS  200u

/* Floored internally to 20 s by cc3501e_wifi_scan() regardless (see that
 * function's own doc) -- this asks for a little more so the literal here
 * means what it says rather than being silently corrected, same reasoning
 * as aen-cc3501e-command-sweep's identical 25 s scan budget. */
#define CONNTWICE_SCAN_TIMEOUT_MS 25000u

/* Full connect budget, NOT a shortened one -- unlike
 * aen-cc3501e-wedge-postmortem's deliberately-shrunk WEDGEPM_CONNECT_
 * TIMEOUT_MS (that app wants to catch the link mid-wedge; this app wants
 * each attempt to reach a genuine TERMINAL WIFI_STATUS so the fail_reason
 * pair it reports is real, not an artifact of giving up early). Same
 * derivation as aen-cc3501e-socket-throughput's SOCKTP_CONNECT_TIMEOUT_MS:
 * the firmware's own worst case is 30 s L2 association
 * (hal/ti/cc3501e_hw_ti_wifi.c) + 10 s DHCP = 40 s, plus a 15 s
 * reinitialisation margin. */
#define CONNTWICE_CONNECT_TIMEOUT_MS 55000u

/*
 * Wi-Fi STA credentials for both connect attempts. DELIBERATELY EMPTY by
 * default -- never hardcode bench credentials in a public example. Set
 * them at build time WITHOUT editing this file, e.g.:
 *
 *   west build ... -- -DEXTRA_CFLAGS="-DCONNTWICE_WIFI_SSID=\\\"myssid\\\" \
 *                                     -DCONNTWICE_WIFI_PASS=\\\"mypass\\\""
 *
 * When CONNTWICE_WIFI_SSID is empty, main() prints one line saying so and
 * returns before STEP 3 (the scan) even runs -- there is nothing to
 * connect twice, so there is nothing for this app to observe.
 */
#ifndef CONNTWICE_WIFI_SSID
#define CONNTWICE_WIFI_SSID ""
#endif
#ifndef CONNTWICE_WIFI_PASS
#define CONNTWICE_WIFI_PASS ""
#endif
/* Security selector on the WIRE encoding: 0 = open, 1 = WPA2-PSK,
 * 2 = WPA3-SAE (CC3501E_WIFI_CONNECT_SEC_* in <alp/chips/cc3501e/wifi.h>) --
 * see the file header's SECURITY VALUE note for why this is NOT the same
 * numbering as the scan-result security enum. */
#ifndef CONNTWICE_WIFI_SECURITY
#define CONNTWICE_WIFI_SECURITY CC3501E_WIFI_CONNECT_SEC_WPA2_PSK
#endif

/** @brief Human name for an @ref alp_cc3501e_wifi_conn_state_t value. */
static const char *conntwice_state_name(uint8_t state)
{
	switch (state) {
	case ALP_CC3501E_WIFI_DISCONNECTED:
		return "DISCONNECTED";
	case ALP_CC3501E_WIFI_CONNECTING:
		return "CONNECTING";
	case ALP_CC3501E_WIFI_CONNECTED:
		return "CONNECTED";
	case ALP_CC3501E_WIFI_CONN_FAILED:
		return "CONN_FAILED";
	default:
		return "UNKNOWN";
	}
}

/** @brief Human name for an @ref alp_cc3501e_wifi_fail_t value. */
static const char *conntwice_fail_name(uint8_t fail_reason)
{
	switch (fail_reason) {
	case ALP_CC3501E_WIFI_FAIL_NONE:
		return "FAIL_NONE";
	case ALP_CC3501E_WIFI_FAIL_TIMEOUT:
		return "FAIL_TIMEOUT";
	case ALP_CC3501E_WIFI_FAIL_REJECTED:
		return "FAIL_REJECTED";
	case ALP_CC3501E_WIFI_FAIL_KICK:
		return "FAIL_KICK";
	default:
		return "UNKNOWN";
	}
}

/**
 * @brief Run one WIFI_CONNECT_STA attempt and report the terminal WIFI_STATUS.
 *
 * Deliberately does NOT return a bare @c alp_status_t from
 * cc3501e_wifi_connect() as this app's verdict -- that return code collapses
 * TIMEOUT vs REJECTED vs KICK into two bare error codes (see the file
 * header). The independent @ref cc3501e_wifi_status read after it is what
 * this app actually cares about.
 *
 * @param fw          Initialised bridge handle.
 * @param attempt_no  1 or 2, for the printed labels only.
 * @param status_out  Receives the decoded WIFI_STATUS snapshot (zeroed if
 *                     the status read itself failed).
 * @return The @ref cc3501e_wifi_status return code -- ALP_OK if @p status_out
 *         is valid, otherwise the status read's own error.
 */
static alp_status_t
conntwice_attempt(cc3501e_t *fw, unsigned attempt_no, alp_cc3501e_wifi_status_t *status_out)
{
	printk("\nSTEP %u: CONNECT attempt %u -> SSID \"%s\" (sec %u)...\n",
	       3u + attempt_no,
	       attempt_no,
	       CONNTWICE_WIFI_SSID,
	       (unsigned)CONNTWICE_WIFI_SECURITY);

	int64_t      t0 = k_uptime_get();
	alp_status_t rc = cc3501e_wifi_connect(fw,
	                                       CONNTWICE_WIFI_SSID,
	                                       (uint8_t)CONNTWICE_WIFI_SECURITY,
	                                       CONNTWICE_WIFI_PASS,
	                                       CONNTWICE_CONNECT_TIMEOUT_MS);
	int64_t      dt = k_uptime_get() - t0;
	printk("  cc3501e_wifi_connect() -> %d (elapsed_ms=%lld) -- this bare return code alone"
	       " collapses TIMEOUT/REJECTED/KICK; see WIFI_STATUS below for the real reason.\n",
	       (int)rc,
	       (long long)dt);

	alp_cc3501e_wifi_status_t st = { 0 };
	alp_status_t              sr = cc3501e_wifi_status(fw, &st);
	if (sr == ALP_OK) {
		printk("  WIFI_STATUS -> state=%u (%s)  fail_reason=%u (%s)\n",
		       (unsigned)st.state,
		       conntwice_state_name(st.state),
		       (unsigned)st.fail_reason,
		       conntwice_fail_name(st.fail_reason));
	} else {
		printk("  WIFI_STATUS read FAILED (rc=%d) -- state/fail_reason UNREAD for this"
		       " attempt.\n",
		       (int)sr);
	}
	if (status_out != NULL) {
		*status_out = st;
	}
	return sr;
}

/* ---------------------------------------------------------------------- */

int main(void)
{
	printk("\n=== AEN801 CC3501E connect-twice probe (#2035) ===\n");

	/*
	 * STATIC, explicitly zero-initialised cc3501e_t -- same discipline as
	 * every aen-cc3501e-* sibling, for the identical reason: this type
	 * embeds several ALP_CC3501E_MAX_PAYLOAD scratch buffers (~32 KB
	 * total). Declared automatic it asks main() for a multi-kilobyte
	 * stack frame, and Zephyr's stack-overflow check fires on the very
	 * first `sub sp` -- before this function's own first printk, so the
	 * app dies with no output at all and looks like a dead board. That is
	 * not hypothetical: it happened on the M55-HE 2026-09-10 and cost a
	 * bench run (see aen-cc3501e-handshake-probe's own prj.conf).
	 */
	static cc3501e_t fw = { 0 };

	/*
	 * ---------------------------------------------------------------
	 * STEP 1 -- bring the bridge up, byte-identical to aen-evk-demo
	 * phase 8 and every aen-cc3501e-* sibling (same
	 * src/cc3501e_bridge.{c,h} template, same overlay).
	 * ---------------------------------------------------------------
	 */
	alp_status_t rc = cc3501e_bridge_bringup(&fw);
	printk("STEP 1: bridge bring-up (WIFI_EN high, nRESET pulsed, SPI1 @ %u Hz) -> %d\n",
	       (unsigned)CC3501E_BRIDGE_SPI_FREQ_HZ,
	       (int)rc);
	if (rc != ALP_OK) {
		printk("  ** bring-up did NOT return ALP_OK -- nothing downstream of it can work."
		       " Stopping here. **\n");
		return 0;
	}

	/*
	 * ---------------------------------------------------------------
	 * STEP 2 -- confirm the link answers before spending a scan/connect
	 * budget on it. Same retry shape as aen-cc3501e-socket-throughput's
	 * own STEP 2.
	 * ---------------------------------------------------------------
	 */
	bool up = false;
	for (unsigned i = 0u; i < CONNTWICE_PING_RETRIES; i++) {
		if (cc3501e_ping(&fw) == ALP_OK) {
			printk("STEP 2: PING ok after %u attempt%s\n", i + 1u, (i == 0u) ? "" : "s");
			up = true;
			break;
		}
		k_msleep(CONNTWICE_PING_GAP_MS);
	}
	if (!up) {
		printk("STEP 2: PING never answered -- check WIFI_EN power, the SPI1 pinmux, and"
		       " that the CC3501E is running its firmware. Stopping here.\n");
		return 0;
	}

	if (CONNTWICE_WIFI_SSID[0] == '\0') {
		printk("\nNo SSID configured (CONNTWICE_WIFI_SSID empty -- set it at build time,"
		       " see this file's header). Nothing to connect twice, nothing to observe."
		       " Stopping here.\n");
		return 0;
	}

	/*
	 * ---------------------------------------------------------------
	 * STEP 3 -- scan FIRST. Load-bearing, not decoration: on the
	 * scan-first ordering the link survives a failed connect (measured,
	 * 2 of 2); connect-first wedges it and nothing afterwards can be
	 * read. See the file header's WHY SCAN FIRST section.
	 * ---------------------------------------------------------------
	 */
	static cc3501e_scan_record_t scan_records[8];
	size_t                       scan_count = 0u;
	alp_status_t                 scan_rc    = cc3501e_wifi_scan(
	    &fw, scan_records, ARRAY_SIZE(scan_records), &scan_count, CONNTWICE_SCAN_TIMEOUT_MS);
	printk("STEP 3: WIFI_SCAN_START -> %d, %u record(s) (capped at %u)\n",
	       (int)scan_rc,
	       (unsigned)scan_count,
	       (unsigned)ARRAY_SIZE(scan_records));
	if (scan_rc != ALP_OK) {
		printk("  ** scan did not return ALP_OK -- proceeding to the connect attempts"
		       " anyway (this app never stops early on a failed step: a failed step's"
		       " own outcome is data), but the scan-first protection this app relies on"
		       " may not have run to completion. **\n");
	}

	/*
	 * ---------------------------------------------------------------
	 * STEP 4 / STEP 5 -- the two connect attempts, same credentials,
	 * back to back. See conntwice_attempt() and the file header's THE
	 * ONE UNPROVEN LINK section for what each one is exercising.
	 * ---------------------------------------------------------------
	 */
	alp_cc3501e_wifi_status_t st1        = { 0 };
	alp_status_t              st1_status = conntwice_attempt(&fw, 1u, &st1);

	alp_cc3501e_wifi_status_t st2        = { 0 };
	alp_status_t              st2_status = conntwice_attempt(&fw, 2u, &st2);

	/*
	 * ---------------------------------------------------------------
	 * STEP 6 -- the plain reading. Prints the pair and what the pair
	 * means, per the three cases the file header names; nothing beyond
	 * that is concluded here (see the file header's closing note).
	 * ---------------------------------------------------------------
	 */
	printk("\nSTEP 6: the reading\n");
	printk("  attempt 1: state=%s fail_reason=%s\n",
	       (st1_status == ALP_OK) ? conntwice_state_name(st1.state) : "UNREAD",
	       (st1_status == ALP_OK) ? conntwice_fail_name(st1.fail_reason) : "UNREAD");
	printk("  attempt 2: state=%s fail_reason=%s\n",
	       (st2_status == ALP_OK) ? conntwice_state_name(st2.state) : "UNREAD",
	       (st2_status == ALP_OK) ? conntwice_fail_name(st2.fail_reason) : "UNREAD");

	if (st1_status != ALP_OK || st2_status != ALP_OK) {
		printk("  VERDICT: at least one WIFI_STATUS read failed -- the pair above is"
		       " incomplete, so no verdict can be read off it this run.\n");
	} else if (st1.state == ALP_CC3501E_WIFI_CONNECTED) {
		printk("  VERDICT: attempt one SUCCEEDED -- this run says nothing about the"
		       " disconnect-bit hypothesis. There was no failure on attempt one to leave"
		       " the bit set behind in the first place.\n");
	} else if (st1.state != ALP_CC3501E_WIFI_CONN_FAILED) {
		printk("  VERDICT: attempt one did not reach a terminal CONNECTED/CONN_FAILED"
		       " state within its own %u ms budget (state=%s) -- cannot evaluate the"
		       " hypothesis this run.\n",
		       (unsigned)CONNTWICE_CONNECT_TIMEOUT_MS,
		       conntwice_state_name(st1.state));
	} else if (st2.state == ALP_CC3501E_WIFI_CONNECTED) {
		printk("  VERDICT: attempt one FAILED (fail_reason=%s) but attempt two SUCCEEDED"
		       " -- the second connect was not blocked, whatever the disconnect bit's"
		       " state actually was. This neither confirms nor refutes the hypothesis by"
		       " itself; printing the raw pair above for the record.\n",
		       conntwice_fail_name(st1.fail_reason));
	} else if (st2.state != ALP_CC3501E_WIFI_CONN_FAILED) {
		printk("  VERDICT: attempt one FAILED (fail_reason=%s) but attempt two did not"
		       " reach a terminal state within its own %u ms budget (state=%s) -- cannot"
		       " classify the pair this run.\n",
		       conntwice_fail_name(st1.fail_reason),
		       (unsigned)CONNTWICE_CONNECT_TIMEOUT_MS,
		       conntwice_state_name(st2.state));
	} else if (st1.fail_reason == ALP_CC3501E_WIFI_FAIL_TIMEOUT &&
	           st2.fail_reason == ALP_CC3501E_WIFI_FAIL_KICK) {
		printk("  VERDICT: CONFIRMED. Attempt one timed out (FAIL_TIMEOUT) and attempt two"
		       " died at the kick (FAIL_KICK) with the identical credentials -- the"
		       " disconnect-in-progress bit stuck across the two attempts, exactly the"
		       " predicted pattern (see this file's header, THE CHAIN THIS APP TESTS).\n");
	} else if (st1.fail_reason == st2.fail_reason) {
		printk("  VERDICT: DEAD. Both attempts failed with the SAME reason (%s) -- the bit"
		       " does not stick across a station-never-associated disconnect; the chain"
		       " this app tests does not bite here.\n",
		       conntwice_fail_name(st1.fail_reason));
	} else {
		printk("  VERDICT: neither predicted pattern -- attempt one failed with %s,"
		       " attempt two with %s. Not TIMEOUT-then-KICK, not identical. Printing the"
		       " pair for manual review; no verdict beyond it is claimed here.\n",
		       conntwice_fail_name(st1.fail_reason),
		       conntwice_fail_name(st2.fail_reason));
	}

	return 0;
}
