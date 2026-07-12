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
 * @param timeout_ms  Per-frame budget for each BEGIN / WRITE / FINISH request.
 * @return ALP_OK once FINISH is acked (the device reboots afterwards);
 *         otherwise the first failing step's status (caller may
 *         cc3501e_ota_abort() to reset the device session).
 */
alp_status_t
cc3501e_ota_update(cc3501e_t *ctx, const uint8_t *image, size_t len, uint32_t timeout_ms);

/* Granular OTA controls (cc3501e_ota_update wraps these for the common path). */

/**
 * @brief Open an OTA session (OTA_BEGIN, opcode 0x40).
 *
 * Declares the full image size up front; the device picks its NON-primary
 * vendor slot and brings it to READY (PSA-FWU), arming the session's write
 * cursor at offset 0.
 *
 * @param ctx         Initialised bridge handle.
 * @param total_len   Full signed GPE vendor-image size in bytes (manifest +
 *                    body) that the session will stream.
 * @param timeout_ms  Per-request poll-by-repeat budget.
 * @return ALP_OK once the session is open; otherwise the mapped error (e.g.
 *         ALP_ERR_BUSY if a session is already in flight).
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
