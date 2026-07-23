/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-cc3501e-gatt-register -- bench check for the NEW CC3501E dynamic GATT
 * registration path (#480/#892) on the Alif Ensemble E8 (M55-HE).
 *
 * WHAT CHANGED UPSTREAM.  `alp_ble_gatt_register_service()` used to be a
 * stub; on this branch it really registers a service on the CC3501E's
 * NimBLE host over the inter-chip bridge and fills in a real attribute
 * handle per characteristic.  This app is the on-silicon proof, run
 * PEER-FREE -- there is no BLE central on the bench, so it cannot do full
 * central-side service discovery.  What it CAN prove without a peer:
 *
 *   1. registration returns real (non-zero) handles -- the registration
 *      itself worked, not just "the call didn't crash";
 *   2. the bridge is still alive and answering AFTER registration -- the
 *      on-silicon check for a NimBLE double-`ble_gatts_start()` use-after-
 *      free that the register path used to trigger (now fixed by resetting
 *      the GATT table via a single `ble_gatts_start()` per register, not a
 *      stray extra call);
 *   3. the register-then-advertise ORDERING GUARD: NimBLE's
 *      `ble_gatts_mutable()` refuses to re-run `ble_gatts_start()` once the
 *      stack is advertising/scanning/connected, so a SECOND register call
 *      issued while advertising must come back `ALP_ERR_BUSY` (see the
 *      @note on `alp_ble_gatt_register_service()` in <alp/ble.h>, and the
 *      wire-level remap comment in chips/cc3501e/cc3501e_ble.c).
 *
 * Full central-side verification (a real central connecting and reading
 * the two characteristics by their discovered handles) is HIL-deferred --
 * it needs a second BLE radio on the bench, which this rig does not have.
 *
 * THE SIX-STEP PASS CONTRACT (order matters -- register must precede
 * advertise, per the @note above):
 *
 *   1. cc3501e_bridge_bringup() + PING -> ALP_OK           (bridge ready)
 *   2. alp_ble_open()            -> non-NULL               (BLE enabled)
 *   3. alp_ble_gatt_register_service() (2 chars)
 *        -> ALP_OK AND both handles non-zero                (registered)
 *   4. a second bridge round-trip (PING) post-register
 *        -> still ALP_OK                                    (UAF-fix check)
 *   5. alp_ble_advertise_start() (<=8-char name)
 *        -> ALP_OK                                          (advertising)
 *   6. alp_ble_gatt_register_service() AGAIN, while advertising
 *        -> ALP_ERR_BUSY                                     (ordering guard)
 *
 * A single `RESULT PASS: ...` line means every step above held; the first
 * step that didn't prints `RESULT FAIL: <which step>` and the app stops --
 * this is a bench GATE, unlike the companion-tour's guarded non-fatal demo
 * steps.
 *
 * Console is the RAM buffer `ram_console_buf` (board.yaml `diagnostics.console:
 * ram`); this bench rig has no UART wired to USB.  BENCH-VALIDATION app --
 * not a customer teaching example (see aen-cc3501e-companion-tour for that).
 *
 * This file is ~50% comment by design: examples are documentation, and a
 * bench-gate app doubles as the readable spec of what "the fix works" means.
 */

#include <stdbool.h>
#include <stdio.h>

#include "alp/ble.h"
#include "alp/chips/cc3501e.h"
#include "alp/peripheral.h"

#include "cc3501e_bridge.h" /* cc3501e_bridge_bringup() -- the SoM bring-up helper */

/* Poll-by-repeat budgets.  PING is cheap + direct (see cc3501e_ping's doc);
 * a handful of retries absorbs residual boot/ramp jitter after
 * cc3501e_bridge_bringup() already waited out the fixed boot budget. */
#define GATT_PING_RETRIES 25u
#define GATT_PING_GAP_MS  200u

/*
 * Example-only vendor 128-bit service + characteristic UUIDs (bytes are
 * little-endian per alp_ble_uuid_t's doc comment).  These are NOT a
 * registered BT-SIG UUID -- a real product picks its own random 128-bit
 * UUID per service (e.g. `uuidgen`).  Three distinct UUIDs, one per
 * service/characteristic, differing only in the last two bytes so the
 * relationship is easy to eyeball in a hex dump.
 */
static const alp_ble_uuid_t GATT_SVC_UUID = { .b = { 0x01,
	                                                 0x02,
	                                                 0x03,
	                                                 0x04,
	                                                 0x05,
	                                                 0x06,
	                                                 0x07,
	                                                 0x08,
	                                                 0x09,
	                                                 0x0a,
	                                                 0x0b,
	                                                 0x0c,
	                                                 0x0d,
	                                                 0x0e,
	                                                 0xa0,
	                                                 0xa1 } };

static const alp_ble_uuid_t GATT_CHR_RO_UUID = { .b = { 0x01,
	                                                    0x02,
	                                                    0x03,
	                                                    0x04,
	                                                    0x05,
	                                                    0x06,
	                                                    0x07,
	                                                    0x08,
	                                                    0x09,
	                                                    0x0a,
	                                                    0x0b,
	                                                    0x0c,
	                                                    0x0d,
	                                                    0x0e,
	                                                    0xb0,
	                                                    0xb1 } };

static const alp_ble_uuid_t GATT_CHR_NOTIFY_UUID = { .b = { 0x01,
	                                                        0x02,
	                                                        0x03,
	                                                        0x04,
	                                                        0x05,
	                                                        0x06,
	                                                        0x07,
	                                                        0x08,
	                                                        0x09,
	                                                        0x0a,
	                                                        0x0b,
	                                                        0x0c,
	                                                        0x0d,
	                                                        0x0e,
	                                                        0xc0,
	                                                        0xc1 } };

/* Small initial values -- just enough bytes to prove the descriptor encode
 * (chips/cc3501e/cc3501e_ble.c's cc35_gatt_register_service) round-trips
 * non-empty payloads, not a real sensor reading. */
static const uint8_t GATT_REG_RO_INITIAL[]     = { 0x2a };
static const uint8_t GATT_REG_NOTIFY_INITIAL[] = { 0x00 };

/* Step: PING until the coprocessor answers (or the retry budget elapses).
 * Used both for the initial link-up check (step 1) and the post-register
 * liveness check (step 4) -- same helper, different meaning at each call
 * site (see the call sites below). */
static bool gatt_ping(cc3501e_t *fw)
{
	for (unsigned i = 0u; i < GATT_PING_RETRIES; ++i) {
		if (cc3501e_ping(fw) == ALP_OK) {
			return true;
		}
		alp_delay_ms(GATT_PING_GAP_MS);
	}
	return false;
}

int main(void)
{
	printf("\n[gatt-register] E1M-AEN CC3501E dynamic GATT registration bench check\n");

	/* ---- Step 1: bridge bring-up + PING -------------------------------- */
	cc3501e_t    fw;
	alp_status_t s = cc3501e_bridge_bringup(&fw);
	if (s != ALP_OK) {
		printf("RESULT FAIL: step 1 bridge bring-up -> %s (check WIFI_EN/SPI1 wiring)\n",
		       alp_status_name(s));
		return 0;
	}
	if (!gatt_ping(&fw)) {
		printf("RESULT FAIL: step 1 PING never answered after bring-up\n");
		return 0;
	}
	printf("[gatt-register] step 1 OK: bridge up + PING answered\n");

	/* ---- Step 2: alp_ble_open() ----------------------------------------
	 * The CC3501E BLE backend's open() (src/backends/ble/cc3501e.c
	 * cc35_open) calls cc3501e_ble_enable() on the first reference, which
	 * starts the firmware's shared-HIF Wi-Fi stack then the NimBLE host --
	 * a non-NULL return here means BOTH came up. */
	alp_ble_t *ble = alp_ble_open();
	if (ble == NULL) {
		printf("RESULT FAIL: step 2 alp_ble_open() -> NULL (err=%s)\n",
		       alp_status_name(alp_last_error()));
		return 0;
	}
	printf("[gatt-register] step 2 OK: alp_ble_open() -> %p (BLE controller + NimBLE up)\n",
	       (void *)ble);

	/* ---- Step 3: register a 2-characteristic service --------------------
	 * READ char first (a passive attribute), READ|NOTIFY second (the one a
	 * real central would subscribe to). Register-before-advertise is the
	 * hard ordering rule this whole app exists to prove -- see the file
	 * banner. A non-zero handle in BOTH slots is the only proof (short of a
	 * central connecting) that the firmware's NimBLE host actually built
	 * GATT attribute-table entries instead of silently no-op'ing. */
	alp_ble_char_def_t chars[2] = {
		{
		    .uuid          = GATT_CHR_RO_UUID,
		    .properties    = ALP_BLE_GATT_PROP_READ,
		    .initial_value = GATT_REG_RO_INITIAL,
		    .initial_len   = sizeof(GATT_REG_RO_INITIAL),
		},
		{
		    .uuid          = GATT_CHR_NOTIFY_UUID,
		    .properties    = ALP_BLE_GATT_PROP_READ | ALP_BLE_GATT_PROP_NOTIFY,
		    .initial_value = GATT_REG_NOTIFY_INITIAL,
		    .initial_len   = sizeof(GATT_REG_NOTIFY_INITIAL),
		},
	};
	alp_ble_service_def_t def = {
		.service_uuid = GATT_SVC_UUID,
		.chars        = chars,
		.num_chars    = 2u,
	};
	alp_ble_attr_handle_t handles[2] = { 0u, 0u };
	s                                = alp_ble_gatt_register_service(ble, &def, handles);
	if (s != ALP_OK || handles[0] == 0u || handles[1] == 0u) {
		printf("RESULT FAIL: step 3 alp_ble_gatt_register_service() -> %s "
		       "handles=[0x%04x, 0x%04x]\n",
		       alp_status_name(s),
		       handles[0],
		       handles[1]);
		alp_ble_close(ble);
		return 0;
	}
	printf("[gatt-register] step 3 OK: registered -- handles=[0x%04x, 0x%04x]\n",
	       handles[0],
	       handles[1]);

	/* ---- Step 4: post-register bridge liveness (the UAF-fix proof) ------
	 * The firmware's register handler re-runs ble_gatts_start() (via
	 * ble_gatts_reset()) to rebuild the attribute table.  Before this
	 * branch's fix, a second/duplicate ble_gatts_start() call could leave
	 * a stale pointer live in the NimBLE heap -- with that bug, the NEXT
	 * bridge transaction (this PING) is exactly where it would surface: as
	 * a desync/hang instead of a clean reply.  A live, correct bridge just
	 * answers PING here, same as step 1.  This is peer-free by design --
	 * it doesn't need a central, only a second round-trip after register. */
	if (!gatt_ping(&fw)) {
		printf("RESULT FAIL: step 4 post-register PING never answered -- possible bridge "
		       "desync/hang (the double-ble_gatts_start() UAF this app checks for)\n");
		alp_ble_close(ble);
		return 0;
	}
	printf("[gatt-register] step 4 OK: bridge still alive post-register (UAF-fix check "
	       "passed)\n");

	/* ---- Step 5: start advertising --------------------------------------
	 * Name budget: the CC3501E backend's pack_adv_data() (src/backends/ble/
	 * cc3501e.c) packs Flags (3 bytes: 1 length + 1 type + 1 flags byte),
	 * then the local name (2 + strlen bytes), then any advertised service
	 * UUIDs (2 + 16*count bytes), into a 31-byte legacy adv PDU
	 * (BLE_AD_MAX_LEN).  We advertise our own registered service UUID
	 * alongside an 8-char name: 3 + (2+8) + (2+16) = 31 -- exactly at the
	 * budget, not over it. A longer name (or a 2nd advertised UUID) would
	 * overflow and pack_adv_data() would return ALP_ERR_INVAL. */
	alp_ble_adv_config_t adv = ALP_BLE_ADV_CONFIG_DEFAULT("GattTest");
	adv.services             = &GATT_SVC_UUID;
	adv.num_services         = 1u;
	s                        = alp_ble_advertise_start(ble, &adv);
	if (s != ALP_OK) {
		printf("RESULT FAIL: step 5 alp_ble_advertise_start() -> %s\n", alp_status_name(s));
		alp_ble_close(ble);
		return 0;
	}
	printf("[gatt-register] step 5 OK: advertising as \"%s\"\n", adv.name);

	/* ---- Step 6: register AGAIN while advertising -> must be BUSY -------
	 * NimBLE's ble_gatts_mutable() refuses to rebuild the attribute table
	 * once the stack is advertising/scanning/connected.  On the wire this
	 * shows up as the firmware's cc3501e_hw_ble_gatt_register() call
	 * failing; chips/cc3501e/cc3501e_ble.c's cc3501e_ble_gatt_register()
	 * remaps that specific, persistent failure to ALP_ERR_BUSY (see its
	 * @note) so callers get "stop advertising, then retry" instead of a
	 * bare IO/timeout.  NOTE: poll_by_repeat retries across the FULL
	 * driver op-timeout budget before giving up and returning that mapped
	 * BUSY, so this call takes several seconds (the poll budget) to
	 * return -- it does not fail fast.  A fast-path EBUSY signal instead
	 * of a full-budget retry is a known follow-up, not implemented here. */
	alp_ble_attr_handle_t handles2[2] = { 0u, 0u };
	s                                 = alp_ble_gatt_register_service(ble, &def, handles2);
	if (s != ALP_ERR_BUSY) {
		printf("RESULT FAIL: step 6 register-while-advertising -> %s (expected "
		       "ALP_ERR_BUSY -- the ble_gatts_mutable() ordering guard didn't trip)\n",
		       alp_status_name(s));
		alp_ble_close(ble);
		return 0;
	}
	printf("[gatt-register] step 6 OK: register-while-advertising -> ALP_ERR_BUSY (ordering "
	       "guard held)\n");

	alp_ble_close(ble);
	printf("RESULT PASS: register(handles=[0x%04x,0x%04x]) -> post-register PING alive -> "
	       "advertise -> register-while-advertising -> ALP_ERR_BUSY\n",
	       handles[0],
	       handles[1]);
	return 0;
}
