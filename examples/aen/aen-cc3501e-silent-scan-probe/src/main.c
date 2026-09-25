/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-cc3501e-silent-scan-probe -- tests whether a single WIFI_SCAN_START
 * submit survives 25 seconds of a completely silent host bus, for the
 * E1M-AEN801 (Alif Ensemble E8, M55-HE), bench RAM-run via J-Link.
 *
 * WHY THIS APP EXISTS -- separating two readings of the WIFI_SCAN_START wedge
 * ------------------------------------------------------------------------
 * WIFI_SCAN_START wedges the CC3501E bridge on this bench. Root-cause
 * analysis has narrowed it to two readings every measurement so far is
 * equally consistent with:
 *
 *   B.     The firmware brackets its station role-up with a bridge quiesce,
 *          and the quiesce hangs. The quiesce cancels an armed
 *          callback-mode transfer, and the firmware's own notes record
 *          three separate bench occasions where doing that WHILE THE HOST
 *          WAS CLOCKING THE BUS locked the core up. The one configuration
 *          in which it ever worked was at boot, with no host traffic to
 *          disrupt.
 *   Other. The heavy radio operation itself does not return within its
 *          bound, in which case host traffic is irrelevant.
 *
 * These separate cleanly on one observation: under B, if the host puts
 * NOTHING AT ALL on the bus while the firmware does its radio work, the
 * cancel operates on an idle armed transfer with no host bytes in flight --
 * the same conditions under which it historically worked -- so the radio
 * work completes, the bridge re-initialises, and the link comes back. Under
 * the OTHER reading the bus stays dead regardless of how quiet the host is.
 *
 * The standard driver call (@ref cc3501e_wifi_scan) cannot test this: it
 * submits and then polls status every 50 ms for its whole budget, and THAT
 * POLLING IS THE TRAFFIC UNDER SUSPICION. This app instead submits once
 * through the raw @ref cc3501e_request path (the same submit-once shape
 * @ref cc3501e_wifi_ap_start uses against WIFI_AP_START,
 * chips/cc3501e/cc3501e_wifi.c, around lines 513-514) and then issues
 * NOTHING AT ALL -- not even a status poll -- for 25 seconds. See STEP 3
 * below for exactly what "nothing at all" means and why it is safe to trust.
 *
 * THE SILENCE IS THE EXPERIMENT. Any instrumentation added to this file
 * that clocks the bus during STEP 3's k_msleep() destroys the result -- see
 * that step's own comment before touching it.
 *
 * WHAT THIS RUN CANNOT PROVE, AND WHEN IT SAYS SO
 * ------------------------------------------------
 * The submit in STEP 2 has to actually land on the wire before the silence
 * means anything: if it comes back as an I/O error or a timeout rather than
 * the expected busy-style acknowledgement, the submit frame itself did not
 * complete, and 25 seconds of silence afterwards proves nothing about
 * either reading above. This app checks for exactly that and prints
 * `=== VOID RUN ===` instead of proceeding -- see STEP 2 below.
 *
 * CREDENTIALS -- none needed
 * ---------------------------
 * A scan takes no SSID or passphrase, and that is part of why it is the
 * right probe here: unlike aen-cc3501e-wedge-postmortem's WIFI_CONNECT_STA
 * repro, this app needs no build-time credential defines and commits none.
 *
 * Build target: alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he -- see this
 * app's README.md for the full west build invocation and the overlay's own
 * header for why the SRAM0 memory placement and CONFIG_DCACHE=n are
 * load-bearing, not decorative: this app's own submit + both PINGs ride the
 * identical SPI1 link every other AEN801 CC3501E bench app measures, and a
 * faster cache-on/DTCM path would shift timing on precisely the axis the
 * 25-second silence exists to hold constant.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "alp/chips/cc3501e.h"
#include "alp/peripheral.h" /* alp_status_name() */
#include "cc3501e_bridge.h" /* cc3501e_bridge_bringup() -- the SoM bring-up template */

/*
 * ======================================================================
 * Tunables
 * ======================================================================
 */

/* Same shape as every aen-cc3501e-* sibling's own PING retry loop (e.g.
 * aen-cc3501e-wedge-postmortem's WEDGEPM_PING_RETRIES): the bring-up call
 * this app makes already waited out its own boot budget once, so this loop
 * only absorbs residual ramp/boot jitter. Reused for both PINGs this app
 * issues (STEP 1's bring-up confirmation and STEP 4's post-silence PING). */
#define SSCANPROBE_PING_RETRIES 25u
#define SSCANPROBE_PING_GAP_MS  200u

/* Mirrors CC3501E_REQ_TMO_MS (chips/cc3501e/cc3501e_internal.h line 19) --
 * the SAME per-attempt reply-wait budget cc3501e_wifi_ap_start() passes to
 * its own single-shot, non-retried cc3501e_request() call (chips/cc3501e/
 * cc3501e_wifi.c, around lines 513-514), the shape this app's own
 * WIFI_SCAN_START submit copies. Not #include'd from the driver's private
 * cc3501e_internal.h -- that header is explicitly "not installed, not part
 * of the public API" (its own file header) -- so this app carries its own
 * copy of the value, same as every aen-cc3501e-* sibling defines its own
 * tunables rather than reaching into driver internals. */
#define SSCANPROBE_SUBMIT_TMO_MS 100u

/*
 * THE LOAD-BEARING CONSTANT. How long STEP 3 issues NOTHING AT ALL on the
 * bus after the STEP 2 submit is acknowledged -- not a status poll, not a
 * ping, not a diagnostic read. This is the entire experiment (see this
 * file's own header): a completely silent bus during the firmware's radio
 * work is the one condition mechanism B's own bench notes record as
 * historically safe. Do NOT shrink this hunting for a faster run, and do
 * NOT add any driver call inside the k_msleep() this constant bounds -- see
 * STEP 3's own comment. */
#define SSCANPROBE_SILENCE_MS 25000u

/* Step 5's ordinary scan budget -- the task's own figure, generously above
 * every aen-cc3501e-* sibling's connect-family budget derivation (40 s of
 * association+DHCP headroom, see aen-cc3501e-wedge-postmortem's
 * WEDGEPM_QUIET_WAIT_MS comment for that derivation) because if the role
 * really did latch from STEP 2's submit, this second scan performs no
 * role-up at all and should return quickly; if it did not, this budget is
 * generous enough not to call a slow-but-alive scan a failure. */
#define SSCANPROBE_SCAN_TIMEOUT_MS 40000u

/* Scan-record capacity for STEP 5's normal wrapper call. Matches
 * aen-cc3501e-companion-tour's own TOUR_SCAN_MAX -- generous for a bench
 * scan, not a hard protocol limit. */
#define SSCANPROBE_SCAN_MAX 16u

/*
 * ======================================================================
 * PING helper -- same shape as every aen-cc3501e-* sibling's own PING loop.
 * ======================================================================
 */

/** PING with a bounded retry loop, printing the attempt count. @p label
 *  prefixes the console line so the transcript names which step's PING
 *  this is (STEP 1's bring-up confirmation vs STEP 4's post-silence one). */
static bool sscanprobe_ping_wait(cc3501e_t *fw, const char *label)
{
	for (unsigned i = 0u; i < SSCANPROBE_PING_RETRIES; i++) {
		if (cc3501e_ping(fw) == ALP_OK) {
			printk("%s: PING ok after %u attempt%s\n", label, i + 1u, (i == 0u) ? "" : "s");
			return true;
		}
		k_msleep(SSCANPROBE_PING_GAP_MS);
	}
	printk("%s: PING never answered after %u attempts\n", label, (unsigned)SSCANPROBE_PING_RETRIES);
	return false;
}

/* ---------------------------------------------------------------------- */

int main(void)
{
	printk("\n=== AEN801 CC3501E silent-scan probe ===\n");

	/*
	 * STATIC, explicitly zero-initialised cc3501e_t -- same discipline as
	 * every aen-cc3501e-* sibling, for the identical reason: this type
	 * embeds several ALP_CC3501E_MAX_PAYLOAD scratch buffers (~32 KB
	 * total). Declared automatic it asks main() for a multi-kilobyte
	 * stack frame, and Zephyr's stack-overflow check fires on the very
	 * first `sub sp` -- before this function's own first printk. Measured
	 * on the M55-HE 2026-09-10 by aen-cc3501e-handshake-probe (32908-byte
	 * frame); this app inherits the same fix rather than rediscovering
	 * the same failure.
	 */
	static cc3501e_t fw = { 0 };

	/*
	 * ---------------------------------------------------------------
	 * STEP 1 -- bring the bridge up, then confirm with a PING, exactly
	 * like every aen-cc3501e-* sibling's own bring-up (same
	 * src/cc3501e_bridge.{c,h} template, same overlay).
	 * ---------------------------------------------------------------
	 */
	alp_status_t rc = cc3501e_bridge_bringup(&fw);
	printk("STEP 1: bridge bring-up (WIFI_EN high, nRESET pulsed, SPI1 @ %u Hz) -> %d (%s)\n",
	       (unsigned)CC3501E_BRIDGE_SPI_FREQ_HZ,
	       (int)rc,
	       alp_status_name(rc));
	if (rc != ALP_OK) {
		printk("  ** bring-up did NOT return ALP_OK -- nothing downstream of it can work. "
		       "Stopping here. **\n");
		return 0;
	}
	if (!sscanprobe_ping_wait(&fw, "STEP 1")) {
		printk("  ** PING never answered -- check WIFI_EN power, the SPI1 pinmux, and that "
		       "the CC3501E is running its firmware. Stopping here. **\n");
		return 0;
	}

	/*
	 * ---------------------------------------------------------------
	 * STEP 2 -- submit WIFI_SCAN_START EXACTLY ONCE, through the raw
	 * cc3501e_request() path rather than the polling wrapper
	 * (cc3501e_wifi_scan(), which re-issues WIFI_SCAN_START on a 50 ms
	 * poll cadence for its whole budget -- see this file's own header
	 * for why that polling is exactly the traffic under suspicion here,
	 * and must not run before the silence in STEP 3).
	 *
	 * This follows cc3501e_wifi_ap_start()'s own submit-once shape
	 * (chips/cc3501e/cc3501e_wifi.c, around lines 513-514): a single,
	 * non-retried cc3501e_request() call with no reply buffer (the scan
	 * result payload is not wanted here -- STEP 5's normal wrapper reads
	 * it later, once the role is confirmed latched) and a fixed
	 * SSCANPROBE_SUBMIT_TMO_MS budget.
	 * ---------------------------------------------------------------
	 */
	printk("\nSTEP 2: submit WIFI_SCAN_START once (raw cc3501e_request(), no polling)\n");
	alp_status_t submit_rc = cc3501e_request(
	    &fw, ALP_CC3501E_CMD_WIFI_SCAN_START, NULL, 0, NULL, 0, NULL, SSCANPROBE_SUBMIT_TMO_MS);
	printk(
	    "STEP 2: WIFI_SCAN_START submit -> %d (%s)\n", (int)submit_rc, alp_status_name(submit_rc));

	/*
	 * The busy-style acknowledgement (ALP_ERR_BUSY, wire RESP_ERR_BUSY --
	 * see cc3501e_reply_verdict(), chips/cc3501e/cc3501e_core.c) is the
	 * ONLY outcome this app treats as "the job was accepted": it means the
	 * firmware's worker actually queued the scan. An I/O error or a
	 * timeout means the submit frame itself did not complete -- the
	 * firmware never got the request, or the reply never came back
	 * readable -- and the 25-second silence that follows would then be
	 * silence around NOTHING, proving neither reading in this file's own
	 * header. This app does not try to guess past that: it declares the
	 * run VOID and stops, rather than let a silent bus after a failed
	 * submit be misread as "the link survived the silence".
	 */
	if (submit_rc != ALP_ERR_BUSY) {
		printk("\n=== VOID RUN ===\n"
		       "STEP 2's submit did NOT get the expected busy-style acknowledgement "
		       "(ALP_ERR_BUSY / RESP_ERR_BUSY) -- it returned %s (%d) instead. The submit "
		       "frame itself did not complete, so this run proves NOTHING about either "
		       "reading in this app's own file header: not that the link survives a silent "
		       "bus during the radio work, and not that it doesn't. Stopping here rather "
		       "than continue into a silence whose premise already failed.\n",
		       alp_status_name(submit_rc),
		       (int)submit_rc);
		return 0;
	}
	printk("STEP 2: accepted (busy-style ack) -- the firmware's scan worker is now running\n");

	/*
	 * ---------------------------------------------------------------
	 * STEP 3 -- THE EXPERIMENT. Issue NOTHING AT ALL for 25 seconds:
	 * no status poll, no ping, no diagnostic read, no driver call of
	 * any kind. See this file's own header for why this exact
	 * condition is what separates the two live readings of the wedge.
	 *
	 * DO NOT ADD ANY CALL BETWEEN THIS COMMENT AND THE k_msleep() BELOW.
	 * Any cc3501e_* call here -- even a read-only one -- clocks the SPI1
	 * bus and reintroduces the exact host traffic this probe exists to
	 * rule out; it would not be a stronger test, it would be a
	 * DIFFERENT, uninformative one indistinguishable from the polling
	 * wrapper's own 50 ms cadence this app was built specifically to
	 * avoid.
	 *
	 * The driver's own housekeeping must not run either, and it does
	 * not: chips/cc3501e/ and the backends it attaches (src/backends/)
	 * carry no k_timer, no k_work / k_work_delayable, and no periodic
	 * poll of any kind (grepped for k_timer / k_work_schedule /
	 * k_work_submit / k_work_init / K_WORK_DELAYABLE_DEFINE /
	 * K_TIMER_DEFINE across both trees, 2026-09-11 -- none matched
	 * outside src/backends/power/zephyr_pm_policy.c, which is an
	 * unrelated PM-policy TU this app's prj.conf never pulls in). Every
	 * request this driver issues is caller-driven; nothing left armed
	 * by STEP 1's bring-up or STEP 2's submit clocks the bus on its own
	 * while this k_msleep() runs. CONFIG_ALP_SDK_CONSOLE=n (prj.conf)
	 * additionally keeps the shell thread out of existence for the same
	 * reason, even though the shell would talk to the E1M UART console,
	 * not this SPI1 link -- unrequested background activity of ANY kind
	 * during this window is worth eliminating on principle here.
	 * ---------------------------------------------------------------
	 */
	printk("\nSTEP 3: silence -- issuing NOTHING for %u ms (no polling, no ping, no "
	       "diagnostic read). This silence is the experiment; see this file's own header.\n",
	       (unsigned)SSCANPROBE_SILENCE_MS);
	k_msleep(SSCANPROBE_SILENCE_MS);
	printk("STEP 3: silence complete\n");

	/*
	 * ---------------------------------------------------------------
	 * STEP 4 -- a single PING. This is the first thing to touch the bus
	 * since STEP 2's submit.
	 * ---------------------------------------------------------------
	 */
	printk("\nSTEP 4: single confirming PING (first bus activity since the STEP 2 submit)\n");
	bool survived_silence = sscanprobe_ping_wait(&fw, "STEP 4");

	/*
	 * ---------------------------------------------------------------
	 * STEP 5 -- a normal scan through the ordinary polling wrapper, now
	 * that the silence is over. If the role latched from STEP 2's
	 * submit, the firmware should already have scan results cached and
	 * this call performs no role-up of its own -- it should return
	 * promptly with records rather than block for the whole budget.
	 * ---------------------------------------------------------------
	 */
	printk("\nSTEP 5: normal Wi-Fi scan (cc3501e_wifi_scan(), %u ms budget)\n",
	       (unsigned)SSCANPROBE_SCAN_TIMEOUT_MS);
	static cc3501e_scan_record_t records[SSCANPROBE_SCAN_MAX];
	size_t                       count = 0;
	alp_status_t                 scan_rc =
	    cc3501e_wifi_scan(&fw, records, SSCANPROBE_SCAN_MAX, &count, SSCANPROBE_SCAN_TIMEOUT_MS);
	printk("STEP 5: cc3501e_wifi_scan() -> %d (%s), record count = %u\n",
	       (int)scan_rc,
	       alp_status_name(scan_rc),
	       (unsigned)count);
	bool got_records = (scan_rc == ALP_OK) && (count > 0u);
	for (size_t i = 0; i < count; i++) {
		const cc3501e_scan_record_t *r = &records[i];
		printk("STEP 5:   [%2u] ssid=\"%s\" sec=%s rssi=%d dBm channel=%u\n",
		       (unsigned)i,
		       r->ssid,
		       cc3501e_wifi_sec_name(r->security_info),
		       (int)r->rssi_dbm,
		       (unsigned)r->channel);
	}

	/*
	 * ---------------------------------------------------------------
	 * READING -- plain summary of the two observations this app exists
	 * to make: did the link survive the silence, and did the second
	 * scan return records.
	 * ---------------------------------------------------------------
	 */
	printk("\n=== READING ===\n");
	printk("Link survived the 25 s silence: %s\n", survived_silence ? "YES" : "NO");
	printk("Second scan (STEP 5) returned records: %s (%u record%s)\n",
	       got_records ? "YES" : "NO",
	       (unsigned)count,
	       (count == 1u) ? "" : "s");
	if (survived_silence && got_records) {
		printk("Consistent with mechanism B (the quiesce-cancel race): a silent bus let the "
		       "radio work complete and the role stayed latched.\n");
	} else if (!survived_silence) {
		printk("The bus stayed dead through a completely silent window -- NOT consistent "
		       "with mechanism B alone; the OTHER reading (the radio op itself does not "
		       "return within its bound, independent of host traffic) is not ruled out.\n");
	} else {
		printk("Link survived the silence but the second scan returned no records -- "
		       "inconclusive on whether the role actually latched; re-run against a bench "
		       "with known APs in range before drawing a conclusion.\n");
	}

	return 0;
}
