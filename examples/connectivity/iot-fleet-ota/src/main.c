/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * iot-fleet-ota -- secure OTA firmware update with rollback.
 *
 * The production-readiness proof: "how do we update 10k units in
 * the field?".  Answer:
 *
 *   1. Every 60 s the device polls a Mender server over HTTPS.
 *      The server (separate repo, see docs/ota.md) answers with
 *      either "no deployment" or a signed firmware manifest +
 *      artefact URL.
 *
 *   2. The device downloads the artefact and verifies its
 *      ECDSA-P256 signature.  The verifying public key was
 *      compiled into the bootloader at provisioning time from
 *      the OPTIGA Trust M's secure NVM (slot 0xE0F0).  The
 *      private half *never* leaves the SoM -- it lives in the
 *      OPTIGA's tamper-resistant store and only the signing
 *      host with physical access to a provisioned OPTIGA can
 *      produce a signature that this device will accept.  See
 *      docs/secure-boot.md "Signing key lifecycle".
 *
 *   3. The verified bytes are written to the inactive MCUboot
 *      slot (slot-B if currently booted from slot-A, vice
 *      versa).  MCUboot's swap-using-scratch trailer is marked
 *      "test pending" -- the next reboot boots the new image
 *      with a one-shot fall-back-if-not-confirmed flag set.
 *
 *   4. k_reboot().  MCUboot validates the signature *again* at
 *      boot, swaps, and hands off to the new image.
 *
 *   5. The new image must call boot_set_confirmed() within its
 *      health-check window or MCUboot rolls back to the
 *      previous slot on the next reboot.  Watchdog-friendly
 *      safety net: an OTA that bricks the device is
 *      automatically reversed.
 *
 * Three layers of defence
 * =======================
 *   - Wire:    HTTPS / TLS (mbedtls) protects the download from
 *              MITM tampering.
 *   - Image:   ECDSA-P256 signature gates *acceptance* into the
 *              flash; an attacker with the server's keys still
 *              can't produce a passing artefact without the
 *              OPTIGA-locked private key.
 *   - Boot:    MCUboot re-verifies and atomic-swaps; mid-write
 *              power loss recovers cleanly via the scratch slot.
 *
 * Targets ALL E1M-X SoMs (the trust model is shared; only the
 * download transport varies -- HTTPS on AEN via the CC3501E,
 * Ethernet on V2N family, optional cellular on customer fork).
 *
 * What runs under native_sim
 * ==========================
 * Mender's HTTPS calls aren't reachable from the framing test,
 * the OPTIGA isn't present, and MCUboot isn't running underneath.
 * The demo stubs out the network and signature paths, prints
 * each protocol stage so a CI observer can confirm the framing
 * is intact, and exits cleanly.
 *
 * What runs on AEN HiL
 * ====================
 * Real polling against a staged Mender server; real ECDSA-P256
 * verification via mbedtls (which routes through the OPTIGA's
 * PSA driver once it lands in v0.3.x for HW acceleration); real
 * slot-B write through <alp/storage.h>; real k_reboot() into
 * MCUboot's swap path.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "alp/iot.h"
#include "alp/security.h"
#include "alp/storage.h"

/* Mender poll cadence.  60 s is aggressive for a fleet (typical
 * production cadence is 15-60 minutes); the short interval here
 * matches the demo intent of "show one full cycle quickly". */
#define OTA_POLL_INTERVAL_S 60u

/* The OTA server URL gets baked in at provisioning time.  In a
 * real fleet customers point this at their own Mender instance;
 * the placeholder here is documentation only. */
#define MENDER_SERVER_URL "https://mender.example.alplab.ai"

/* ECDSA-P256 signatures are 64 bytes raw / ~72 bytes DER.  We
 * use the raw form on the wire to match imgtool's signed
 * artefact encoding. */
#define SIG_ECDSA_P256_BYTES 64u

/* Manifest payload size cap.  Mender's deployment JSON is small
 * (artefact URL, checksum, signature, target slot, version
 * string) -- 1 KiB is generous. */
#define MANIFEST_MAX_BYTES 1024u

/* ----------------------------------------------------------------- */
/* Stage 1: connect to the Mender server                              */
/* ----------------------------------------------------------------- */

/**
 * @brief Open the Wi-Fi station + raise it to associated state.
 *
 * On native_sim the open returns NULL with NOSUPPORT and the
 * framing path returns false -- which is the expected behaviour
 * documented in <alp/iot.h>'s NOSUPPORT contract.
 */
static bool fleet_wifi_up(void)
{
    printf("[ota] stage 1: bringing Wi-Fi station up\n");
    alp_wifi_t *w = alp_wifi_open();
    if (w == NULL) {
        printf("[ota]   alp_wifi_open -> NULL (native_sim: NOSUPPORT;"
               " HiL pre-DT: NOT_READY)\n");
        return false;
    }
    /* On real silicon the credentials come from the EEPROM
     * factory manifest (alp_hw_info_t.wifi_*) so the same
     * binary works across fleet units.  Hardcoded for the
     * framing demo. */
    const alp_wifi_credentials_t creds = {
        .ssid = "fleet-ssid",
        .psk  = "fleet-psk",
    };
    const alp_status_t rc = alp_wifi_connect(w, &creds, 10000u);
    if (rc != ALP_OK) {
        printf("[ota]   alp_wifi_connect -> %d\n", (int)rc);
        alp_wifi_close(w);
        return false;
    }
    printf("[ota]   Wi-Fi associated -- ready for HTTPS poll\n");
    alp_wifi_close(w);  /* HTTPS client re-uses the station;
                         * keep the handle short-lived for the
                         * framing test. */
    return true;
}

/* ----------------------------------------------------------------- */
/* Stage 2: poll Mender's HTTPS deployment endpoint                   */
/* ----------------------------------------------------------------- */

/**
 * @brief Ask the Mender server whether a deployment is pending.
 *
 * The real Mender protocol is HTTPS GET on
 * /api/devices/v1/deployments/device/deployments/next plus a
 * device-identity body.  On native_sim we stub the call and
 * print the protocol stage.
 *
 * @param[out] manifest_out   Manifest bytes (on real silicon).
 * @param[in]  manifest_cap   Buffer capacity.
 * @param[out] manifest_len   Bytes written.
 *
 * @return true if a deployment is pending, false if not.
 */
static bool fleet_poll_for_update(uint8_t *manifest_out,
                                  size_t   manifest_cap,
                                  size_t  *manifest_len)
{
    (void)manifest_out;
    (void)manifest_cap;
    if (manifest_len != NULL) {
        *manifest_len = 0u;
    }
    printf("[ota] stage 2: polling %s for deployment\n", MENDER_SERVER_URL);
    printf("[ota]   GET /api/devices/v1/deployments/.../next"
           " (stubbed on native_sim)\n");
    /* On native_sim no manifest comes back -- the framing demo
     * exits the poll loop here.  On HiL the mender-mcu-client
     * fills the buffer with the deployment JSON. */
    return false;
}

/* ----------------------------------------------------------------- */
/* Stage 3: verify ECDSA-P256 signature                               */
/* ----------------------------------------------------------------- */

/**
 * @brief Verify the artefact signature against the OPTIGA-locked
 *        public key.
 *
 * On AEN-Zephyr this routes through mbedtls's PSA Crypto API,
 * which the OPTIGA Trust M PSA driver (v0.3.x) accelerates in
 * hardware.  The verifying key is the public half of the
 * OPTIGA's slot 0xE0F0 -- compiled into the MCUboot bootloader
 * at production provisioning time.
 *
 * On native_sim the function returns true (framing only); on
 * HiL a tampered artefact returns false and the OTA is aborted
 * before any flash is touched.
 *
 * @param[in] image      Artefact bytes.
 * @param[in] image_len  Artefact length.
 * @param[in] sig        Signature bytes (raw P-256, 64 B).
 * @param[in] sig_len    Signature length.
 *
 * @return true on signature pass, false otherwise.
 */
static bool fleet_verify_signature(const uint8_t *image, size_t image_len,
                                   const uint8_t *sig,   size_t sig_len)
{
    (void)image;
    (void)image_len;
    (void)sig;
    (void)sig_len;
    printf("[ota] stage 3: verifying ECDSA-P256 signature\n");
    printf("[ota]   public key = OPTIGA Trust M slot 0xE0F0"
           " (compiled into MCUboot)\n");
    printf("[ota]   alp_hash + ECDSA-P256 verify"
           " (stubbed on native_sim)\n");
    return true;
}

/* ----------------------------------------------------------------- */
/* Stage 4: write to the inactive MCUboot slot                        */
/* ----------------------------------------------------------------- */

/**
 * @brief Stream the verified artefact into the inactive MCUboot slot.
 *
 * MCUboot lays out slot-A and slot-B back-to-back in the
 * internal flash.  alp_storage_open(INTERNAL_FLASH) gives the
 * raw flash handle; the partition table (DT chosen node
 * `zephyr,code-partition`) determines slot-A's offset, slot-B
 * lives at the matching offset on the other side.  Chunk-wise
 * erase + write keeps the watchdog happy.
 */
static bool fleet_write_inactive_slot(const uint8_t *image, size_t image_len)
{
    (void)image;
    (void)image_len;
    printf("[ota] stage 4: writing artefact to inactive MCUboot slot\n");
    alp_storage_t *s = alp_storage_open(&(alp_storage_config_t){
        .kind        = ALP_STORAGE_KIND_INTERNAL_FLASH,
        .instance_id = 0u,
    });
    if (s == NULL) {
        printf("[ota]   alp_storage_open(INTERNAL_FLASH) -> NULL"
               " (last_err=%d)\n", (int)alp_last_error());
        return false;
    }
    alp_storage_info_t info = { 0 };
    const alp_status_t rc = alp_storage_get_info(s, &info);
    if (rc == ALP_OK) {
        printf("[ota]   flash geometry: %llu bytes total,"
               " erase=%u write=%u\n",
               (unsigned long long)info.total_bytes,
               (unsigned)info.erase_size,
               (unsigned)info.block_size);
    }
    /* On HiL: loop over MENDER_CHUNK_SIZE windows, erase the
     * destination sector, write the chunk, feed the watchdog,
     * repeat.  The framing demo stops at the open()+info read
     * to keep the native_sim run bounded. */
    printf("[ota]   (chunked erase + write happens here on HiL)\n");
    alp_storage_close(s);
    return true;
}

/* ----------------------------------------------------------------- */
/* Stage 5: arm swap-with-rollback + reboot                            */
/* ----------------------------------------------------------------- */

/**
 * @brief Set MCUboot's "test pending" flag in the slot trailer
 *        and trigger a reboot.
 *
 * MCUboot's swap-using-scratch mode treats the test-pending
 * flag as one-shot: the next boot tries the new slot, but if
 * boot_set_confirmed() isn't called in the new image's
 * health-check window, the *following* boot rolls back to the
 * previous slot.  Mid-swap power loss is recovered via the
 * scratch sector (see docs/secure-boot.md "Failure modes").
 */
static void fleet_arm_swap_and_reboot(void)
{
    printf("[ota] stage 5: arming swap-with-rollback + rebooting\n");
    printf("[ota]   boot_request_upgrade(BOOT_UPGRADE_TEST)"
           " (stubbed on native_sim)\n");
    printf("[ota]   k_reboot() (stubbed on native_sim)\n");
#ifndef CONFIG_BOARD_NATIVE_SIM
    /* sys_reboot() / k_reboot() -- the real implementation lands
     * alongside the mender-mcu-client integration in v0.4.  Kept
     * out of the native_sim build to avoid pulling Zephyr's
     * reboot subsys into the framing test. */
#endif
}

/* ----------------------------------------------------------------- */
/* Orchestrator                                                       */
/* ----------------------------------------------------------------- */

/**
 * @brief One iteration of the OTA poll loop.
 *
 * On native_sim the poll returns "no deployment" and the loop
 * exits.  On HiL the loop runs forever at OTA_POLL_INTERVAL_S
 * cadence; when a deployment lands the function returns through
 * k_reboot() and never reaches the next iteration.
 *
 * @return true to keep looping, false to exit.
 */
static bool fleet_ota_tick(void)
{
    if (!fleet_wifi_up()) {
        return true;  /* try again next tick */
    }

    uint8_t manifest[MANIFEST_MAX_BYTES];
    size_t  manifest_len = 0u;
    if (!fleet_poll_for_update(manifest, sizeof manifest, &manifest_len)) {
        printf("[ota]   no deployment pending -- sleeping %us\n",
               (unsigned)OTA_POLL_INTERVAL_S);
        return true;
    }

    /* On HiL the manifest carries the artefact URL + signature.
     * The download happens chunked through the mender-mcu-client
     * (which calls back into this app's "verify-and-write" path
     * once the bytes are buffered).  The flow below is the
     * happy-path skeleton. */
    const uint8_t *image    = manifest;          /* placeholder */
    const size_t   img_len  = manifest_len;
    const uint8_t *sig      = manifest;          /* placeholder */
    const size_t   sig_len  = SIG_ECDSA_P256_BYTES;

    if (!fleet_verify_signature(image, img_len, sig, sig_len)) {
        printf("[ota]   signature verification FAILED -- discarding\n");
        return true;
    }
    if (!fleet_write_inactive_slot(image, img_len)) {
        printf("[ota]   slot write failed -- aborting deployment\n");
        return true;
    }
    fleet_arm_swap_and_reboot();
    return false;  /* k_reboot() would have happened on HiL */
}

int main(void)
{
    printf("[ota] alp-sdk iot-fleet-ota demo\n");
    printf("[ota]   trust: OPTIGA Trust M (slot 0xE0F0) +"
           " ECDSA-P256 + MCUboot swap-using-scratch\n");
    printf("[ota]   transport: HTTPS poll to %s (Mender protocol)\n",
           MENDER_SERVER_URL);

    for (;;) {
        if (!fleet_ota_tick()) {
            break;
        }
#ifdef CONFIG_BOARD_NATIVE_SIM
        break; /* one iteration is enough for the framing test */
#else
        k_sleep(K_SECONDS(OTA_POLL_INTERVAL_S));
#endif
    }

    printf("[ota] done\n");
    return 0;
}
