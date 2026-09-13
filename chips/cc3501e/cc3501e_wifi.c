/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * CC3501E Wi-Fi host helpers (opcodes 0x03, 0x10..0x17, 0x1B).  See
 * <alp/chips/cc3501e/wifi.h> for the public API and
 * <alp/protocol/cc3501e.h> for the wire protocol.
 *
 * Each is a thin wrapper over cc3501e_request matching the opcodes +
 * payloads in <alp/protocol/cc3501e.h>.  WIRE GAPS (the protocol
 * header, owned by the firmware-side agent, has opcodes but NO reply
 * payload structs for these -- noted per-helper):
 *   - GET_MAC (0x03): reply data assumed to be the 6 MAC bytes.
 *   - WIFI_GET_RSSI (0x16): reply data assumed to be one int8 dBm.
 *   - WIFI_GET_IP (0x17): reply data assumed to be 4 IPv4 octets.
 *   - WIFI_SCAN_START (0x10): the header defines alp_cc3501e_scan_
 *     result_t and documents scan results as ASYNC events
 *     (EVT_WIFI_SCAN_RESULT 0x18) -- there is no synchronous
 *     count/list envelope.  This rev has no async-event line, so the
 *     helper assumes the firmware returns the packed records as the
 *     SCAN_START reply payload (each fixed 10-byte header + inline
 *     ssid_len SSID bytes).
 */

#include <string.h>
#include <stdint.h>

#include "cc3501e_internal.h"

/* Minimum budget for an op that can overlap a radio bring-up.  On this
 * no-host-IRQ rev the bridge link is DOWN while the CC35 runs a radio op
 * (Wlan_Start/RoleUp can take SECONDS); requests during that window read back
 * IO and must keep retrying.  Floor the get-MAC poll budget here so a small
 * caller timeout can't give up inside the down-window before the radio is up. */
#define CC3501E_WIFI_DOWN_WINDOW_MS 10000u

/* Minimum budget for a SCAN, which is strictly larger than the plain radio
 * down-window above because a scan issued as the first Wi-Fi op of a boot pays
 * the STA role-up first.  The firmware's own bounded worst case, read off
 * hal/ti/cc3501e_hw_ti_wifi.c: CC3501E_WIFI_ROLE_TIMEOUT_MS = 10 s for the
 * Wlan_RoleUp, then a 6 s osi_SyncObjWait for the Wlan_Scan result = 16 s.
 *
 * A caller budget below that gives up on a HEALTHY board mid-scan, and this is
 * not hypothetical: a 15 s budget produced `WIFI_SCAN_START rc=-4
 * elapsed_ms=15062` on silicon, 1062 ms inside the firmware's own bound.
 *
 * Be careful what that proves.  It was read at the time as the caller's clock
 * running out, and this floor was written on that reading.  It is equally
 * consistent with the link having been down for the whole radio op -- the
 * first-radio-op behaviour, where a polled scan issued as the first radio
 * operation of a boot never recovers.  Bench-measured later: the same scan
 * shape returns records when it is NOT the first polled radio op (6 records,
 * and 5 across four earlier cold-booted runs), and fails when it is.
 *
 * So this floor is NOT claimed to fix that failure.  It is the narrower,
 * defensible thing: a caller budget below the firmware's own bound cannot
 * express a healthy outcome at all, so a timeout at that budget tells you
 * nothing about the radio.  Removing an uninformative failure mode is worth
 * doing on its own.
 *
 * 20 s = the 16 s bound plus margin for the reply round trip and host
 * scheduling, the same shape the 55 s connect budget uses over its own 40 s. */
#define CC3501E_WIFI_SCAN_WINDOW_MS 20000u

/* See <alp/protocol/cc3501e.h> for the wire shape and cc3501e_internal.h for
 * why this is not `static` (test visibility). */
bool cc3501e_mac_is_valid(const uint8_t mac[CC3501E_MAC_LEN])
{
	bool all_zero = true;
	for (size_t i = 0; i < CC3501E_MAC_LEN; i++) {
		if (mac[i] != 0x00u) {
			all_zero = false;
			break;
		}
	}
	if (all_zero) return false;

	/* IEEE 802.1: bit 0 of the first octet is the group/multicast bit.  A
	 * real station's individually-assigned (unicast) address always has it
	 * clear; a garbled-but-nonzero dead-phase reply is not bound by that. */
	if ((mac[0] & 0x01u) != 0u) return false;

	return true;
}

alp_status_t cc3501e_wifi_get_mac(cc3501e_t *ctx, uint8_t mac[CC3501E_MAC_LEN], uint32_t timeout_ms)
{
	if (mac == NULL) return ALP_ERR_INVAL;

	/* Floor the budget to cover the radio down-window: a GET_MAC issued while
	 * the CC35 is still bringing the radio up (boot Wlan_Start) sees the bridge
	 * down (IO) and must keep retrying until the radio is up + the slave
	 * re-syncs.  A caller passing a short timeout would otherwise give up mid
	 * down-window and report a spurious failure. */
	uint32_t budget = timeout_ms;
	if (budget < CC3501E_WIFI_DOWN_WINDOW_MS) budget = CC3501E_WIFI_DOWN_WINDOW_MS;

	uint8_t      reply[CC3501E_MAC_LEN] = { 0 };
	size_t       got                    = 0;
	alp_status_t s =
	    poll_by_repeat(ctx, ALP_CC3501E_CMD_GET_MAC, NULL, 0, reply, sizeof(reply), &got, budget);
	if (s != ALP_OK) return s;
	if (got < CC3501E_MAC_LEN) return ALP_ERR_IO; /* short reply -- firmware/wire gap */

	/* #2035: cc3501e_reply_may_be_all_zero() (cc3501e_core.c) already treats an
	 * all-zero GET_MAC reply as the #1378 dead-phase alias -- GET_MAC is not on
	 * its exemption list, so a header that read intact followed by an all-zero
	 * payload phase never reaches here as ALP_OK at all.  But that transport-
	 * level guard only sees BYTES; it cannot know a MAC is invalid unless every
	 * one of them is 0x00.  A dead phase that returns a NON-zero garbage
	 * pattern -- or, in principle, a genuinely corrupted-but-nonzero reply --
	 * would sail through it and land here.  cc3501e_mac_is_valid() catches the
	 * one thing this driver DOES know about a real station address regardless
	 * of byte pattern: the IEEE group/multicast bit (mac[0] bit 0) is never set
	 * on an individual (unicast) station address.  Reject here, at the driver,
	 * with the same loud ALP_ERR_IO -- not silently retried, and not left for
	 * every caller to reinvent (this was previously caught only by a demo that
	 * happened to read the MAC twice and compare:
	 * examples/aen/aen-evk-demo's cc35_mac_plausible()). */
	if (!cc3501e_mac_is_valid(reply)) return ALP_ERR_IO;

	memcpy(mac, reply, CC3501E_MAC_LEN);
	return ALP_OK;
}

/* On-wire fixed header of a scan record (alp_cc3501e_scan_result_t without the
 * inline SSID): bssid[6] + rssi(1) + channel(1) + security(1) + ssid_len(1). */
#define CC3501E_SCAN_REC_HDR 11u

/* Decode the raw TI 16-bit SecurityInfo. The sec-type bitmap lives in the high
 * byte ((info >> 8) & 0x3f = WLAN_SCAN_RESULT_SEC_TYPE_BITMAP): SAE bits
 * (0x08|0x10) mark WPA3, 0x04 = WPA2, 0x02 = WPA, 0x01 = WEP, 0 = open. */
cc3501e_wifi_sec_t cc3501e_wifi_sec_kind(uint16_t security_info)
{
	uint8_t sec = (uint8_t)((security_info >> 8) & 0x3fu);
	if (sec == 0u) return CC3501E_WIFI_SEC_OPEN;
	if (sec & (0x08u | 0x10u)) return CC3501E_WIFI_SEC_WPA3;
	if (sec & 0x04u) return CC3501E_WIFI_SEC_WPA2;
	if (sec & 0x02u) return CC3501E_WIFI_SEC_WPA;
	if (sec & 0x01u) return CC3501E_WIFI_SEC_WEP;
	return CC3501E_WIFI_SEC_UNKNOWN;
}

const char *cc3501e_wifi_sec_name(uint16_t security_info)
{
	switch (cc3501e_wifi_sec_kind(security_info)) {
	case CC3501E_WIFI_SEC_OPEN:
		return "open";
	case CC3501E_WIFI_SEC_WEP:
		return "wep";
	case CC3501E_WIFI_SEC_WPA:
		return "wpa";
	case CC3501E_WIFI_SEC_WPA2:
		return "wpa2";
	case CC3501E_WIFI_SEC_WPA3:
		return "wpa3";
	default:
		return "sec?";
	}
}

alp_status_t cc3501e_wifi_scan(cc3501e_t             *ctx,
                               cc3501e_scan_record_t *out_records,
                               size_t                 cap,
                               size_t                *count,
                               uint32_t               timeout_ms)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (out_records == NULL && cap > 0u) return ALP_ERR_INVAL;
	if (count != NULL) *count = 0;

	/* Serialize same-context reentrancy explicitly (issue #740): a caller
	 * that re-enters this same ctx (e.g. from inside a callback invoked
	 * further down the same call stack) while a scan is already decoding
	 * here would otherwise race on wifi_scan_buf below.  Report BUSY rather
	 * than alias.  NOTE this is a plain test-then-set bool, not an
	 * atomic/CAS: it catches same-call-stack reentrancy but does not by
	 * itself serialize two truly concurrent callers on the SAME ctx from
	 * separate threads/cores -- see the concurrency warning on
	 * cc3501e_poll_events() in <alp/chips/cc3501e/events.h>. */
	if (ctx->wifi_scan_busy) return ALP_ERR_BUSY;
	ctx->wifi_scan_busy = true;

	/* The scan records can fill the reply payload; receive into the
	 * context's own scratch buffer (per-instance -- see cc3501e_t's
	 * wifi_scan_buf comment; keeps cc3501e_request's rx_scratch free for
	 * the framing, and no longer aliases across cc3501e_t instances). */
	uint8_t *scan_buf = ctx->wifi_scan_buf;
	size_t   got      = 0;

	/* Floor the budget at the firmware's own worst case -- see
	 * CC3501E_WIFI_SCAN_WINDOW_MS.  A caller asking for less is asking for an
	 * answer the firmware cannot give in time, and the resulting timeout looks
	 * exactly like a dead link. */
	uint32_t budget = timeout_ms;
	if (budget < CC3501E_WIFI_SCAN_WINDOW_MS) budget = CC3501E_WIFI_SCAN_WINDOW_MS;

	alp_status_t s = poll_by_repeat(ctx,
	                                ALP_CC3501E_CMD_WIFI_SCAN_START,
	                                NULL,
	                                0,
	                                scan_buf,
	                                sizeof(ctx->wifi_scan_buf),
	                                &got,
	                                budget);
	if (s != ALP_OK) {
		ctx->wifi_scan_busy = false;
		return s;
	}

	/* Walk the packed records: each is a 10-byte fixed header immediately
	 * followed by ssid_len inline SSID bytes (no padding). */
	size_t off = 0;
	size_t n   = 0;
	while (off + CC3501E_SCAN_REC_HDR <= got && n < cap) {
		const uint8_t *rec      = &scan_buf[off];
		uint8_t        ssid_len = rec[10];
		if (off + CC3501E_SCAN_REC_HDR + (size_t)ssid_len > got) {
			break; /* truncated trailing record -- stop cleanly */
		}
		cc3501e_scan_record_t *out = &out_records[n];
		memcpy(out->bssid, &rec[0], 6);
		out->rssi_dbm = (int8_t)rec[6];
		out->channel  = rec[7];
		/* Raw 16-bit SecurityInfo, little-endian (firmware packs both bytes at
		 * rec[8..9]; the sec-type lives in the high byte). */
		out->security_info = (uint16_t)rec[8] | ((uint16_t)rec[9] << 8);
		out->ssid_len      = ssid_len;
		uint8_t copy       = (ssid_len > CC3501E_SSID_MAX) ? (uint8_t)CC3501E_SSID_MAX : ssid_len;
		memcpy(out->ssid, &rec[CC3501E_SCAN_REC_HDR], copy);
		out->ssid[copy] = '\0';
		off += CC3501E_SCAN_REC_HDR + (size_t)ssid_len;
		n++;
	}
	if (count != NULL) *count = n;
	ctx->wifi_scan_busy = false;
	return ALP_OK;
}

alp_status_t cc3501e_wifi_scan_stop(cc3501e_t *ctx)
{
	/* Abort an in-progress scan (WIFI_SCAN_STOP, 0x11).  No payload, no reply
	 * data -- success is the OK status.  The firmware tears the scan down as a
	 * radio op, so the bridge can be briefly down (IO/BUSY) mid-abort; floor the
	 * budget to the radio down-window and let poll_by_repeat retry, exactly like
	 * cc3501e_wifi_get_mac. */
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_WIFI_SCAN_STOP, NULL, 0, NULL, 0, NULL, CC3501E_WIFI_DOWN_WINDOW_MS);
}

/* Backoff between CMD_WIFI_STATUS collect polls while cc3501e_wifi_connect()
 * awaits the outcome -- same cadence as poll_by_repeat's CC3501E_POLL_GAP_MS
 * (cc3501e_core.c), duplicated here rather than exported because this loop
 * polls a DIFFERENT opcode (WIFI_STATUS) than the one it is bounding
 * (WIFI_CONNECT_STA), so it cannot just call poll_by_repeat itself. */
#define CC3501E_WIFI_STATUS_POLL_GAP_MS 50u

/* Single, non-retried WIFI_STATUS read (opcode 0x1B) -- decodes the same
 * fixed 4-byte wire layout as the public cc3501e_wifi_status() below, but
 * WITHOUT its down-window poll_by_repeat.
 *
 * cc3501e_wifi_connect()'s own loop below already re-polls on its own
 * cadence (CC3501E_WIFI_STATUS_POLL_GAP_MS) for the caller's whole
 * timeout_ms budget.  Calling the public cc3501e_wifi_status() from inside
 * that loop layered ITS OWN internal retry (up to CC3501E_WIFI_DOWN_WINDOW_MS
 * = 10 s) underneath the outer loop, and the outer loop's
 * `remaining -= gap` accounting only ever saw the 50 ms gap it slept for --
 * never the up-to-10-s the inner call could burn on a wedged transport.
 * Measured against a wedged transport in this repo's own harness:
 * connect(timeout_ms=200) made 1005 WIFI_STATUS attempts = 50250 ms (251x
 * the declared budget) before giving up.
 *
 * A single non-retried attempt here (nominally CC3501E_REQ_TMO_MS, the same
 * per-attempt budget every other single-shot request in this file passes --
 * but cc3501e_request() does NOT enforce it, see the #1481 note in
 * cc3501e_wifi_connect() below) keeps the outer loop the SOLE owner of the
 * retry budget, matching <alp/chips/cc3501e/wifi.h>'s documented contract
 * for cc3501e_wifi_connect ("Upper bound on the WIFI_STATUS poll budget"). */
static alp_status_t wifi_status_once(cc3501e_t *ctx, alp_cc3501e_wifi_status_t *out)
{
	uint8_t      reply[4] = { 0 };
	size_t       got      = 0;
	alp_status_t s        = cc3501e_request(
	    ctx, ALP_CC3501E_CMD_WIFI_STATUS, NULL, 0, reply, sizeof(reply), &got, CC3501E_REQ_TMO_MS);
	if (s != ALP_OK) return s;
	if (got < sizeof(reply)) return ALP_ERR_IO; /* short reply -- firmware/wire gap */
	out->state       = reply[0];
	out->fail_reason = reply[1];
	out->rssi_dbm    = (int8_t)reply[2];
	out->reserved    = reply[3];
	return ALP_OK;
}

alp_status_t cc3501e_wifi_connect(cc3501e_t  *ctx,
                                  const char *ssid,
                                  uint8_t     sec_type,
                                  const char *pass,
                                  uint32_t    timeout_ms)
{
	if (ssid == NULL) return ALP_ERR_INVAL;
	size_t ssid_len = strlen(ssid);
	size_t psk_len  = (pass != NULL) ? strlen(pass) : 0u;
	if (ssid_len > 32u || psk_len > 64u) return ALP_ERR_INVAL;

	/* #1435: clear a STALE ASSOCIATION left by a previous FAILED connect
	 * before this one submits.  Bench-proven on E1M-AEN801 r1, reproduced
	 * 2/2, one boot, single variable: connect to a real AP succeeds
	 * (rssi=-48 dBm, ip 192.168.1.14); `wifi disconnect`; the SAME connect
	 * succeeds again (rssi=-47 dBm); ONE connect to a non-existent SSID
	 * fails -5; the SAME connect that just worked now ALSO fails -5,
	 * nothing else changed; `wifi disconnect`; the same connect succeeds
	 * again (rssi=-49 dBm).  The second connect's failure decodes as
	 * ALP_CC3501E_WIFI_FAIL_KICK (3): the Wlan_Connect kick itself, not the
	 * SSID/passphrase -- proof it is the stale association from the FIRST
	 * failure wedging the second, not a bad credential.
	 *
	 * This is NOT a role teardown: the STA role is brought up once per
	 * process lifetime (wifi_sta_role_up, pre-cached in
	 * cc3501e_hw_wifi_boot_start()) and must stay up; Wlan_Disconnect does
	 * not take the role down.  What wedges the next Wlan_Connect kick is
	 * association state left inside the NWP by the failed attempt, and
	 * cc3501e_wifi_disconnect() (WIFI_DISCONNECT, 0x13) is the bench-proven
	 * clear for it.
	 *
	 * Cleared at ENTRY, not at the failure exits below: clearing on the way
	 * OUT of a failed connect erased the failure_reason latch a caller
	 * needs to read (cc3501e_hw_wifi_disconnect() does
	 * wifi_conn_set(DISCONNECTED, FAIL_NONE)), could tear down a LATE
	 * success that lands after a poll-exhaustion timeout, and added up to
	 * CC3501E_WIFI_DOWN_WINDOW_MS to every failed call regardless of
	 * timeout_ms.  Clearing at entry, conditional on the latch actually
	 * reading CONN_FAILED, avoids all three: a failed attempt's
	 * state/fail_reason survive untouched for the caller to read, a live
	 * CONNECTING attempt or an already-CONNECTED association is left alone
	 * (see below), and the bound only ever applies to the connect that
	 * follows a failure.
	 *
	 * Uses wifi_status_once() -- the single non-retried attempt (nominally
	 * CC3501E_REQ_TMO_MS; cc3501e_request() does not enforce it, see the
	 * #1481 note below) -- NOT the public cc3501e_wifi_status(), which
	 * rides its own CC3501E_WIFI_DOWN_WINDOW_MS retry and would make a
	 * wedged transport worse here.  If the read fails, or the state is
	 * anything other than CONN_FAILED, this falls through and does
	 * nothing: CONNECTING is a live attempt (today's behaviour is the new
	 * submit bounces BUSY and the status loop below keeps tracking the OLD
	 * attempt), and CONNECTED is connect-while-connected -- a pre-existing,
	 * separate, unowned semantic this fix does not expand into.  The
	 * clear's own result is deliberately discarded -- best-effort, same
	 * reasoning as every other radio-op teardown in this file. */
	alp_cc3501e_wifi_status_t entry_st;
	if (wifi_status_once(ctx, &entry_st) == ALP_OK &&
	    entry_st.state == ALP_CC3501E_WIFI_CONN_FAILED) {
		(void)cc3501e_wifi_disconnect(ctx);
	}

	/* On-wire payload: alp_cc3501e_wifi_connect_t header (4 B) + inline SSID +
	 * inline passphrase, all packed with no padding. */
	uint8_t                    payload[sizeof(alp_cc3501e_wifi_connect_t) + 32u + 64u];
	alp_cc3501e_wifi_connect_t hdr = {
		.ssid_len = (uint8_t)ssid_len,
		.psk_len  = (uint8_t)psk_len,
		.security = sec_type,
		.reserved = 0u,
	};
	size_t off = 0;
	memcpy(&payload[off], &hdr, sizeof(hdr));
	off += sizeof(hdr);
	memcpy(&payload[off], ssid, ssid_len);
	off += ssid_len;
	if (psk_len > 0u) {
		memcpy(&payload[off], pass, psk_len);
		off += psk_len;
	}

	/* SUBMIT ONCE (issues #1376/#1378) -- CONNECT_STA is fire-and-forget on
	 * the firmware side: WORKER_IDLE queues the association and acks
	 * RESP_ERR_BUSY, the drain runs the seconds-long body off THIS exchange,
	 * and the outcome is mirrored into the non-blocking WIFI_STATUS latch --
	 * the host never collects DONE/ERR through this job slot (see
	 * cc3501e-bridge-firmware:src/worker.c's worker_run_pending() comment).  The old
	 * contract poll_by_repeat()'d this opcode on the wire-level BUSY/IO
	 * retry rule; every 50 ms retry that landed on the now-IDLE slot (freed
	 * the instant the drain finished, BEFORE the READY re-arm) submitted a
	 * BRAND NEW association -- five retries, five real join attempts, for
	 * one user command.  A single request removes that loop entirely.
	 *
	 * The submit's own ack is NOT trusted as the connect result, in either
	 * direction.  Only ALP_ERR_INVAL short-circuits: wifi_join() on the
	 * firmware side validates the payload BEFORE touching the worker at all
	 * (protocol_wifi.c), so RESP_ERR_INVALID is a definite synchronous
	 * reject -- nothing was queued, there is nothing to await.  Every other
	 * outcome falls through to the INDEPENDENT WIFI_STATUS channel below,
	 * DELIBERATELY including ALP_ERR_IO: a transport-level fault on the
	 * submit exchange is itself ambiguous evidence (see cc3501e_core.c's
	 * dead-phase note) -- it may mean nothing reached the firmware, or it
	 * may mean a genuine BUSY ack was lost in transit after the submit
	 * landed.  Bailing out on that ambiguity would risk reporting a false
	 * failure for an association that is actually running; polling
	 * WIFI_STATUS instead is safe either way -- it reads DISCONNECTED
	 * (nothing running) through to a timeout if nothing was submitted, or
	 * tracks the real CONNECTING/CONNECTED/FAILED transition if it was.
	 * This is what closes #1378 for this specific race: even a spurious OK
	 * or a spurious IO on the submit exchange cannot make this function
	 * report ALP_OK for an association WIFI_STATUS never confirms. */
	alp_status_t s = cc3501e_request(
	    ctx, ALP_CC3501E_CMD_WIFI_CONNECT_STA, payload, off, NULL, 0, NULL, CC3501E_REQ_TMO_MS);
	if (s == ALP_ERR_INVAL) {
		return s; /* definite synchronous reject -- nothing submitted */
	}

	/* Await the outcome off the non-blocking WIFI_STATUS latch (opcode
	 * 0x1B) -- never off the submit's own ack.  CONNECTING means the
	 * association is still running; CONNECTED / CONN_FAILED are terminal.
	 * Uses wifi_status_once() (a single non-retried attempt), NOT the public
	 * cc3501e_wifi_status() -- see wifi_status_once()'s comment for why
	 * layering that function's own 10 s down-window retry underneath this
	 * loop broke the timeout_ms accounting.
	 *
	 * #1481: this used to bound the loop with a decrementing `remaining`
	 * ledger that DEBITED CC3501E_REQ_TMO_MS (100 ms) as a defensive
	 * estimate of a failed read's worst case, on top of the real
	 * CC3501E_WIFI_STATUS_POLL_GAP_MS (50 ms) it then slept -- 150 ms
	 * charged per failed iteration for ~54 ms of actual wall clock.  On a
	 * board where the CC3501E link is unusable during association (READY
	 * never latches, every poll fails), that phantom debit ran the ledger
	 * to zero at roughly a third of the caller's declared timeout_ms --
	 * silicon-measured twice at timeout_ms=15000 giving up at 5.385 s and
	 * 5.386 s, matching 100 iterations x 150 ms plus per-exchange overhead.
	 * Tuning the constant cannot fix an estimate that has to cover an
	 * unbounded worst case (cc3501e_request() discards timeout_ms outright,
	 * see cc3501e_core.c's `(void)timeout_ms`) -- any finite debit is either
	 * too small to be a real bound or, as here, big enough to starve a
	 * healthy connect.  The loop now bounds itself on real elapsed time
	 * instead: `elapsed_ms` only ever grows by the poll gap this thread
	 * actually slept (alp_delay_ms's contract is "at least this many ms
	 * elapse"), so it is a true lower bound on wall clock and the loop
	 * cannot exit before timeout_ms of real time has passed. It also
	 * cannot hang: the gap clamps to the remaining budget and is checked
	 * for exhaustion before every sleep, so elapsed_ms strictly climbs to
	 * timeout_ms in a bounded number of iterations. What it does NOT
	 * capture is the unmeasured time spent blocked inside a failed read --
	 * there is no portable monotonic clock in this OS-agnostic chips layer
	 * to charge that against (see cc3501e_ota.c's identical constraint) --
	 * but that omission only makes the loop run a little past timeout_ms,
	 * never short of it, which is the safe direction for a caller's
	 * declared budget. */
	uint32_t elapsed_ms = 0u;
	for (;;) {
		alp_cc3501e_wifi_status_t st;
		alp_status_t              ss = wifi_status_once(ctx, &st);
		if (ss == ALP_OK) {
			if (st.state == ALP_CC3501E_WIFI_CONNECTED) return ALP_OK;
			if (st.state == ALP_CC3501E_WIFI_CONN_FAILED) {
				return (st.fail_reason == ALP_CC3501E_WIFI_FAIL_TIMEOUT) ? ALP_ERR_TIMEOUT
				                                                         : ALP_ERR_IO;
			}
			/* DISCONNECTED (not yet latched) or CONNECTING: keep polling.
			 *
			 * No debit here, and no `remaining` ledger anywhere in this loop
			 * any more.  This IS the conversion #1985 tracked and that
			 * #1953 had already made in poll_by_repeat(): the loop now
			 * accumulates elapsed_ms from the gap it actually slept, rather
			 * than decrementing a budget by each attempt's DECLARED
			 * worst-case cost.
			 *
			 * #1985's own note said this "needs re-deriving, not a drop-in
			 * swap", because converting changes the exact attempt counts two
			 * tests assert.  Both were re-derived in the same commit rather
			 * than relaxed:
			 * test_wifi_connect_bounds_status_attempts_on_wedged_transport_1382
			 * moves from <=4 to <=7, which is 1 entry-check read plus
			 * floor(200/50)+1 = 6 loop reads for a 200 ms budget, with slack;
			 * test_wifi_connect_healthy_poll_not_over_debited_1481 keeps its
			 * intent, since removing the phantom debit is exactly what it
			 * was written to protect.
			 *
			 * Why it mattered enough to convert: the old ledger charged
			 * CC3501E_REQ_TMO_MS per failed status read on top of the real
			 * 50 ms gap.  On E1M-AEN801 the READY line never latches, so
			 * EVERY poll during an association fails, and the budget drained
			 * roughly three times faster than the clock -- the caller gave up
			 * while the radio was still legitimately associating.  Measured
			 * on silicon after the change: a connect now consumes its whole
			 * configured budget instead of bailing early. */
		}
		/* ss != ALP_OK: a single status read failing (e.g. a transient
		 * down-window IO) is worth one more pass rather than an immediate
		 * bail -- the next iteration will retry it. */
		if (elapsed_ms >= timeout_ms) return ALP_ERR_TIMEOUT;
		uint32_t gap = ((timeout_ms - elapsed_ms) < CC3501E_WIFI_STATUS_POLL_GAP_MS)
		                   ? (timeout_ms - elapsed_ms)
		                   : CC3501E_WIFI_STATUS_POLL_GAP_MS;
		alp_delay_ms(gap);
		elapsed_ms += gap;
	}
}

alp_status_t cc3501e_wifi_disconnect(cc3501e_t *ctx)
{
	/* Tear down the STA association (WIFI_DISCONNECT, 0x13).  No payload, no
	 * reply data -- success is the OK status.  Disconnect is a radio op
	 * (Wlan_Disconnect), so the bridge can be briefly down (IO/BUSY) while it
	 * runs; floor the budget to the radio down-window and let poll_by_repeat
	 * retry, exactly like cc3501e_wifi_get_mac. */
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_WIFI_DISCONNECT, NULL, 0, NULL, 0, NULL, CC3501E_WIFI_DOWN_WINDOW_MS);
}

alp_status_t cc3501e_wifi_ap_start(cc3501e_t  *ctx,
                                   const char *ssid,
                                   uint8_t     sec_type,
                                   const char *pass,
                                   uint32_t    timeout_ms)
{
	if (ssid == NULL) return ALP_ERR_INVAL;
	size_t ssid_len = strlen(ssid);
	size_t psk_len  = (pass != NULL) ? strlen(pass) : 0u;
	if (ssid_len > 32u || psk_len > 64u) return ALP_ERR_INVAL;

	/* WIFI_AP_START reuses the STA connect wire format: an
	 * alp_cc3501e_wifi_connect_t header (4 B) + inline SSID + inline
	 * passphrase, all packed with no padding (firmware wifi_join validates
	 * both paths against the same struct). */
	uint8_t                    payload[sizeof(alp_cc3501e_wifi_connect_t) + 32u + 64u];
	alp_cc3501e_wifi_connect_t hdr = {
		.ssid_len = (uint8_t)ssid_len,
		.psk_len  = (uint8_t)psk_len,
		.security = sec_type,
		.reserved = 0u,
	};
	size_t off = 0;
	memcpy(&payload[off], &hdr, sizeof(hdr));
	off += sizeof(hdr);
	memcpy(&payload[off], ssid, ssid_len);
	off += ssid_len;
	if (psk_len > 0u) {
		memcpy(&payload[off], pass, psk_len);
		off += psk_len;
	}

	/* SUBMIT ONCE (#1385) -- do NOT poll_by_repeat() this opcode.  AP_START is
	 * fire-and-forget on the firmware side exactly like CONNECT_STA
	 * (handle_worker_routed_payload acks every fresh submit RESP_ERR_BUSY, and
	 * worker_run_pending() resets the job slot for CONNECT_STA/AP_START BEFORE
	 * cc3501e_bridge_ready() re-arms the link, so the WORKER_DONE -> RESP_OK
	 * branch is never collectable).  A poll-by-repeat wrapper around this
	 * opcode is PROVABLY a no-win loop: every attempt lands on either BUSY (no
	 * progress) or the dead-phase 0x00 alias, which cc3501e_request_locked()
	 * now rejects as ALP_ERR_IO (also no progress) -- there is no reply this
	 * opcode can ever produce that reads as ALP_OK.  An earlier revision of
	 * this fix kept the poll anyway (reasoning: "nothing sound to replace it
	 * with"), which left two costs unpaid: the console's only caller passes a
	 * 50 s budget, so `wifi ap` blocked the shell for up to 50 s on a
	 * result that was mathematically already known before the first byte went
	 * on the wire; and every retry that landed on the freshly-reset IDLE slot
	 * submitted a BRAND NEW `Wlan_RoleUp` on live radio hardware -- the same
	 * retry storm #1376 measured and fixed for CONNECT_STA.  Submitting once
	 * and returning immediately removes both costs, but NOT at zero
	 * information loss: the old poll also retried a RESP_ERR_BUSY bounce off
	 * an in-flight worker job and a transport IO fault during the radio-down
	 * window, both cases where nothing had been submitted yet, so dropping it
	 * trades "eventually lands (or reports ALP_ERR_TIMEOUT after genuinely
	 * exhausting the budget)" for "one shot, then ALP_ERR_TIMEOUT either way"
	 * -- see the caller-visible-outcome distinction in the @warning on
	 * cc3501e_wifi_ap_start() in <alp/chips/cc3501e/wifi.h>.
	 *
	 * cc3501e_wifi_connect() escaped the identical trap by submitting once and
	 * then awaiting the independent WIFI_STATUS latch -- AP_START has no such
	 * channel: the TI HAL's cc3501e_hw_wifi_ap_start()
	 * (hal/ti/cc3501e_hw_ti_wifi.c) never writes g_wifi_conn, the latch
	 * handle_wifi_status reads.  Giving AP_START one is a FIRMWARE change
	 * (mirror the AP outcome into a latch, or add an AP-status opcode + a
	 * protocol version bump) and needs a bench, so it is not made here.
	 * @p timeout_ms is therefore currently unused: there is nothing left to
	 * bound a retry loop over.  It stays in the signature (ABI/API stable) so
	 * a future firmware-side confirmation channel can reuse it exactly as
	 * cc3501e_wifi_connect() uses its own timeout_ms, without an API break. */
	alp_status_t s = cc3501e_request(
	    ctx, ALP_CC3501E_CMD_WIFI_AP_START, payload, off, NULL, 0, NULL, CC3501E_REQ_TMO_MS);
	/* Only ALP_ERR_INVAL and ALP_ERR_NOT_READY are definite, conclusive
	 * answers -- the former is wifi_join()'s synchronous pre-worker validation
	 * reject (same firmware function CONNECT_STA's payload goes through, see
	 * cc3501e_wifi_connect()'s identical short-circuit), the latter never
	 * reached the wire at all (ctx NULL/uninitialised).  Every other outcome
	 * squashes to ALP_ERR_TIMEOUT, and NOT all of them mean "submitted": the
	 * expected RESP_ERR_BUSY submit ack and the rejected dead-phase alias did
	 * reach the wire, but a RESP_ERR_BUSY bounce off an in-flight worker job
	 * (cmd never queued, cc3501e-bridge-firmware:src/protocol.c's QUEUED/RUNNING
	 * default: case), a transport IO fault during the radio-down window, and
	 * cc3501e_request()'s own ALP_ERR_BUSY when cc3501e_lock_acquire() times
	 * out under a concurrent caller all mean NOTHING was submitted -- and read
	 * back identical to the cases that did.  ALP_ERR_TIMEOUT here is therefore
	 * fully inconclusive, not "submitted, unconfirmed" (see the @warning on
	 * cc3501e_wifi_ap_start() in <alp/chips/cc3501e/wifi.h>). */
	if (s == ALP_ERR_INVAL || s == ALP_ERR_NOT_READY) {
		return s;
	}

	/* CONFIRM against an independent channel (#1696).
	 *
	 * The submit ack carries no information -- see the block above -- so the
	 * outcome has to be read from somewhere the AP path actually writes.  It
	 * does write one: cc3501e_hw_wifi_ap_start() sets `wifi_ap_role_up` on a
	 * successful Wlan_RoleUp, cc3501e_hw_radio_role() turns that into
	 * ROLE_WIFI_AP, and GET_DIAG_INFO publishes it as byte 3 of its reply
	 * (cc3501e-bridge-firmware:src/protocol_diag.c).  That is exactly the
	 * independent confirmation channel this wrapper was missing.
	 *
	 * The comment above used to say no such channel existed and that giving
	 * AP_START one was a firmware change plus a protocol bump.  That was true
	 * of firmware v4, which is what it was written against: the `role` field
	 * arrived later (for #1562) and the wire is v5 now.  No firmware change
	 * and no version bump are involved here -- only the host learning to read
	 * a field the firmware has been publishing all along.
	 *
	 * cc3501e_diag_info() is explicitly non-disturbing (no side effects on
	 * radio state), so polling it cannot perturb the AP being confirmed --
	 * unlike re-submitting AP_START, which put a fresh Wlan_RoleUp on live
	 * radio hardware every retry (the #1376 storm).  Still submit ONCE.
	 *
	 * Budget accounting used to mirror cc3501e_wifi_connect(): debit the
	 * attempt's declared worst case ONLY when the read itself failed.
	 * cc3501e_wifi_connect() has since moved off this estimate-based ledger
	 * entirely (see its #1481 note) because the debit above is unbounded on
	 * a wedged transport and, even confined to the failure branch, still
	 * overcharges every failed poll's real wall-clock cost against
	 * timeout_ms.  This loop has the identical defect and is a candidate for
	 * the same fix; left alone here as out of scope for #1481's fix targeted
	 * at cc3501e_wifi_connect(). */
	uint32_t remaining = timeout_ms;
	for (;;) {
		alp_cc3501e_diag_info_t di = { 0 };
		const alp_status_t      ds = cc3501e_diag_info(ctx, &di);

		if (ds == ALP_OK) {
			if (di.role == (uint8_t)ALP_CC3501E_ROLE_WIFI_AP) {
				return ALP_OK;
			}
			/* Role not up yet: no attempt_cost debit here, same #1481
			 * reasoning as cc3501e_wifi_connect() above -- but a
			 * successful read is not actually free wall-clock time, the
			 * same premise issue #1953 disproved for poll_by_repeat().
			 * Deferred, not fixed here: #1985. */
		} else {
			/* One failed read is worth another pass, but charge its declared
			 * worst case so `remaining` cannot ignore the failure path --
			 * cc3501e_request() does not itself bound an attempt. */
			uint32_t attempt_cost =
			    (CC3501E_REQ_TMO_MS < remaining) ? CC3501E_REQ_TMO_MS : remaining;
			remaining -= attempt_cost;
		}
		if (remaining == 0u) return ALP_ERR_TIMEOUT;
		uint32_t gap = (remaining < CC3501E_WIFI_STATUS_POLL_GAP_MS)
		                   ? remaining
		                   : CC3501E_WIFI_STATUS_POLL_GAP_MS;
		alp_delay_ms(gap);
		remaining -= gap;
	}
}

alp_status_t cc3501e_wifi_ap_stop(cc3501e_t *ctx)
{
	/* Tear down the soft-AP (WIFI_AP_STOP, 0x15).  No payload, no reply data --
	 * success is the OK status.  Like cc3501e_wifi_disconnect this is a radio
	 * op, so the bridge can be briefly down (IO/BUSY) while it runs; floor the
	 * budget to the radio down-window and let poll_by_repeat retry. */
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_WIFI_AP_STOP, NULL, 0, NULL, 0, NULL, CC3501E_WIFI_DOWN_WINDOW_MS);
}

alp_status_t cc3501e_wifi_rssi(cc3501e_t *ctx, int8_t *rssi)
{
	if (rssi == NULL) return ALP_ERR_INVAL;
	uint8_t reply[1] = { 0 };
	size_t  got      = 0;
	/* WIFI_GET_RSSI is WORKER-ROUTED on the firmware side (handle_worker_
	 * routed in protocol_wifi.c: Wlan_Get(WLAN_GET_RSSI) lazy-starts the
	 * radio on first use, which can take seconds and so must run off the SPI
	 * ISR) -- the firmware acks a fresh submit with RESP_ERR_BUSY and only
	 * returns RESP_OK once the drain has collected the value.  #1377: a
	 * single cc3501e_request() here essentially never lands on the DONE
	 * poll -- it collects the submit's own BUSY ack, reports it up as
	 * ALP_ERR_BUSY, and leaves the job orphaned in the single job slot for
	 * the next unrelated worker-routed call to trip over.  Poll-by-repeat,
	 * floored to the radio down-window, exactly like cc3501e_wifi_get_mac. */
	alp_status_t s = poll_by_repeat(ctx,
	                                ALP_CC3501E_CMD_WIFI_GET_RSSI,
	                                NULL,
	                                0,
	                                reply,
	                                sizeof(reply),
	                                &got,
	                                CC3501E_WIFI_DOWN_WINDOW_MS);
	if (s != ALP_OK) return s;
	if (got < 1u) return ALP_ERR_IO;
	*rssi = (int8_t)reply[0];
	return ALP_OK;
}

alp_status_t cc3501e_wifi_get_ip(cc3501e_t *ctx, uint8_t iface, uint8_t ip[4])
{
	if (ip == NULL) return ALP_ERR_INVAL;
	if (iface != (uint8_t)ALP_CC3501E_WIFI_IFACE_STA &&
	    iface != (uint8_t)ALP_CC3501E_WIFI_IFACE_AP) {
		return ALP_ERR_INVAL;
	}
	/* Protocol v9: the request carries one interface-selector byte.  A
	 * zero-length request still means STA on the firmware side, but this host
	 * only ever talks to a matching-version firmware (cc3501e_reset()'s
	 * GET_VERSION gate), so always send the explicit byte. */
	uint8_t      req[1]   = { iface };
	uint8_t      reply[4] = { 0 };
	size_t       got      = 0;
	alp_status_t s        = cc3501e_request(ctx,
	                                        ALP_CC3501E_CMD_WIFI_GET_IP,
	                                        req,
	                                        sizeof(req),
	                                        reply,
	                                        sizeof(reply),
	                                        &got,
	                                        CC3501E_REQ_TMO_MS);
	if (s != ALP_OK) {
		/* #2035: WIFI_GET_IP's own firmware handler (hal/ti/cc3501e_hw_ti_wifi.c)
		 * answers ALP_CC3501E_RESP_ERR_RADIO for THREE distinct "there is no
		 * address" conditions on this opcode -- the network stack not up, the
		 * address lookup failing, or a genuine 0.0.0.0 lease -- because it has
		 * no dedicated "not-ready" status of its own on this path.  Left as
		 * plain ALP_ERR_IO, that is INDISTINGUISHABLE at the call site from a
		 * transport that is actually broken (a failed transceive, a malformed
		 * reply) -- both come back as the same code.  That ambiguity is exactly
		 * what cost a multi-week bench investigation (#2035): the radio had
		 * associated (RSSI read -75 dBm right after the "failed" connect,
		 * corroborated by the scan's identical reading), yet thirty 1 Hz
		 * get_ip polls all came back ALP_ERR_IO and were read as "the
		 * instrument did not read" rather than "there is no address" -- because
		 * the code genuinely could not say which.
		 *
		 * The disambiguator is the poisoning guarantee documented on
		 * ALP_CC3501E_RX_SCRATCH_NO_STATUS and cc3501e_request_locked()'s out:
		 * label: ctx->rx_scratch[0] holds a real decoded ALP_CC3501E_RESP_ERR_*
		 * code ONLY when a status byte was genuinely decoded off the wire --
		 * never leftover residue from a pre-decode failure.  So a RESP_ERR_RADIO
		 * seen here means the firmware answered and told us it has no address;
		 * report that distinctly as ALP_ERR_NOT_READY ("poll again").  Any other
		 * failure -- including this cc3501e_request() call itself returning
		 * ALP_ERR_IO for a reason OTHER than RESP_ERR_RADIO, or the short-reply
		 * check below -- is a genuine wire fault and stays ALP_ERR_IO.  Do not
		 * collapse these back together: that is the bug this fixes. */
		if (s == ALP_ERR_IO && ctx->rx_scratch[0] == ALP_CC3501E_RESP_ERR_RADIO) {
			return ALP_ERR_NOT_READY;
		}
		return s;
	}
	if (got < 4u) return ALP_ERR_IO;
	/* Byte-order normalise (host-only): the firmware derives these 4 bytes from the
	 * lwIP netif address -- a NETWORK-order u32 (netif_ip4_addr()->addr) -- but extracts
	 * it MSB-first, so on the wire the octets arrive REVERSED (192.168.1.14 -> the wire
	 * bytes [14,1,168,192]).  Reverse them here to canonical dotted-quad order
	 * (ip[0]=192 ... ip[3]=14), which is directly printable AND matches the ip[]
	 * convention cc3501e_sock_connect expects (network order, ip[0] = most-significant
	 * octet) -- so a get_ip result can feed straight back into a connect. */
	ip[0] = reply[3];
	ip[1] = reply[2];
	ip[2] = reply[1];
	ip[3] = reply[0];
	return ALP_OK;
}

alp_status_t cc3501e_wifi_status(cc3501e_t *ctx, alp_cc3501e_wifi_status_t *out)
{
	if (out == NULL) return ALP_ERR_INVAL;

	/* Reply is the fixed 4-byte alp_cc3501e_wifi_status_t wire layout (no
	 * padding): state | fail_reason | rssi_dbm | reserved.  The FIRMWARE-side
	 * handler (handle_wifi_status) is a genuine non-blocking latch read -- no
	 * radio op, ISR-safe, always replies RESP_OK -- but the shared bridge
	 * TRANSPORT is briefly down whenever ANY radio op is in flight (Wlan_Start
	 * at boot, or a worker connect/scan/rssi body), exactly like every other
	 * op in this file.  #1377: a status read taken moments after a connect
	 * submit can land in that down-window and desync like any other
	 * transaction (ver / scan / connect all healthy, then every SUBSEQUENT
	 * status call failed -5) -- that is the shared TRANSPORT contract, not
	 * something specific to the WIFI_STATUS opcode, so this needs the same
	 * poll-by-repeat down-window handling as the radio-adjacent ops even
	 * though the opcode itself never replies BUSY.  Only the IO-retry leg of
	 * poll_by_repeat ever fires here in practice. */
	uint8_t      reply[4] = { 0 };
	size_t       got      = 0;
	alp_status_t s        = poll_by_repeat(ctx,
	                                       ALP_CC3501E_CMD_WIFI_STATUS,
	                                       NULL,
	                                       0,
	                                       reply,
	                                       sizeof(reply),
	                                       &got,
	                                       CC3501E_WIFI_DOWN_WINDOW_MS);
	if (s != ALP_OK) return s;
	if (got < sizeof(reply)) return ALP_ERR_IO; /* short reply -- firmware/wire gap */

	/* Decode wire -> struct field by field (matches the packed layout in
	 * alp/protocol/cc3501e.h), mirroring how cc3501e_wifi_scan walks records. */
	out->state       = reply[0];
	out->fail_reason = reply[1];
	out->rssi_dbm    = (int8_t)reply[2];
	out->reserved    = reply[3];
	return ALP_OK;
}
