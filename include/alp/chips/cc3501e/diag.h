/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file diag.h
 * @brief CC3501E meta + diagnostics host helpers (backend-independent).
 *
 * Thin wrappers over @ref cc3501e_request for the meta (0x00/0x02/0x04)
 * and diagnostics (0x70/0x71) opcodes.  The firmware answers these from
 * in-RAM bookkeeping (no radio op), so they use a short request budget
 * and never poll-by-repeat.
 */

#ifndef ALP_CHIPS_CC3501E_DIAG_H
#define ALP_CHIPS_CC3501E_DIAG_H

#include <stdint.h>

#include "alp/chips/cc3501e/core.h"
#include "alp/protocol/cc3501e.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Liveness probe: PING the CC3501E firmware (PING, opcode 0x00).
 *
 * Sends a header-only PING and treats the firmware's OK status as proof the
 * link + firmware are alive.  Cheaper + more direct than @ref cc3501e_get_version
 * (which round-trips the protocol version); use this when only "is it alive?"
 * matters.
 *
 * @param ctx  Initialised driver context.
 * @return ALP_OK on a successful round-trip; ALP_ERR_IO on a desync / no reply;
 *         ALP_ERR_NOT_READY if @p ctx is not initialised.
 */
alp_status_t cc3501e_ping(cc3501e_t *ctx);

/**
 * @brief Request an in-band firmware soft reset (RESET, opcode 0x02).
 *
 * The firmware ACKs the command and then performs a DEFERRED reboot (it drains
 * the reply first).  Unlike @ref cc3501e_reset / @ref cc3501e_hard_reset -- which
 * pulse the WIFI_EN / nRESET GPIO lines -- this needs no host pins; it is a
 * firmware-side reboot over the wire.  The bridge link drops after the ack, so
 * re-establish it with @ref cc3501e_sync (and confirm with @ref cc3501e_ping)
 * once the firmware is back up.
 *
 * @param ctx  Initialised driver context.
 * @return ALP_OK once the reset is acked (reboot follows); otherwise the mapped
 *         error.
 */
alp_status_t cc3501e_soft_reset(cc3501e_t *ctx);

/**
 * @brief Read extended firmware diagnostics (GET_DIAG_INFO, opcode 0x04).
 *
 * Decodes the 16-byte @ref alp_cc3501e_diag_info_t reply field-by-field: the
 * firmware release @c fw_version (distinct from the protocol version that
 * @ref cc3501e_get_version returns), the @c reset_cause
 * (@ref alp_cc3501e_reset_cause_t), the active @c role (@ref alp_cc3501e_role_t),
 * @c uptime_ms, @c free_heap_bytes, and the @c last_error the firmware last
 * emitted (@ref alp_cc3501e_resp_t).  Non-disturbing: no side effects on radio
 * state.  v2-firmware-only -- v1 firmware rejects with RESP_ERR_INVALID.
 *
 * @param ctx  Initialised driver context.
 * @param out  Receives the decoded diagnostics snapshot on success.
 * @return ALP_OK with @p out filled; ALP_ERR_INVAL on a NULL @p out;
 *         ALP_ERR_IO on a short reply; otherwise the mapped error.
 */
alp_status_t cc3501e_diag_info(cc3501e_t *ctx, alp_cc3501e_diag_info_t *out);

/**
 * @brief Read the firmware frame counters (DIAG_GET_STATS, opcode 0x70).
 *
 * The reply data is two little-endian 32-bit counters: frames the firmware
 * answered OK, and frames it answered with a non-OK status.  The protocol
 * header defines the opcode but NO reply struct for these, so they come back
 * through out-params rather than a typedef.
 *
 * @param ctx         Initialised driver context.
 * @param frames_ok   Receives the count of OK-answered frames.
 * @param frames_err  Receives the count of error-answered frames.
 * @return ALP_OK with both counters filled; ALP_ERR_INVAL if either out-param
 *         is NULL; ALP_ERR_IO on a short reply; otherwise the mapped error.
 */
alp_status_t cc3501e_diag_stats(cc3501e_t *ctx, uint32_t *frames_ok, uint32_t *frames_err);

/**
 * @brief Set the firmware log verbosity (DIAG_LOG_LEVEL, opcode 0x71).
 *
 * The request payload is a single log-level byte; higher values are more
 * verbose (the firmware clamps to its supported range).  No reply data.
 *
 * @param ctx    Initialised driver context.
 * @param level  Firmware log level to apply.
 * @return ALP_OK once applied; otherwise the mapped error.
 */
alp_status_t cc3501e_diag_log_level(cc3501e_t *ctx, uint8_t level);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_CC3501E_DIAG_H */
