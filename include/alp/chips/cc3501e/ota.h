/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ota.h
 * @brief CC3501E OTA firmware update -- stream a new image over the bridge.
 *
 * The Alif host obtains a signed GPE-format vendor image (via the
 * device-side Mender contract; the OTA server is a separate repo) and
 * streams it into the CC3501E's non-primary vendor slot, which the
 * CC35 then installs + swaps on reboot (PSA-FWU).  See
 * docs/cc3501e-bridge.md "OTA".
 */

#ifndef ALP_CHIPS_CC3501E_OTA_H
#define ALP_CHIPS_CC3501E_OTA_H

#include <stdint.h>
#include <stddef.h>

#include "alp/chips/cc3501e/core.h"
#include "alp/protocol/cc3501e.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Push a complete signed CC3501E vendor image over the bridge + install.
 *
 * Runs the full cycle: OTA_BEGIN(len) -> chunked OTA_WRITE -> OTA_FINISH.  On
 * success the CC3501E has staged the image into its non-primary vendor slot and
 * reboots so BL2 swaps it to primary (TRIAL), after which it self-accepts.  THE
 * BRIDGE LINK DROPS during that reboot: expect the link to go quiet, then
 * re-establish (cc3501e_reset / the soak) and confirm the new GET_VERSION.
 *
 * Recovers from a missed per-chunk reply by re-syncing to the device's actual
 * write cursor (CMD_OTA_STATUS) rather than blindly re-sending (OTA_WRITE is
 * not idempotent -- a re-sent already-written offset is rejected).
 *
 * @param ctx         Initialised bridge handle.
 * @param image       Signed GPE-format vendor image (manifest + body).
 * @param len         Image length in bytes (must exceed the manifest).
 * @param timeout_ms  Per-frame budget for each BEGIN / WRITE / FINISH request --
 *                    AND the whole-operation budget for the update-mode entry
 *                    this call now performs first (@ref cc3501e_ota_update_mode).
 *                    Roughly a sixth of it is spent polling for the reboot, after
 *                    a fixed ~3.5 s settle, so a per-frame value under ~5 s buys
 *                    only one or two confirm polls.  The bench uses 20000.
 * @return ALP_OK once FINISH is acked (the device reboots afterwards);
 *         otherwise the first failing step's status (caller may
 *         cc3501e_ota_abort() to reset the device session).
 */
alp_status_t
cc3501e_ota_update(cc3501e_t *ctx, const uint8_t *image, size_t len, uint32_t timeout_ms);

/**
 * @brief Put the device into (or take it out of) OTA update mode.
 *
 * The device persists a flag and WARM-REBOOTS.  On that boot it never opens the
 * bridge SPI in DMA/callback mode -- which is the only state in which
 * psa_fwu_start() and psa_fwu_write() return at all (bench-proven, silicon
 * 2026-08-21) -- and runs a dedicated loop that does nothing but service the
 * polled bridge frame-by-frame and pump the OTA flush at frame boundaries.
 *
 * Call this BEFORE @ref cc3501e_ota_begin.  The OTA session is RAM-only, so
 * entering mid-session throws away the write cursor and forces a full re-BEGIN
 * (another slot erase, 22-181 s).  After a SUCCESSFUL @ref cc3501e_ota_finish the
 * device leaves update mode BY ITSELF -- do not call this with @p enable false
 * after one of those.  A FAILED finish (or any abandoned session) is the
 * opposite case: the device stays parked in the radio-dead polled boot, so you
 * MUST take it back out with @p enable false or every later Wi-Fi/BLE/GET_MAC
 * call queues forever and answers BUSY forever.  @ref cc3501e_ota_update does
 * both for you -- the entry, and the exit on every failure path.
 *
 * The host drives NO pin here -- entry is device-initiated.  Recovery when the
 * confirm poll exhausts its budget is @ref cc3501e_hard_reset (NOT
 * @ref cc3501e_reset -- the cold cycle re-triggers the Puya double-boot bug and can
 * leave ctx NOT_READY); a reset of either kind always lands in the NORMAL mode.
 *
 * In update mode only PING / OTA_* / GET_DIAG_INFO / RESET are serviced.  Wi-Fi,
 * BLE and GET_MAC queue forever and answer BUSY forever, because nothing drains
 * the worker on that boot.
 *
 * @param ctx         Initialised bridge handle.
 * @param enable      true to enter update mode; false to return to the normal bridge.
 * @param timeout_ms  Whole-operation budget for the reboot + confirm readback.
 * @retval ALP_OK           the device is confirmed running in the requested mode
 *                          (or was already in it, in which case it did NOT reboot).
 * @retval ALP_ERR_NOT_READY  @p ctx is NULL or not initialised.
 * @retval ALP_ERR_TIMEOUT  it never came back in the requested mode; the device
 *                          has been hard-reset.
 */
alp_status_t cc3501e_ota_update_mode(cc3501e_t *ctx, bool enable, uint32_t timeout_ms);

/* Granular OTA controls (cc3501e_ota_update wraps these for the common path). */

/**
 * @brief Open an OTA session (OTA_BEGIN, opcode 0x40).
 *
 * Declares the full image size up front; the device picks its NON-primary
 * vendor slot and brings it to READY (PSA-FWU), arming the session's write
 * cursor at offset 0.
 *
 * PRECONDITION: the device MUST already be in update mode (@ref
 * cc3501e_ota_update_mode with @p enable true).  Opening a session runs
 * psa_fwu_query and, on a dirty slot, a full slot erase -- and those calls never
 * return while the bridge is open in callback/DMA mode, so a BEGIN on the normal
 * bridge does not fail, it WEDGES the device until a WIFI_EN/nRESET cold cycle.
 * This function refuses with @ref ALP_ERR_NOT_READY rather than let that happen.
 * @ref cc3501e_ota_update handles the entry for you.
 *
 * A second BEGIN on a session that is already WRITING is accepted only when it
 * declares the SAME @p total_len and nothing has been written yet; anything else
 * is a different image and is rejected, because merging it would splice the two.
 * Abort first, then begin again.
 *
 * @param ctx         Initialised bridge handle.
 * @param total_len   Full signed GPE vendor-image size in bytes (manifest +
 *                    body) that the session will stream.
 * @param timeout_ms  Per-request poll-by-repeat budget.
 * @retval ALP_OK           the session is open.
 * @retval ALP_ERR_NOT_READY  the device is not in update mode (see above).
 * @retval ALP_ERR_BUSY     a session is already in flight.
 */
alp_status_t cc3501e_ota_begin(cc3501e_t *ctx, uint32_t total_len, uint32_t timeout_ms);

/**
 * @brief Stream one SEQUENTIAL image chunk (OTA_WRITE, opcode 0x41).
 *
 * Preconditions (both enforced): @p offset MUST equal the device's running
 * write cursor -- out-of-order writes are rejected by the firmware -- and
 * @p len MUST be 1..ALP_CC3501E_OTA_MAX_CHUNK bytes (the wire frame is a
 * 4-byte LE offset + the raw bytes, bounded by ALP_CC3501E_MAX_PAYLOAD).
 * After a missed reply, re-sync to the device's actual cursor with
 * @ref cc3501e_ota_status instead of blindly re-sending.
 *
 * @param ctx         Initialised bridge handle.
 * @param offset      Absolute byte offset into the image; must equal the
 *                    device's write cursor (bytes_written so far).
 * @param data        Chunk bytes to append (must be non-NULL).
 * @param len         Chunk length: 1..ALP_CC3501E_OTA_MAX_CHUNK.
 * @param timeout_ms  Per-request poll-by-repeat budget.
 * @return ALP_OK once the chunk is accepted; ALP_ERR_INVAL on a NULL @p data
 *         or an out-of-range @p len; otherwise the mapped error (a
 *         cursor-mismatched offset surfaces as the firmware's INVALID).
 */
alp_status_t cc3501e_ota_write(cc3501e_t     *ctx,
                               uint32_t       offset,
                               const uint8_t *data,
                               size_t         len,
                               uint32_t       timeout_ms);

/**
 * @brief Finalize the session (OTA_FINISH, opcode 0x42).
 *
 * The device installs the fully-streamed image into its non-primary vendor
 * slot and arms the deferred swap reboot (the bridge link drops while the
 * device reboots and BL2/MCUboot swaps the slot to primary).
 *
 * @param ctx         Initialised bridge handle.
 * @param timeout_ms  Per-request poll-by-repeat budget.
 * @return ALP_OK once FINISH is acked (reboot follows); otherwise the mapped
 *         error (e.g. an incomplete stream is rejected).
 */
alp_status_t cc3501e_ota_finish(cc3501e_t *ctx, uint32_t timeout_ms);

/**
 * @brief Cancel an in-flight OTA session (OTA_ABORT, opcode 0x43).
 *
 * Resets the device-side session back to IDLE, discarding streamed bytes.
 *
 * @param ctx         Initialised bridge handle.
 * @param timeout_ms  Per-request poll-by-repeat budget.
 * @return ALP_OK once the session is cancelled; otherwise the mapped error.
 */
alp_status_t cc3501e_ota_abort(cc3501e_t *ctx, uint32_t timeout_ms);

/**
 * @brief Promote an already-committed pending image (OTA_PROMOTE, opcode 0x46).
 *
 * Requests the deferred swap-reboot for an image already installed to STAGED --
 * for example one left pending by a bare reset that carried no swap request. A
 * committed STAGED image survives a reset while the device's RAM session state
 * resets to IDLE, so a fresh @ref cc3501e_ota_finish is unreachable (a new
 * session is rejected while a slot is occupied); this is the only path to
 * request the swap for such an image. The bridge link drops while the device
 * reboots and BL2/MCUboot swaps the pending slot to primary. If nothing is
 * pending the reboot is a clean no-op.
 *
 * @param ctx         Initialised bridge handle.
 * @param timeout_ms  Per-request poll-by-repeat budget.
 * @return ALP_OK once the promote is acked (reboot follows); otherwise the
 *         mapped error (e.g. ALP_ERR_NOT_READY on a non-OTA firmware build).
 */
alp_status_t cc3501e_ota_promote(cc3501e_t *ctx, uint32_t timeout_ms);

/**
 * @brief Query the device-side OTA session state (OTA_STATUS, opcode 0x44).
 *
 * Fills @p out with the session state, the bytes accepted so far (the write
 * cursor a resuming host must continue from), and the total declared at
 * BEGIN.  Read-only: safe to call at any point in the session.
 *
 * @param ctx         Initialised bridge handle.
 * @param out         Receives the @ref alp_cc3501e_ota_status_t snapshot.
 * @param timeout_ms  Per-request poll-by-repeat budget.
 * @return ALP_OK with @p out filled; ALP_ERR_INVAL on a NULL @p out;
 *         ALP_ERR_IO on a short reply; otherwise the mapped error.
 */
alp_status_t cc3501e_ota_status(cc3501e_t *ctx, alp_cc3501e_ota_status_t *out, uint32_t timeout_ms);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_CC3501E_OTA_H */
