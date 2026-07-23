/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-cc3501e-ble-gatt -- bench proof of the #480 CC3501E BLE GATT-SERVER
 * path through the PORTABLE <alp/ble.h> surface, on the E1M-AEN801 (Alif
 * Ensemble E8, M55-HE).
 *
 * WHAT THIS PROVES.  <alp/ble.h> is backend-dispatched (src/ble_dispatch.c):
 * on AEN, alp_ble_open() selects src/backends/ble/cc3501e.c, which forwards
 * every call over the inter-chip SPI1 bridge to the TI CC3501E's OWN
 * Cortex-M33 -- the CC35 runs the real BLE controller + NimBLE host, not the
 * Alif.  So "the portable GATT path" here means: app calls alp_ble_*() ->
 * ble_dispatch.c -> backends/ble/cc3501e.c -> chips/cc3501e/ble.h wire
 * commands (<alp/protocol/cc3501e.h> opcodes 0x30..0x3B) -> the CC35
 * firmware.  It is a full-stack coprocessor bridge, NOT a bare HCI
 * controller the Alif drives directly.
 *
 * SERVER-ONLY, NO LIVE CENTRAL PEER ON THIS BENCH.  There is no second BLE
 * device in range to connect to, so this app never calls alp_ble_connect().
 * That shapes the PASS contract below in a way worth reading carefully
 * BEFORE assuming a non-ALP_OK return means something is broken:
 *
 *   1. alp_ble_open()               -- expect non-NULL.  Internally routes
 *      through cc35_open() (backends/ble/cc3501e.c:86-104), which calls
 *      cc3501e_ble_enable() -- a REAL wire round-trip (BLE_ENABLE, 0x30)
 *      that brings the CC35's Wi-Fi stack + NimBLE host up.
 *
 *   2. alp_ble_gatt_register_service() -- expect ALP_ERR_NOSUPPORT, not
 *      ALP_OK.  cc35_gatt_register_service() (backends/ble/cc3501e.c:
 *      145-155) is an intentional, documented stub: "The firmware takes an
 *      opaque packed GATT descriptor.  The portable runtime
 *      service-definition encoder is a later slice."  Getting NOSUPPORT
 *      back -- not a hang, not a silently-wrong handle -- IS the proof that
 *      the portable dispatch reaches the real CC3501E backend and that
 *      backend correctly reports its own current limit instead of
 *      pretending to succeed.
 *
 *   3. alp_ble_advertise_start() -- expect ALP_OK.  This is the one call in
 *      this app that is a genuine over-the-bridge wire round-trip to
 *      firmware that already exists end to end (BLE_ADV_START, 0x32,
 *      chips/cc3501e/ble.h) -- the real "on silicon" proof leg.
 *
 *   4. gatt_read / gatt_write / gatt_notify -- called with a NULL
 *      alp_ble_conn_t* (there is no connection to pass: <alp/ble.h> models
 *      these as PEER-scoped ops taken over a connection returned by
 *      alp_ble_connect(), and central-role connect is out of scope on a
 *      server-only, no-peer bench -- see chips/cc3501e/ble.h's
 *      cc3501e_ble_gatt_read/write, documented as needing "BLE enabled +
 *      CONNECTED").  Calling them anyway with conn=NULL is the "local"
 *      exercise the task asks for: it proves the dispatcher's
 *      connection-handle guard (src/ble_dispatch.c) rejects the call
 *      BEFORE it ever reaches the wire, with the exact documented status,
 *      rather than crashing on a NULL deref or blocking forever:
 *        - gatt_read(NULL, ...)  -> ALP_ERR_NOT_READY  (ble_dispatch.c: c
 *          == NULL fails alp_handle_op_enter up front)
 *        - gatt_write(NULL, ...) -> ALP_ERR_NOT_READY  (same guard)
 *        - gatt_notify(ble, NULL, ...) -> ALP_ERR_INVAL (ble_dispatch.c
 *          checks `conn == NULL` explicitly and returns INVAL, a
 *          DIFFERENT code from the read/write guard -- both are asserted
 *          below so a future dispatch refactor that blurs them is caught)
 *      A full read/write ECHO against a live central is explicitly
 *      HIL-deferred: it needs a second BLE peer on the bench, which this
 *      single-board setup does not have.
 *
 * This file is ~50% comment by design: examples are documentation.
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "alp/ble.h"
#include "alp/chips/cc3501e.h"
#include "alp/peripheral.h"

#include "cc3501e_bridge.h" /* cc3501e_bridge_bringup() -- the SoM bring-up helper */

/* Poll-by-repeat budget for the first PING: reset() already waited out the
 * boot budget, so this just absorbs residual ramp/boot jitter (same
 * constants as aen-cc3501e-bringup / aen-cc3501e-companion-tour). */
#define GATT_PING_RETRIES 25u
#define GATT_PING_GAP_MS  200u

/* Caller budget passed to the local (no-peer) gatt_read/write calls below.
 * It is never actually waited out: both calls reject on the NULL conn
 * before any timeout-driven wait starts (see the file header). Kept
 * non-zero anyway so the call shape matches a real central-role caller's. */
#define GATT_LOCAL_OP_TIMEOUT_MS 2000u

/*
 * Example/test-only UUIDs -- NOT allocated from the BT-SIG / a real vendor
 * range.  A production service must use a UUID it actually owns (a 16-bit
 * SIG-assigned UUID, or a 128-bit UUID generated for the product).
 */
static const alp_ble_uuid_t GATT_SVC_UUID   = { .b = { 0xA1,
	                                                   0xB0,
	                                                   0xE1,
	                                                   0x60,
	                                                   0x00,
	                                                   0x01,
	                                                   0x00,
	                                                   0x02,
	                                                   0x00,
	                                                   0x03,
	                                                   0x00,
	                                                   0x04,
	                                                   0x00,
	                                                   0x05,
	                                                   0x00,
	                                                   0x01 } };
static const alp_ble_uuid_t GATT_CHAR1_UUID = { .b = { 0xA1,
	                                                   0xB0,
	                                                   0xE1,
	                                                   0x60,
	                                                   0x00,
	                                                   0x01,
	                                                   0x00,
	                                                   0x02,
	                                                   0x00,
	                                                   0x03,
	                                                   0x00,
	                                                   0x04,
	                                                   0x00,
	                                                   0x05,
	                                                   0x00,
	                                                   0x02 } };
static const alp_ble_uuid_t GATT_CHAR2_UUID = { .b = { 0xA1,
	                                                   0xB0,
	                                                   0xE1,
	                                                   0x60,
	                                                   0x00,
	                                                   0x01,
	                                                   0x00,
	                                                   0x02,
	                                                   0x00,
	                                                   0x03,
	                                                   0x00,
	                                                   0x04,
	                                                   0x00,
	                                                   0x05,
	                                                   0x00,
	                                                   0x03 } };

/* Track pass/fail across the checkpoints below so main() can print one
 * RESULT PASS:/RESULT FAIL: line at the end (the bench-observable contract
 * -- see flashing-and-bench-debugging-aen's "how to verify" section). */
static unsigned g_fail_count;

/* Print + tally one checkpoint.  `what` names the call, `got` is its
 * return, `want` is the value THIS bench (server-only, no live peer) is
 * expected to see -- see the file header for why several of these are
 * deliberately not ALP_OK. */
static void gatt_check(const char *what, alp_status_t got, alp_status_t want)
{
	if (got == want) {
		printf("[gatt] %-24s -> %s (expected)\n", what, alp_status_name(got));
	} else {
		printf("[gatt] %-24s -> %s (expected %s)\n",
		       what,
		       alp_status_name(got),
		       alp_status_name(want));
		g_fail_count++;
	}
}

int main(void)
{
	printf("\n[gatt] E1M-AEN CC3501E BLE GATT-SERVER bench proof (#480, server-only, no peer)\n");

	/*
	 * Step 1 -- SoM bring-up (control pins + inter-chip SPI + power/reset),
	 * ONE call: cc3501e_bridge_bringup() (cc3501e_bridge.{c,h}, the same
	 * reusable template aen-cc3501e-bringup / -companion-tour use).
	 */
	cc3501e_t    fw;
	alp_status_t s = cc3501e_bridge_bringup(&fw);
	if (s != ALP_OK) {
		printf("RESULT FAIL: cc3501e_bridge_bringup -> %s (check the board overlay / "
		       "WIFI_EN+nRESET wiring)\n",
		       alp_status_name(s));
		return 0;
	}
	printf("[gatt] bridge bring-up -> %s\n", alp_status_name(s));

	/* Step 2 -- liveness.  A serviced PING proves the firmware parsed a
	 * frame and staged its reply over the hardware-framed bridge -- the
	 * gate every later step depends on. */
	bool linked = false;
	for (unsigned attempt = 0u; attempt < GATT_PING_RETRIES; ++attempt) {
		if (cc3501e_ping(&fw) == ALP_OK) {
			printf("[gatt] PING ok after %u attempt%s\n", attempt + 1u, (attempt == 0u) ? "" : "s");
			linked = true;
			break;
		}
		alp_delay_ms(GATT_PING_GAP_MS);
	}
	if (!linked) {
		printf("RESULT FAIL: coprocessor never answered PING -- check power (WIFI_EN), the "
		       "SPI1 pinmux, and that the CC3501E is running its firmware\n");
		return 0;
	}

	/*
	 * Step 3 -- open the portable BLE radio.  On AEN this selects
	 * backends/ble/cc3501e.c, whose open() calls cc3501e_ble_enable()
	 * under the hood (BLE_ENABLE, 0x30) -- a real wire round-trip that
	 * brings the CC35's NimBLE host up.  A non-NULL return covers BOTH
	 * "bridge ready" and "cc3501e_ble_enable OK" from the task's PASS
	 * contract in one checkpoint (that IS the call cc35_open makes).
	 */
	alp_ble_t *ble = alp_ble_open();
	if (ble == NULL) {
		printf("RESULT FAIL: alp_ble_open -> NULL, err=%s (BLE not built into this "
		       "CC3501E firmware image?)\n",
		       alp_status_name(alp_last_error()));
		return 0;
	}
	printf("[gatt] alp_ble_open -> ready (cc3501e_ble_enable succeeded)\n");

	/*
	 * Step 4 -- register a 2-characteristic GATT service via the portable
	 * API.  Expect ALP_ERR_NOSUPPORT -- see the file header for why that
	 * is the correct, current answer for this backend, not a bug.
	 */
	static const uint8_t     char1_initial[4] = { 0x00, 0x00, 0x00, 0x00 };
	const alp_ble_char_def_t chars[2]         = {
		{ .uuid          = GATT_CHAR1_UUID,
		  .properties    = ALP_BLE_GATT_PROP_READ | ALP_BLE_GATT_PROP_WRITE,
		  .initial_value = char1_initial,
		  .initial_len   = sizeof(char1_initial) },
		{ .uuid          = GATT_CHAR2_UUID,
		  .properties    = ALP_BLE_GATT_PROP_NOTIFY,
		  .initial_value = NULL,
		  .initial_len   = 0u },
	};
	const alp_ble_service_def_t svc = {
		.service_uuid = GATT_SVC_UUID,
		.chars        = chars,
		.num_chars    = 2u,
	};
	alp_ble_attr_handle_t handles[2] = { 0u, 0u };
	alp_status_t          rc_reg     = alp_ble_gatt_register_service(ble, &svc, handles);
	gatt_check("gatt_register_service", rc_reg, ALP_ERR_NOSUPPORT);

	/*
	 * Step 5 -- start advertising.  Expect ALP_OK: this is the genuine
	 * over-the-bridge silicon proof leg (BLE_ADV_START, 0x32).
	 *
	 * ADV-DATA BUDGET (bench-proven on E1M-AEN801, #480): the CC3501E
	 * BLE_ADV_START wire carries ONE legacy 31-byte advertising payload and
	 * has no scan-response field, so the backend cannot spill the name into a
	 * SCAN_RSP the way a full host stack would -- everything must fit 31 bytes:
	 *   Flags            = 3                (2 hdr + 1, always emitted first)
	 * + Complete Name    = 2 + strlen(name)
	 * + 128-bit UUID     = 2 + 16*num_services
	 * With one service UUID that leaves the name <= 8 chars.  A longer name
	 * overflows and cc3501e.c pack_adv_data() rejects the whole config with
	 * ALP_ERR_INVAL (the first cut of this example used a 15-char name and
	 * FAILED on silicon exactly here).  Keep the service UUID advertised (it is
	 * what a GATT-server proof should announce) and keep the name <= 8.
	 */
	alp_ble_adv_config_t adv = ALP_BLE_ADV_CONFIG_DEFAULT("AenGATT");
	adv.services             = &GATT_SVC_UUID;
	adv.num_services         = 1u;
	alp_status_t rc_adv      = alp_ble_advertise_start(ble, &adv);
	gatt_check("advertise_start", rc_adv, ALP_OK);

	/*
	 * Step 6 -- the "local" gatt read/write/notify exercise: no live
	 * central peer means no alp_ble_conn_t, so these are called with a
	 * NULL conn on purpose (see the file header for exactly which guard
	 * each expected status proves).  handles[0]/[1] are never populated
	 * by Step 4 (NOSUPPORT left them 0) -- irrelevant here, since every
	 * one of these calls is rejected before the handle is ever used.
	 */
	uint8_t      read_buf[32] = { 0 };
	size_t       read_len     = 0u;
	alp_status_t rc_read      = alp_ble_gatt_read(
	    NULL, handles[0], read_buf, sizeof(read_buf), &read_len, GATT_LOCAL_OP_TIMEOUT_MS);
	gatt_check("gatt_read(no conn)", rc_read, ALP_ERR_NOT_READY);

	static const uint8_t write_val[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
	alp_status_t         rc_write     = alp_ble_gatt_write(
	    NULL, handles[0], write_val, sizeof(write_val), GATT_LOCAL_OP_TIMEOUT_MS);
	gatt_check("gatt_write(no conn)", rc_write, ALP_ERR_NOT_READY);

	static const uint8_t notify_val[2] = { 0x01, 0x00 };
	alp_status_t         rc_notify =
	    alp_ble_gatt_notify(ble, NULL, handles[1], notify_val, sizeof(notify_val));
	gatt_check("gatt_notify(no conn)", rc_notify, ALP_ERR_INVAL);

	/* Cleanup.  Not part of the PASS contract (advertising already proved
	 * itself in Step 5); logged for bench visibility only. */
	printf("[gatt] advertise_stop -> %s\n", alp_status_name(alp_ble_advertise_stop(ble)));
	alp_ble_close(ble);

	if (g_fail_count == 0u) {
		printf("RESULT PASS: bridge ready, ble_open ok, register_service NOSUPPORT "
		       "(expected -- stub), advertise_start OK, gatt_read/write/notify all "
		       "correctly rejected with no live peer\n");
	} else {
		printf("RESULT FAIL: %u gatt checkpoint(s) did not match the expected "
		       "server-only/no-peer status -- see [gatt] lines above\n",
		       g_fail_count);
	}
	return 0;
}
