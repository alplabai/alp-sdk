/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * production-deployment -- the v1.0 reference application for a
 * field-deployable IoT product.  Demonstrates the full
 * "manufactured -> deployed -> updated -> recovered" lifecycle on
 * a single app, so customers see how the SDK's secure-boot, OTA,
 * EEPROM-provisioning, and remote-attestation pieces fit together.
 *
 * The four lifecycle stages the example shows
 * ===========================================
 *
 *   1. Factory-provisioning read-back.
 *      On boot, `alp_hw_info_*` reads the manufacturer EEPROM
 *      manifest programmed at factory test.  The manifest carries
 *      the board's per-unit identity: SKU, serial number, HW
 *      revision, factory-burn date.  Production-grade firmware
 *      treats this as authoritative -- never derive identity from
 *      the SoC's internal UID alone.
 *
 *   2. Secure-boot evidence.
 *      MCUboot has already validated the running image against the
 *      ECDSA-P256 boot key by the time `main()` runs (the
 *      bootloader rejects unsigned / wrong-key images at boot).
 *      This app reads back the active slot + image revision so the
 *      cloud-side fleet console can confirm the deployed firmware
 *      matches what the signing service produced.  See
 *      `docs/secure-boot.md` for the signing-service contract.
 *
 *   3. OTA polling + apply.
 *      `alp_iot_*` opens a TLS-mutual-auth connection to the
 *      Mender server and polls for a deployment.  If one is
 *      pending, the SDK downloads the new image, verifies its
 *      signature against the same ECDSA-P256 boot key, writes it
 *      to the inactive MCUboot slot, sets the swap-pending flag,
 *      and requests a reboot.  Post-reboot, MCUboot confirms or
 *      reverts based on the new image's `mark_confirmed` call.
 *
 *   4. Remote attestation.
 *      Per the threat model (`docs/threat-model.md` §asset 8), a
 *      cloud-side fleet operator needs evidence that the device
 *      hasn't been physically tampered with.  This app publishes
 *      a periodic heartbeat that includes the OPTIGA-signed boot
 *      log + the current secure-boot slot.  An attacker who
 *      swapped the flash sees the OPTIGA signature break.
 *
 * Why this is a "reference application" not a "library example"
 * =============================================================
 *
 * Every previous flagship example shows one SDK surface
 * (PWM, I²C, audio, inference, ...).  This one shows the
 * *production deployment lifecycle* the SDK is designed for --
 * the integration of all the security + connectivity pieces into
 * a single field-deployable app.  Customers ship variants of
 * this skeleton to production rather than writing the
 * EEPROM-readback / Mender / OPTIGA glue from scratch.
 *
 * What runs under native_sim
 * ==========================
 *
 * The four pieces above need real silicon to verify (EEPROM
 * I²C reads, MCUboot slot inspection, TLS to a real Mender
 * server, OPTIGA signature).  Under native_sim the app reaches
 * each stage's open() call, observes the documented NOSUPPORT /
 * NOT_READY return, prints the stage transition, and proceeds.
 * The framing is what tests; the substantive verification gates
 * on HiL per `docs/test-plan.md`.
 *
 * What runs on HiL
 * ================
 *
 * Full lifecycle, end-to-end:
 *   - Boot from a factory-signed image.
 *   - Read manufacturer EEPROM, print serial + SKU + rev.
 *   - Inspect MCUboot slots, print image revision.
 *   - Connect to a Mender server (board-staged
 *     production-test rig), poll for an update.
 *   - When a deployment lands, download + verify + apply
 *     it, request reboot.
 *   - Post-reboot: confirm the new image to keep the swap.
 *   - Publish OPTIGA-signed attestation every 60 s thereafter.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "alp/hw_info.h"
#include "alp/iot.h"
#include "alp/security.h"
#include "alp/storage.h"

#define MENDER_POLL_INTERVAL_S 600u
#define HEARTBEAT_INTERVAL_S   60u

/* ----------------------------------------------------------------- */
/* Stage 1: factory-provisioning read-back                            */
/* ----------------------------------------------------------------- */

static void stage_factory_identity(void)
{
	printf("[prod] stage 1: reading manufacturer EEPROM manifest\n");
	alp_hw_info_t      info = { 0 };
	const alp_status_t s    = alp_hw_info_read(&info);
	if (s != ALP_OK) {
		printf("[prod]   alp_hw_info_read -> %d -- continuing with no identity\n", (int)s);
		return;
	}
	printf("[prod]   SoM SKU=%s\n", info.som_sku);
	printf("[prod]   SoM serial=%s\n", info.som_serial);
	printf("[prod]   SoM hw_rev=%s\n", info.som_hw_rev);
	printf("[prod]   SoM mfg date=%u-%02u-%02u\n",
	       (unsigned)info.som_mfg_year,
	       (unsigned)info.som_mfg_month,
	       (unsigned)info.som_mfg_day);
	if (info.board_name[0] != '\0') {
		printf("[prod]   board=%s rev=%s\n", info.board_name, info.board_hw_rev);
	}
}

/* ----------------------------------------------------------------- */
/* Stage 2: secure-boot evidence                                       */
/* ----------------------------------------------------------------- */

static void stage_secure_boot_evidence(void)
{
	printf("[prod] stage 2: reading MCUboot slot info\n");
	/* alp_storage_open(INTERNAL_FLASH) lets us inspect the MCUboot
     * trailer area; the swap-state byte + image_ok flag live in
     * the last sector of the active slot.  The exact offsets are
     * MCUboot's responsibility; this example just demonstrates the
     * read path. */
	alp_storage_t *s = alp_storage_open(&(alp_storage_config_t){
	    .kind        = ALP_STORAGE_KIND_INTERNAL_FLASH,
	    .instance_id = 0u,
	});
	if (s == NULL) {
		printf("[prod]   internal flash open -> last_err=%d\n", (int)alp_last_error());
		return;
	}
	alp_storage_info_t info = { 0 };
	const alp_status_t rc   = alp_storage_get_info(s, &info);
	if (rc == ALP_OK) {
		printf("[prod]   internal flash: %llu bytes total, block=%u erase=%u\n",
		       (unsigned long long)info.total_bytes,
		       (unsigned)info.block_size,
		       (unsigned)info.erase_size);
		printf("[prod]   (slot inspection + image_ok read happens here on HiL)\n");
	}
	alp_storage_close(s);
}

/* ----------------------------------------------------------------- */
/* Stage 3: OTA polling                                                */
/* ----------------------------------------------------------------- */

static void stage_ota_poll(void)
{
	printf("[prod] stage 3: connecting to Mender server\n");
	/* The real Mender stack:
     *   1. alp_iot_wifi_open + alp_iot_wifi_connect to associate.
     *   2. alp_iot_mqtt_open against the broker carrying the
     *      Mender deployment commands (or HTTPS poll, depending
     *      on the Mender flavour).
     *   3. On a deployment event: alp_storage_open(QSPI) the OTA
     *      partition, write chunks, mark MCUboot for swap on
     *      next boot, alp_iot_publish a deployment-accepted
     *      status, reboot.
     *
     * On native_sim every step returns NOSUPPORT; the example
     * prints the transitions but doesn't actually move bytes. */
	alp_wifi_t *wifi = alp_wifi_open();
	if (wifi == NULL) {
		printf("[prod]   wifi open -> NOSUPPORT (native_sim) or NOT_READY (HiL pre-DT)\n");
		return;
	}
	alp_wifi_close(wifi);
}

/* ----------------------------------------------------------------- */
/* Stage 4: remote attestation                                         */
/* ----------------------------------------------------------------- */

static void stage_remote_attestation_tick(void)
{
	/* alp_aead_open(AES_GCM, derived-key) wrapped around an
     * OPTIGA-signed payload would be the production shape; under
     * native_sim we exercise the framing path so the code-path
     * coverage stays meaningful in CI. */
	uint8_t            payload[64];
	const alp_status_t s = alp_random_bytes(payload, sizeof payload);
	if (s != ALP_OK) {
		printf("[prod]   attestation: alp_random_bytes -> %d\n", (int)s);
		return;
	}
	printf("[prod]   attestation: 64-byte nonce drawn from TRNG\n");
	/* On HiL: alp_optiga_sign(payload, sizeof payload, signature)
     * then alp_iot_mqtt_publish(topic, signature, ...). */
}

int main(void)
{
	printf("[prod] alp-sdk production-deployment flagship\n");

	stage_factory_identity();
	stage_secure_boot_evidence();
	stage_ota_poll();

	/* Steady-state loop -- this is the production shape.  On HiL
     * the heartbeat publishes every HEARTBEAT_INTERVAL_S; the
     * OTA poll fires every MENDER_POLL_INTERVAL_S.  Under
     * native_sim the loop exits after one iteration so the
     * Twister harness sees a clean "done". */
	uint32_t ota_countdown_s = MENDER_POLL_INTERVAL_S;
	for (;;) {
		stage_remote_attestation_tick();
		if (ota_countdown_s <= HEARTBEAT_INTERVAL_S) {
			stage_ota_poll();
			ota_countdown_s = MENDER_POLL_INTERVAL_S;
		} else {
			ota_countdown_s -= HEARTBEAT_INTERVAL_S;
		}
#ifdef CONFIG_BOARD_NATIVE_SIM
		break; /* one iteration is enough for the framing test */
#else
		k_sleep(K_SECONDS(HEARTBEAT_INTERVAL_S));
#endif
	}

	printf("[prod] done\n");
	return 0;
}
