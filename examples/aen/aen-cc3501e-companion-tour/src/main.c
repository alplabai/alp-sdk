/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-cc3501e-companion-tour -- the CAPSTONE full-surface example for the
 * on-module TI CC3501E Wi-Fi 6 + BLE 5.4 coprocessor, driven from the Alif
 * Ensemble E8 host (M55-HE).
 *
 * WHAT THIS IS.  aen-cc3501e-bringup is the *minimal* bring-up: power the
 * CC3501E, PING it, and soak so a J-Link can watch the link.  THIS example
 * is the opposite end -- it exercises the WHOLE companion API in one linear
 * sequence, as living documentation of the hand-written-firmware path:
 *
 *     init  ->  ping  ->  diag info
 *           ->  portable Wi-Fi open  ->  portable BLE open + scan
 *           ->  Wi-Fi scan  ->  Wi-Fi connect  ->  get IP
 *           ->  TCP socket: open -> connect -> send -> recv -> close
 *           ->  Wi-Fi disconnect
 *           ->  BLE enable  ->  BLE scan  ->  BLE disable
 *           ->  proxied-GPIO read
 *
 * The first radio checkpoint goes through the application-level portable APIs
 * in <alp/iot.h> and <alp/ble.h>.  The longer tour that follows stays on
 * <alp/chips/cc3501e.h> so developers still have a diagnostic surface for
 * firmware-version, scan-record, socket, and bridge-level bring-up work.  The
 * app never touches the raw wire protocol.
 *
 * WHY IT IS A DEMO, NOT A GATE.  Every step is guarded and NON-FATAL: a step
 * that fails prints the status and the tour continues to the next.  On a bench
 * with no AP in range the connect times out, with no BLE-built firmware the
 * enable returns NOT_READY, and so on -- the example still runs end to end and
 * shows you the shape of each call.  A real application would branch on these
 * statuses instead of just logging them.
 *
 * WHERE THE HARDWARE SETUP LIVES.  The SoM bring-up (control pins + inter-chip
 * SPI + power/reset sequence) is one call -- cc3501e_bridge_bringup() in
 * cc3501e_bridge.{c,h}, the reusable SoM template shared with the bringup
 * example.  Copy that pair into your own app, or call it directly.
 *
 * This file is ~50 % comment by design: examples are documentation for
 * hand-written firmware, not just runnable code.
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "alp/ble.h"
#include "alp/chips/cc3501e.h"
#include "alp/e1m_pinout.h" /* ALP_E1M_GPIO_IOxx -- portable E1M pin ids (proxied via the bridge) */
#include "alp/iot.h"
#include "alp/peripheral.h"

#include "cc3501e_bridge.h" /* cc3501e_bridge_bringup() -- the SoM bring-up helper */

/* ------------------------------------------------------------------ */
/* Tunables -- timeouts + the (build-time) Wi-Fi credentials.         */
/* ------------------------------------------------------------------ */

/* Poll-by-repeat budgets.  Several companion ops kick off a firmware worker
 * (scan / association / BLE bring-up) and answer BUSY until it finishes; the
 * driver re-issues the request until OK or the budget elapses.  These are the
 * *upper bounds* on that wait, generous enough for a real radio op.
 *
 * Every one is -D-overridable.  They were bare #defines, which silently WON
 * over a command-line -D and cost a bench run: a run built with
 * -DTOUR_CONNECT_TIMEOUT=60000u got 15000u anyway, and the resulting failure
 * could not be told apart from a real one.  A bare #define beside the
 * #ifndef-guarded credentials right below it is a trap, so they now match.
 *
 * TOUR_CONNECT_TIMEOUT's default is deliberately BELOW the firmware's own
 * worst case for a station connect, because this app is a quick full-surface
 * tour rather than a connection test, and a tour that parks for a minute on one
 * step is not a tour.
 *
 * That worst case is now about 60 s, not the 40 s an earlier version of this
 * comment gave: up to 10 s of Wlan_RoleUp (a connect issued as the first radio
 * op of a boot carries the role-up inside the connect body), up to 30 s of
 * association, and a 20 s DHCP-lease poll -- CC3501E_STA_DHCP_TRIES went from 50
 * to 100 so the poll covers lwIP's fourth DISCOVER at t=14 s instead of stopping
 * four seconds short of it.
 *
 * WHY THAT NUMBER MATTERS RATHER THAN BEING TRIVIA: a run budgeted below the
 * firmware's bound reports failures the radio never suffered, and it does not
 * look wrong while doing it -- the call simply returns -4 with the association
 * still in progress.  Two bench sessions in this campaign were hard to compare
 * for exactly that reason.  **If you are MEASURING connect or DHCP success,
 * override this to 70000u** (the sibling aen-cc3501e-socket-throughput budgets
 * 55000u, which predates the 20 s DHCP poll and is now marginal).  The tour's
 * own default is for touring, and a -4 at this default means "did not finish
 * inside a tour's patience", not "the radio failed".
 * TOUR_SCAN_TIMEOUT is likewise below cc3501e_wifi_scan()'s own 20 s floor,
 * which simply raises it -- see that floor's comment for why 15 s could not
 * express a healthy scan. */
#ifndef TOUR_PING_RETRIES
#define TOUR_PING_RETRIES 25u
#endif
#ifndef TOUR_PING_GAP_MS
#define TOUR_PING_GAP_MS 200u
#endif
#ifndef TOUR_SCAN_TIMEOUT
#define TOUR_SCAN_TIMEOUT 8000u
#endif
#ifndef TOUR_CONNECT_TIMEOUT
#define TOUR_CONNECT_TIMEOUT 15000u
#endif
#ifndef TOUR_BLE_TIMEOUT
#define TOUR_BLE_TIMEOUT 30000u
#endif
#ifndef TOUR_SOCK_TIMEOUT
#define TOUR_SOCK_TIMEOUT 5000u
#endif

/* How many scan records the witness arrays hold (bounded -- a busy band can
 * report dozens of APs; we print the first handful). */
#define TOUR_SCAN_MAX 16u

/*
 * Wi-Fi STA credentials for the CONNECT step.  DELIBERATELY EMPTY by default --
 * never hardcode bench credentials in a public example.  Set them at build time
 * WITHOUT editing this file, e.g.:
 *
 *   west build ... -- -DEXTRA_CFLAGS="-DTOUR_WIFI_SSID=\\\"myssid\\\" \
 *                                     -DTOUR_WIFI_PASS=\\\"mypass\\\""
 *
 * When TOUR_WIFI_SSID is empty the connect + socket + IP steps are skipped
 * (they need an association), and the tour jumps straight to the BLE section.
 */
#ifndef TOUR_WIFI_SSID
#define TOUR_WIFI_SSID ""
#endif
#ifndef TOUR_WIFI_PASS
#define TOUR_WIFI_PASS ""
#endif
/* Security: 0 = open, 1 = WPA2-PSK, 2 = WPA3-SAE (alp_cc3501e_wifi_connect_t). */
#ifndef TOUR_WIFI_SECURITY
#define TOUR_WIFI_SECURITY 1u
#endif

/*
 * Destination for the TCP socket round-trip (only used when connected).
 *
 * Override it at build time for your bench, the same way as the credentials:
 *
 *   west build ... -- -DEXTRA_CFLAGS="-DTOUR_TCP_A=192 -DTOUR_TCP_B=168 \
 *                                     -DTOUR_TCP_C=1   -DTOUR_TCP_D=1"
 *
 * The default was example.com's old canonical IPv4 (93.184.216.34), which no
 * longer serves -- a connect to it returns -4 and reads like a radio fault when
 * it is only a dead address.  It now defaults to the .1 gateway of a 192.168.1/24
 * bench LAN, which is far likelier to answer on port 80; point it wherever suits.
 *
 * ip[] is network order, ip[0] = the most-significant octet (a.b.c.d), which is
 * exactly what cc3501e_wifi_get_ip() also hands back, so a lease can feed a
 * connect directly.
 */
#ifndef TOUR_TCP_A
#define TOUR_TCP_A 192
#endif
#ifndef TOUR_TCP_B
#define TOUR_TCP_B 168
#endif
#ifndef TOUR_TCP_C
#define TOUR_TCP_C 1
#endif
#ifndef TOUR_TCP_D
#define TOUR_TCP_D 1
#endif
static const uint8_t TOUR_TCP_IP[4] = { TOUR_TCP_A, TOUR_TCP_B, TOUR_TCP_C, TOUR_TCP_D };
#define TOUR_TCP_PORT 80u

typedef struct {
	unsigned count;
} portable_ble_scan_ctx_t;

/* ------------------------------------------------------------------ */
/* Tour steps.  Each is self-contained + non-fatal: it logs its own      */
/* outcome and returns void so main() reads as the sequence-of-steps      */
/* documentation it is meant to be.                                       */
/* ------------------------------------------------------------------ */

/* Step: PING until the coprocessor answers (or the retry budget elapses).
 * reset() already waited out the boot budget, so the first PING usually
 * lands; the loop absorbs residual ramp/boot jitter.  Returns true if up. */
static bool tour_ping(cc3501e_t *fw)
{
	for (unsigned i = 0u; i < TOUR_PING_RETRIES; ++i) {
		alp_status_t s = cc3501e_ping(fw);
		if (s == ALP_OK) {
			printf("[tour] PING ok after %u attempt%s\n", i + 1u, (i == 0u) ? "" : "s");
			return true;
		}
		alp_delay_ms(TOUR_PING_GAP_MS);
	}
	printf("[tour] PING never answered -- check WIFI_EN power, the SPI1 pinmux, and that "
	       "the CC3501E is running its firmware\n");
	return false;
}

/* Step: protocol version + extended diagnostics.  GET_VERSION returns the wire
 * *protocol* version (the host compat gate); GET_DIAG_INFO decodes the 16-byte
 * firmware snapshot (release version, reset cause, active role, uptime, heap). */
static void tour_diag(cc3501e_t *fw)
{
	uint16_t     version = 0u;
	alp_status_t s       = cc3501e_get_version(fw, &version);
	if (s == ALP_OK) {
		printf("[tour] GET_VERSION -> protocol v%u (host expects v%u)%s\n",
		       version,
		       ALP_CC3501E_PROTOCOL_VERSION,
		       (version == ALP_CC3501E_PROTOCOL_VERSION) ? " -- match" : " -- MISMATCH!");
	} else {
		printf("[tour] GET_VERSION -> %d\n", (int)s);
	}

	/* GET_DIAG_INFO is a v2-firmware feature; v0.1 firmware rejects it with
	 * ALP_ERR_INVAL, which still proves the request round-trips + the error
	 * path is wired.  Decode the struct only on success. */
	alp_cc3501e_diag_info_t diag;
	s = cc3501e_diag_info(fw, &diag);
	if (s == ALP_OK) {
		printf("[tour] diag: fw=0x%04x reset_cause=%u role=%u uptime=%u ms "
		       "free_heap=%u B last_error=%u\n",
		       diag.fw_version,
		       diag.reset_cause,
		       diag.role,
		       diag.uptime_ms,
		       diag.free_heap_bytes,
		       diag.last_error);
	} else if (s == ALP_ERR_INVAL) {
		printf("[tour] GET_DIAG_INFO -> rejected (v0.1 firmware; v2-only) -- expected\n");
	} else {
		printf("[tour] GET_DIAG_INFO -> %d\n", (int)s);
	}
}

static void portable_ble_scan_cb(const alp_ble_scan_result_t *r, void *user)
{
	portable_ble_scan_ctx_t *ctx = (portable_ble_scan_ctx_t *)user;
	if (ctx != NULL) {
		ctx->count++;
		if (ctx->count <= 3u) {
			printf("PORTABLE_WIRELESS: ble_adv #%u rssi=%d len=%u\n",
			       ctx->count,
			       (int)r->rssi_dbm,
			       (unsigned)r->adv_len);
		}
	}
}

/*
 * Step: prove the application-level portable wireless dispatchers bind to the
 * same live CC3501E bridge.  cc3501e_bridge_bringup() attached @p fw to the
 * portable Wi-Fi and BLE backends; this checkpoint verifies the public handles
 * open and that BLE can run a real scan through <alp/ble.h>.
 */
/* Returns the fail count so main() can fold this checkpoint's own tally
 * into the RESULT verdict -- see the header comment on RESULT below for
 * why this is the one checkpoint in the tour that gates it. */
static unsigned tour_portable_wireless_checkpoint(void)
{
	unsigned pass = 0u;
	unsigned fail = 0u;

	alp_wifi_t *wifi = alp_wifi_open();
	if (wifi == NULL) {
		printf("PORTABLE_WIRELESS: wifi_open FAIL err=%d\n", (int)alp_last_error());
		fail++;
	} else {
		printf("PORTABLE_WIRELESS: wifi_open PASS\n");
		pass++;
		alp_wifi_close(wifi);
	}

	alp_ble_t *ble = alp_ble_open();
	if (ble == NULL) {
		printf("PORTABLE_WIRELESS: ble_open FAIL err=%d\n", (int)alp_last_error());
		fail++;
	} else {
		printf("PORTABLE_WIRELESS: ble_open PASS\n");
		pass++;

		portable_ble_scan_ctx_t scan = { 0 };
		alp_status_t            s    = alp_ble_scan_start(ble, false, portable_ble_scan_cb, &scan);
		if (s == ALP_OK) {
			printf("PORTABLE_WIRELESS: ble_scan PASS count=%u\n", scan.count);
			pass++;
		} else {
			printf("PORTABLE_WIRELESS: ble_scan FAIL err=%d\n", (int)s);
			fail++;
		}
		alp_ble_close(ble);
	}

	printf("PORTABLE_WIRELESS: SUMMARY pass=%u fail=%u\n", pass, fail);
	return fail;
}

/* Step: run a Wi-Fi scan and print the discovered APs.  SCAN_START is
 * poll-by-repeat (the firmware runs Wlan_Scan on a worker and answers BUSY
 * until it finishes); a success proves the whole submit -> worker -> reply
 * seam from the host.  cc3501e_wifi_sec_name() decodes each record's raw TI
 * SecurityInfo into a human bucket (open/wpa2/wpa3/...).
 *
 * THIS CALL IS THE TOUR'S FIRST RADIO OP OF THE BOOT (PING/GET_VERSION/
 * GET_DIAG_INFO above are not worker-routed and don't count), which makes it
 * the one call site issue #2035's known first-radio-op wedge can hit: roughly
 * 2 in 16 cold boots, the first worker-routed radio opcode times out (-4) and
 * the link then reads -5 until a cold cycle. The bridge firmware deliberately
 * does not fix this in-band (see @ref cc3501e_hard_reset's doc for why), so
 * the sanctioned response lives here, host-side, and ONLY here -- every later
 * radio call in this tour (connect, BLE enable/scan, ...) is left alone,
 * because a failure THERE is a real failure, not this boot-time condition. */
static void tour_wifi_scan(cc3501e_t *fw)
{
	static cc3501e_scan_record_t recs[TOUR_SCAN_MAX];
	size_t                       n = 0u;
	alp_status_t s = cc3501e_wifi_scan(fw, recs, TOUR_SCAN_MAX, &n, TOUR_SCAN_TIMEOUT);

	if (s == ALP_ERR_TIMEOUT) {
		/* The known wedge's signature: -4 on this, the first radio op of the
		 * boot. Recover with exactly ONE cc3501e_hard_reset() + ONE retry --
		 * never loop -- and say so out loud, so this statistic stays visible
		 * instead of hiding behind a silent retry. cc3501e_hard_reset() only
		 * pulses the line and blind-settles; it does not itself confirm the
		 * link, so the retried scan below is what proves recovery. */
		printf("[tour] WIFI_SCAN -> -4 (first radio op of this boot) -- known issue #2035 "
		       "wedge (~2 in 16 cold boots); issuing ONE cc3501e_hard_reset() + ONE retry\n");
		(void)cc3501e_hard_reset(fw);
		s = cc3501e_wifi_scan(fw, recs, TOUR_SCAN_MAX, &n, TOUR_SCAN_TIMEOUT);
		if (s != ALP_OK) {
			/* A second failure is a real failure -- surface it, don't retry again. */
			printf("[tour] WIFI_SCAN retry after hard reset -> %d (not the known wedge; "
			       "a genuine failure)\n",
			       (int)s);
			return;
		}
		printf("[tour] WIFI_SCAN recovered after one cc3501e_hard_reset() + retry\n");
	} else if (s != ALP_OK) {
		printf("[tour] WIFI_SCAN -> %d\n", (int)s);
		return;
	}
	printf("[tour] WIFI_SCAN -> %u AP(s)\n", (unsigned)n);
	for (size_t i = 0u; i < n; ++i) {
		printf("   [%u] \"%s\" ch%u %d dBm %s\n",
		       (unsigned)i,
		       recs[i].ssid,
		       recs[i].channel,
		       (int)recs[i].rssi_dbm,
		       cc3501e_wifi_sec_name(recs[i].security_info));
	}
}

/*
 * Step: connect to an AP, then open a TCP socket and do a tiny HTTP GET.
 *
 * Skipped entirely when TOUR_WIFI_SSID is empty (no credentials -> nothing to
 * associate with).  On a successful association it:
 *   - reads the DHCP lease with cc3501e_wifi_get_ip() (dotted-quad, printable),
 *   - opens a STREAM (TCP) socket, connects it to TOUR_TCP_IP:TOUR_TCP_PORT,
 *   - sends a minimal HTTP request and reads whatever comes back,
 *   - closes the socket.
 * Every socket call is a worker-routed poll-by-repeat over the bridge.  Then it
 * tears the association down again with cc3501e_wifi_disconnect().
 */
static void tour_wifi_connect_and_socket(cc3501e_t *fw)
{
	if (TOUR_WIFI_SSID[0] == '\0') {
		printf("[tour] WIFI_CONNECT skipped (TOUR_WIFI_SSID empty -- set it at build time); "
		       "socket + IP steps need an association, skipping too\n");
		return;
	}

	printf("[tour] WIFI_CONNECT -> SSID \"%s\" (sec %u)...\n",
	       TOUR_WIFI_SSID,
	       (unsigned)TOUR_WIFI_SECURITY);
	alp_status_t s = cc3501e_wifi_connect(
	    fw, TOUR_WIFI_SSID, (uint8_t)TOUR_WIFI_SECURITY, TOUR_WIFI_PASS, TOUR_CONNECT_TIMEOUT);
	if (s != ALP_OK) {
		printf("[tour] WIFI_CONNECT -> %d (no AP in range? wrong PSK?)\n", (int)s);

		/*
		 * ISSUE #2035 -- the decisive read, and the reason this branch got
		 * three extra reads instead of just a `return`.
		 *
		 * The bridge firmware has TWO writers of ALP_CC3501E_WIFI_FAIL_TIMEOUT
		 * and the host cannot tell them apart -- both latch the identical
		 * status, which maps here to the identical ALP_ERR_TIMEOUT:
		 *   1. the 30 s association wait expiring (a genuine L2 failure), and
		 *   2. a DHCP-lease gate that runs AFTER a successful
		 *      WLAN_EVENT_CONNECT, when the interface still has no address
		 *      (two FAIL_TIMEOUT sites, the second right after the lease poll).
		 *      That gate was 10 s in the ORIGINAL v0.8.0 image and is 20 s in
		 *      the re-cut, which covers lwIP's fourth DISCOVER at t=14 s
		 *      instead of stopping four seconds short of it.
		 * Bench-measured AGAINST THE ORIGINAL v0.8.0 IMAGE, whose DHCP gate
		 * really was 10 s: this call returned -4 at 14.45-16.87 s against a
		 * 70 s budget that verifiably reached the driver -- ~5 s of
		 * association plus that fixed 10 s poll fits the bracket exactly,
		 * and the 30 s path does not.  So the radio had associated and the
		 * failure was layer 3.  The reasoning held: lwIP's DISCOVER backoff
		 * lands at 0, 2, 6, 14 s, so a 10 s window only ever fit three
		 * attempts.
		 *
		 * SINCE CONFIRMED AND FIXED.  The gate is 20 s in the re-cut, the
		 * station no longer enters power save across DHCP, and a stalled
		 * client is restarted rather than left in an exponential backoff.
		 * Address acquisition went from roughly 1 in 4 to 14 of 16.
		 *
		 * These reads change NO behaviour and NO timeout -- they only look
		 * harder at what already happened:
		 *   1. Signal strength: a plausible dBm means the radio associated
		 *      at L2 (the failure is L3); unavailable means it never did.
		 *   2. Address, polled once a second for ~30 s: a lease arriving
		 *      late is the single most informative outcome -- it means the
		 *      firmware's DHCP gate is too short at this link budget, and
		 *      that the association works.  Measured both ways: late leases
		 *      landed at 12 s before the fixes and at 1 s after them.
		 *   3. The diagnostic reply's reserved[0] byte: the last Wi-Fi event
		 *      ID the firmware's callback saw.  CONNECTED supports the
		 *      address-lease reading; DISCONNECTED means it associated and
		 *      then dropped; SCAN_RESULT means the wait was released by a
		 *      stray event and the association never happened.
		 */

		/* 1. Signal strength.  Wait at least 1 s before reading -- the
		 * firmware's own comment warns this read can block if issued
		 * immediately after associating -- and by now the firmware's DHCP
		 * window has already closed too, so this is never read inside it. */
		alp_delay_ms(1000u);
		int8_t       rssi       = 0;
		alp_status_t rssi_s     = cc3501e_wifi_rssi(fw, &rssi);
		bool         associated = (rssi_s == ALP_OK);
		if (associated) {
			printf("[tour] POST-FAIL RSSI -> %d dBm (status %d) -- radio IS associated at L2\n",
			       (int)rssi,
			       (int)rssi_s);
		} else {
			printf("[tour] POST-FAIL RSSI -> status %d (no reading) -- radio likely never "
			       "associated\n",
			       (int)rssi_s);
		}

		/* 2. Address, polled once a second for ~30 s.  A late lease is the
		 * single most informative outcome available -- see the comment
		 * above. */
		bool late_lease = false;
		for (unsigned i = 0u; i < 30u; ++i) {
			uint8_t      ip[4] = { 0 };
			alp_status_t ip_s  = cc3501e_wifi_get_ip(fw, ALP_CC3501E_WIFI_IFACE_STA, ip);
			if (ip_s == ALP_OK) {
				printf("[tour] POST-FAIL IP poll [%u s] -> %u.%u.%u.%u -- LATE LEASE\n",
				       i + 1u,
				       ip[0],
				       ip[1],
				       ip[2],
				       ip[3]);
				late_lease = true;
				break;
			}
			printf("[tour] POST-FAIL IP poll [%u s] -> status %d\n", i + 1u, (int)ip_s);
			alp_delay_ms(1000u);
		}

		/* 3. The diagnostic reply's reserved[0].  THIS IS NOT AN
		 * ALP_CC3501E_EVT_WIFI_* opcode -- it is the bridge radio's OWN
		 * vendor TI SDK event id (WlanEvent_t.Id), a completely different
		 * namespace that DOES COLLIDE with the alp async-event-ring opcodes:
		 * the vendor's WlanEventId_e (wlan_if.h,
		 * simplelink_wifi_sdk_10_10_01_08) runs genuine ids 1..31, and three
		 * of those -- WLAN_EVENT_P2P_PEER_NOT_FOUND=24(0x18),
		 * _PERIODIC_SCAN_COMPLETE=25(0x19), WLAN_EVENT_FW_CRASH=26(0x1A) --
		 * alias EVT_WIFI_SCAN_RESULT/_CONNECTED/_DISCONNECTED exactly. Bug
		 * #2035 is what decoding reserved[0] against the opcode namespace
		 * caused: a real WLAN_EVENT_FW_CRASH (0x1A) read as
		 * EVT_WIFI_DISCONNECTED, so a radio firmware crash printed as
		 * "associated then dropped" -- not merely an unreachable branch.
		 * cc3501e_diag_info()'s field-level doc names this the moment it is
		 * read; see @ref alp_cc3501e_radio_evt_t for the decode table. */
		alp_cc3501e_diag_info_t diag;
		alp_status_t            diag_s   = cc3501e_diag_info(fw, &diag);
		bool                    have_evt = (diag_s == ALP_OK);
		uint8_t                 evt      = have_evt ? diag.reserved[0] : 0u;
		if (have_evt) {
			const char *evt_name;
			switch (evt) {
			case 0u:
				evt_name = "none since reset";
				break;
			case ALP_CC3501E_RADIO_EVT_CONNECT:
				evt_name = "CONNECT";
				break;
			case ALP_CC3501E_RADIO_EVT_ASSOCIATED:
				evt_name = "ASSOCIATED";
				break;
			case ALP_CC3501E_RADIO_EVT_DISCONNECT:
				evt_name = "DISCONNECT";
				break;
			case ALP_CC3501E_RADIO_EVT_SCAN_RESULT:
				evt_name = "SCAN_RESULT";
				break;
			case ALP_CC3501E_RADIO_EVT_EXTENDED_SCAN_RESULT:
				evt_name = "EXTENDED_SCAN_RESULT";
				break;
			case ALP_CC3501E_RADIO_EVT_AUTHENTICATION_REJECTED:
				evt_name = "AUTHENTICATION_REJECTED";
				break;
			case ALP_CC3501E_RADIO_EVT_ASSOCIATION_REJECTED:
				evt_name = "ASSOCIATION_REJECTED";
				break;
			case ALP_CC3501E_RADIO_EVT_CONNECTING:
				evt_name = "CONNECTING";
				break;
			case ALP_CC3501E_RADIO_EVT_FW_CRASH:
				evt_name = "FW_CRASH";
				break;
			case ALP_CC3501E_RADIO_EVT_COMMAND_TIMEOUT:
				evt_name = "COMMAND_TIMEOUT";
				break;
			case ALP_CC3501E_RADIO_EVT_ERROR:
				evt_name = "ERROR";
				break;
			default:
				evt_name = "other radio event";
				break;
			}
			printf("[tour] POST-FAIL diag reserved[0] -> 0x%02x (%s)\n", evt, evt_name);
		} else {
			printf("[tour] POST-FAIL diag read -> status %d (no event byte available)\n",
			       (int)diag_s);
		}

		/* Plain reading.  Any leg that itself failed to read makes the run
		 * inconclusive -- never inferred from a read that did not happen.
		 * A CONNECT/ASSOCIATED event alongside a valid RSSI is the bench
		 * evidence this issue was filed over: the radio associated and no
		 * lease arrived, which is an L3/DHCP failure, not an L2 one.  An
		 * AUTHENTICATION_REJECTED / ASSOCIATION_REJECTED event is a genuine
		 * L2 failure regardless of what RSSI reads, since the radio heard
		 * the AP well enough to be rejected by it -- and likewise a
		 * FW_CRASH / COMMAND_TIMEOUT / ERROR event is a genuine radio fault
		 * regardless of what RSSI reads: reporting either as an
		 * "association failure" would blame L2 for a fault that already
		 * happened one layer down. */
		bool radio_connected = have_evt && (evt == ALP_CC3501E_RADIO_EVT_CONNECT ||
		                                    evt == ALP_CC3501E_RADIO_EVT_ASSOCIATED);
		bool radio_rejected  = have_evt && (evt == ALP_CC3501E_RADIO_EVT_AUTHENTICATION_REJECTED ||
		                                    evt == ALP_CC3501E_RADIO_EVT_ASSOCIATION_REJECTED);
		bool radio_fault     = have_evt && (evt == ALP_CC3501E_RADIO_EVT_FW_CRASH ||
		                                    evt == ALP_CC3501E_RADIO_EVT_COMMAND_TIMEOUT ||
		                                    evt == ALP_CC3501E_RADIO_EVT_ERROR);
		if (!associated && !late_lease && !have_evt) {
			printf("[tour] POST-FAIL READING -> inconclusive: the reads themselves failed\n");
		} else if (late_lease) {
			printf("[tour] POST-FAIL READING -> a DHCP lease arrived after the connect call "
			       "gave up: the firmware's DHCP gate is too short at this link budget, "
			       "association DOES work\n");
		} else if (radio_rejected) {
			printf("[tour] POST-FAIL READING -> radio event 0x%02x is a genuine L2 failure: "
			       "the AP heard and rejected the association attempt\n",
			       evt);
		} else if (radio_fault) {
			printf("[tour] POST-FAIL READING -> radio event 0x%02x is a RADIO FIRMWARE FAULT, "
			       "not an association failure: the bridge's own event log says its Wi-Fi stack "
			       "crashed, timed out, or errored -- it never got the chance to fail at L2\n",
			       evt);
		} else if (associated && radio_connected) {
			printf("[tour] POST-FAIL READING -> the radio associated (RSSI %d dBm, radio event "
			       "0x%02x) and no lease arrived in the poll window: an L3/DHCP failure, not an "
			       "association failure\n",
			       (int)rssi,
			       evt);
		} else if (associated && have_evt && evt == ALP_CC3501E_RADIO_EVT_DISCONNECT) {
			printf("[tour] POST-FAIL READING -> radio associated then dropped (RSSI reading + "
			       "radio event DISCONNECT): an association that did not hold, not one that "
			       "never happened\n");
		} else if (!associated && have_evt && evt == ALP_CC3501E_RADIO_EVT_SCAN_RESULT) {
			printf("[tour] POST-FAIL READING -> no RSSI and the last radio event was a stray "
			       "SCAN_RESULT: the wait was released by that event, association never "
			       "happened\n");
		} else if (!associated && have_evt && (radio_connected || !radio_rejected)) {
			/* Anything else with no RSSI reading: a read that did not happen
			 * never yields a positive finding, and in particular a
			 * CONNECT/ASSOCIATED event here would directly contradict "no
			 * RSSI -> never associated" -- report inconclusive rather than
			 * asserting an L2 failure the radio's own event disputes. */
			printf("[tour] POST-FAIL READING -> inconclusive: no RSSI reading, radio event "
			       "0x%02x does not settle whether L2 association happened\n",
			       evt);
			/* NOTE: with radio_rejected already excluded above, the
			 * condition on this branch reduces to plain !associated -- there
			 * is deliberately no separate "no RSSI -> assert L2 failure"
			 * branch left below it; a failed/absent RSSI read alone never
			 * proves an association failure (see the comment block above). */
		} else if (have_evt) {
			printf("[tour] POST-FAIL READING -> mixed signals (RSSI %d dBm, radio event 0x%02x): "
			       "inconclusive\n",
			       (int)rssi,
			       evt);
		} else {
			printf("[tour] POST-FAIL READING -> mixed signals (RSSI %d dBm, diag read failed): "
			       "inconclusive\n",
			       (int)rssi);
		}

		return;
	}
	printf("[tour] WIFI_CONNECT -> associated\n");

	/* DHCP lease.  ip[] is network order (ip[0] = MSB), already dotted-quad. */
	uint8_t ip[4] = { 0 };
	if (cc3501e_wifi_get_ip(fw, ALP_CC3501E_WIFI_IFACE_STA, ip) == ALP_OK) {
		printf("[tour] IP -> %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
	} else {
		printf("[tour] IP -> not leased yet\n");
	}

	/* Signal strength of the association (a quick sanity read). */
	int8_t rssi = 0;
	if (cc3501e_wifi_rssi(fw, &rssi) == ALP_OK) {
		printf("[tour] RSSI -> %d dBm\n", (int)rssi);
	}

	/* ---- TCP socket round-trip ---- */
	uint16_t sock = 0u;
	s             = cc3501e_sock_open(fw,
	                                  ALP_CC3501E_SOCK_FAMILY_IPV4,
	                                  ALP_CC3501E_SOCK_TYPE_STREAM,
	                                  0u /* default proto = TCP */,
	                                  &sock,
	                                  TOUR_SOCK_TIMEOUT);
	if (s != ALP_OK) {
		printf("[tour] SOCK_OPEN -> %d\n", (int)s);
	} else {
		printf("[tour] SOCK_OPEN -> handle 0x%04x\n", sock);

		s = cc3501e_sock_connect(fw, sock, TOUR_TCP_IP, TOUR_TCP_PORT, TOUR_SOCK_TIMEOUT);
		if (s != ALP_OK) {
			printf("[tour] SOCK_CONNECT -> %d\n", (int)s);
		} else {
			printf("[tour] SOCK_CONNECT -> %u.%u.%u.%u:%u\n",
			       TOUR_TCP_IP[0],
			       TOUR_TCP_IP[1],
			       TOUR_TCP_IP[2],
			       TOUR_TCP_IP[3],
			       (unsigned)TOUR_TCP_PORT);

			static const char req[] = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
			size_t            sent  = 0u;
			s                       = cc3501e_sock_send(
			    fw, sock, (const uint8_t *)req, sizeof(req) - 1u, &sent, TOUR_SOCK_TIMEOUT);
			printf("[tour] SOCK_SEND -> %d (%u/%u bytes queued)\n",
			       (int)s,
			       (unsigned)sent,
			       (unsigned)(sizeof(req) - 1u));

			/* One receive window: print how many bytes came back (a real client
			 * would loop until a zero-length recv after the peer closes). */
			static uint8_t resp[256];
			size_t         got = 0u;
			s = cc3501e_sock_recv(fw, sock, resp, sizeof(resp), &got, TOUR_SOCK_TIMEOUT);
			printf("[tour] SOCK_RECV -> %d (%u bytes)\n", (int)s, (unsigned)got);
		}
		(void)cc3501e_sock_close(fw, sock, TOUR_SOCK_TIMEOUT);
		printf("[tour] SOCK_CLOSE done\n");
	}

	/* Tear the association back down (radio op; poll-by-repeat internally). */
	s = cc3501e_wifi_disconnect(fw);
	printf("[tour] WIFI_DISCONNECT -> %d\n", (int)s);
}

/*
 * Step: bring BLE up, scan for advertisers, then bring it back down.
 *
 * BLE_ENABLE is worker-routed (it starts the shared-HIF Wi-Fi stack then the
 * NimBLE host, ~seconds) so the budget is generous.  Firmware built WITHOUT
 * BLE answers NOT_READY -- handled gracefully.  BLE_SCAN_START runs a NimBLE
 * GAP discovery for a fixed window and returns the de-duplicated reports.
 */
static void tour_ble(cc3501e_t *fw)
{
	alp_status_t s = cc3501e_ble_enable(fw, TOUR_BLE_TIMEOUT);
	if (s == ALP_ERR_NOT_READY) {
		printf("[tour] BLE_ENABLE -> NOT_READY (firmware built without BLE) -- skipping BLE\n");
		return;
	}
	if (s != ALP_OK) {
		printf("[tour] BLE_ENABLE -> %d\n", (int)s);
		return;
	}
	printf("[tour] BLE_ENABLE -> controller + NimBLE host up\n");

	static cc3501e_ble_scan_record_t recs[TOUR_SCAN_MAX];
	size_t                           n = 0u;
	s = cc3501e_ble_scan(fw, recs, TOUR_SCAN_MAX, &n, TOUR_BLE_TIMEOUT);
	if (s == ALP_OK) {
		printf("[tour] BLE_SCAN -> %u advertiser(s)\n", (unsigned)n);
		for (size_t i = 0u; i < n; ++i) {
			printf("   [%u] %02x:%02x:%02x:%02x:%02x:%02x %d dBm \"%s\"\n",
			       (unsigned)i,
			       recs[i].addr[5],
			       recs[i].addr[4],
			       recs[i].addr[3],
			       recs[i].addr[2],
			       recs[i].addr[1],
			       recs[i].addr[0],
			       (int)recs[i].rssi_dbm,
			       recs[i].name);
		}
	} else {
		printf("[tour] BLE_SCAN -> %d\n", (int)s);
	}

	s = cc3501e_ble_disable(fw, TOUR_BLE_TIMEOUT);
	printf("[tour] BLE_DISABLE -> %d\n", (int)s);
}

/*
 * Step: read a CC3501E-proxied E1M IO through the portable GPIO API.
 *
 * With CONFIG_ALP_SDK_GPIO_CC3501E_PROXY on and the SoM route table populated
 * (src/cc3501e_gpio_routes.c), alp_gpio_open(ALP_E1M_GPIO_IOxx) for a mapped IO
 * routes over the inter-chip bridge (cc3501e_gpio_configure/_read) while every
 * other pin delegates to the platform GPIO driver -- the app code is identical
 * either way.  IO15 (S_BMI323.INT1) is a proxied input in the route table.
 */
static void tour_gpio_proxy(void)
{
	alp_gpio_t *io = alp_gpio_open(ALP_E1M_GPIO_IO15);
	if (io == NULL) {
		printf("[tour] GPIO proxy: alp_gpio_open(IO15) -> not available on this board\n");
		return;
	}
	(void)alp_gpio_configure(io, ALP_GPIO_INPUT, ALP_GPIO_PULL_NONE);
	bool level = false;
	if (alp_gpio_read(io, &level) == ALP_OK) {
		printf("[tour] GPIO proxy: IO15 (over the bridge) reads %d\n", (int)level);
	} else {
		printf("[tour] GPIO proxy: IO15 read failed (err=%d)\n", (int)alp_last_error());
	}
	alp_gpio_close(io);
}

int main(void)
{
	printf("\n[tour] E1M-AEN CC3501E companion full-surface tour\n");

	/*
	 * Step 1 -- bring up the SoM's CC3501E in ONE call.  cc3501e_bridge_bringup()
	 * opens the inter-chip SPI + the WIFI_EN/nRESET control pins, binds them,
	 * attaches the GPIO proxy, and runs the power+reset sequence.  Everything
	 * after this uses the portable cc3501e_* + alp_gpio_* surfaces.
	 *
	 * STATIC, not a stack local (#1868): sizeof(cc3501e_t) is ~32 KB
	 * (measured -- rx_scratch/tx_scratch/wifi_scan_buf/ble_scan_buf/
	 * evt_buf/sock_buf/spi1_{tx,rx}_buf are each sized off the v5
	 * ALP_CC3501E_MAX_PAYLOAD=4096) against CONFIG_MAIN_STACK_SIZE=4096,
	 * so as a local the prologue's frame reservation alone crosses
	 * PSPLIM and the M55 raises a UsageFault at main() entry -- before
	 * even this function's first printf runs.  Same fix, same reason,
	 * as aen-cc3501e-bringup's `fw`.
	 */
	static cc3501e_t fw;
	alp_status_t     s = cc3501e_bridge_bringup(&fw);
	if (s == ALP_ERR_NOT_PRESENT_ON_THIS_SOC) {
		printf("[tour] bridge bring-up failed (SPI bus %u / WIFI_EN+nRESET absent? err=%d) -- "
		       "check the board overlay\n",
		       CC3501E_BRIDGE_SPI_BUS_ID,
		       (int)alp_last_error());
		return 0;
	}
	printf("[tour] bridge bring-up -> %d%s\n",
	       (int)s,
	       (s == ALP_ERR_NOSUPPORT) ? " (control pins not bound?)" : "");

	/* Step 2 -- liveness.  Every later step is non-fatal, but if the link never
	 * comes up they will all just log errors, so surface it clearly here.
	 * `up` feeds the RESULT verdict below -- it is the app's own witness of
	 * whether the coprocessor ever answered. */
	bool up = tour_ping(&fw);
	if (!up) {
		printf("[tour] link down -- the remaining steps will report errors; continuing so "
		       "the call shapes are still shown\n");
	}

	/* Step 3 -- version + diagnostics. */
	tour_diag(&fw);

	/* Step 4 -- portable Wi-Fi/BLE dispatch checkpoint.  This is the one step
	 * in the tour with its own pass/fail tally (PORTABLE_WIRELESS: SUMMARY);
	 * everything else here is deliberately non-fatal narration (see the file
	 * header), so this checkpoint's fail count is what RESULT gates on. */
	unsigned wireless_fail = tour_portable_wireless_checkpoint();

	/* Step 5 -- Wi-Fi scan (poll-by-repeat worker seam). */
	tour_wifi_scan(&fw);

	/* Step 6 -- Wi-Fi connect + a TCP socket round-trip + disconnect
	 * (skipped when no credentials are built in). */
	tour_wifi_connect_and_socket(&fw);

	/* Step 7 -- BLE enable -> scan -> disable. */
	tour_ble(&fw);

	/* Step 8 -- a proxied-GPIO read over the bridge. */
	tour_gpio_proxy();

	printf("[tour] full-surface tour complete\n");

	/*
	 * RESULT verdict.  Most of the tour is deliberately non-fatal narration
	 * (see the file header: "a demo, not a gate"), so RESULT can't just fire
	 * unconditionally at the end -- that would print PASS even on a dead
	 * link.  It gates on the two pieces of state the tour DOES track:
	 *   - `up`: did the coprocessor ever answer a PING (tour_ping's own
	 *     result, not just "we reached this line"),
	 *   - `wireless_fail`: the portable Wi-Fi/BLE checkpoint's own fail
	 *     tally (PORTABLE_WIRELESS: SUMMARY above).
	 * Both are false/zero only on a run that actually talked to the radio
	 * successfully -- a run where the link never came up leaves `up` false
	 * (and the checkpoint's own opens would fail too), so neither path here
	 * can reach PASS without having done the work.
	 */
	if (up && wireless_fail == 0u) {
		printf("RESULT PASS: cc3501e link up, portable Wi-Fi+BLE checkpoint clean "
		       "(fail=0)\n");
	} else if (!up) {
		printf("RESULT FAIL: coprocessor never answered PING\n");
	} else {
		printf("RESULT FAIL: %u portable Wi-Fi/BLE checkpoint failure(s)\n", wireless_fail);
	}
	return 0;
}
