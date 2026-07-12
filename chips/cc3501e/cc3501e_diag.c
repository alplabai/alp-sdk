/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * CC3501E meta + diagnostics host helpers (0x00 / 0x02 / 0x04 / 0x70 / 0x71).
 * See <alp/chips/cc3501e/diag.h> for the public API.
 *
 * All backend-independent: the firmware answers these from in-RAM
 * bookkeeping (no radio op), so they take the caller's short request
 * budget with no radio-down-window floor.  Reply layouts are decoded
 * field-by-field from <alp/protocol/cc3501e.h>.
 */

#include <stdint.h>

#include "cc3501e_internal.h"

alp_status_t cc3501e_ping(cc3501e_t *ctx)
{
	/* PING (0x00): header-only request, no reply data -- the OK status IS the
	 * liveness proof.  Distinct from cc3501e_get_version (which round-trips the
	 * protocol version): PING is the cheapest is-the-firmware-alive probe. */
	return cc3501e_request(ctx, ALP_CC3501E_CMD_PING, NULL, 0, NULL, 0, NULL, CC3501E_REQ_TMO_MS);
}

alp_status_t cc3501e_soft_reset(cc3501e_t *ctx)
{
	/* RESET (0x02): the firmware ACKs, then performs a DEFERRED reboot (it lets
	 * the reply drain before resetting).  Unlike cc3501e_reset / cc3501e_hard_reset
	 * -- which pulse the WIFI_EN / nRESET GPIOs -- this is an in-band firmware-side
	 * reboot needing no host pins.  The link drops after the ack; the caller
	 * re-syncs (cc3501e_sync) + re-pings once the firmware is back up. */
	return cc3501e_request(ctx, ALP_CC3501E_CMD_RESET, NULL, 0, NULL, 0, NULL, CC3501E_REQ_TMO_MS);
}

alp_status_t cc3501e_diag_info(cc3501e_t *ctx, alp_cc3501e_diag_info_t *out)
{
	if (out == NULL) return ALP_ERR_INVAL;
	/* GET_DIAG_INFO (0x04) reply = the 16-byte packed alp_cc3501e_diag_info_t:
	 * fw_version(LE16) | reset_cause(1) | role(1) | uptime_ms(LE32) |
	 * free_heap_bytes(LE32) | last_error(1) | reserved(3). */
	uint8_t      reply[16] = { 0 };
	size_t       got       = 0;
	alp_status_t s         = cc3501e_request(ctx,
	                                         ALP_CC3501E_CMD_GET_DIAG_INFO,
	                                         NULL,
	                                         0,
	                                         reply,
	                                         sizeof(reply),
	                                         &got,
	                                         CC3501E_REQ_TMO_MS);
	if (s != ALP_OK) return s;
	if (got < sizeof(reply)) return ALP_ERR_IO;
	out->fw_version  = (uint16_t)reply[0] | ((uint16_t)reply[1] << 8);
	out->reset_cause = reply[2];
	out->role        = reply[3];
	out->uptime_ms   = (uint32_t)reply[4] | ((uint32_t)reply[5] << 8) | ((uint32_t)reply[6] << 16) |
	                   ((uint32_t)reply[7] << 24);
	out->free_heap_bytes = (uint32_t)reply[8] | ((uint32_t)reply[9] << 8) |
	                       ((uint32_t)reply[10] << 16) | ((uint32_t)reply[11] << 24);
	out->last_error      = reply[12];
	out->reserved[0]     = reply[13];
	out->reserved[1]     = reply[14];
	out->reserved[2]     = reply[15];
	return ALP_OK;
}

alp_status_t cc3501e_diag_stats(cc3501e_t *ctx, uint32_t *frames_ok, uint32_t *frames_err)
{
	if (frames_ok == NULL || frames_err == NULL) return ALP_ERR_INVAL;
	/* DIAG_GET_STATS (0x70) reply = frames_ok(LE32) | frames_err(LE32).  The
	 * protocol header carries the opcode but NO reply struct for these two frame
	 * counters, so they are returned via out-params rather than a typedef. */
	uint8_t      reply[8] = { 0 };
	size_t       got      = 0;
	alp_status_t s        = cc3501e_request(ctx,
	                                        ALP_CC3501E_CMD_DIAG_GET_STATS,
	                                        NULL,
	                                        0,
	                                        reply,
	                                        sizeof(reply),
	                                        &got,
	                                        CC3501E_REQ_TMO_MS);
	if (s != ALP_OK) return s;
	if (got < sizeof(reply)) return ALP_ERR_IO;
	*frames_ok  = (uint32_t)reply[0] | ((uint32_t)reply[1] << 8) | ((uint32_t)reply[2] << 16) |
	              ((uint32_t)reply[3] << 24);
	*frames_err = (uint32_t)reply[4] | ((uint32_t)reply[5] << 8) | ((uint32_t)reply[6] << 16) |
	              ((uint32_t)reply[7] << 24);
	return ALP_OK;
}

alp_status_t cc3501e_diag_log_level(cc3501e_t *ctx, uint8_t level)
{
	/* DIAG_LOG_LEVEL (0x71): request payload = level(1); no reply data. */
	uint8_t payload[1] = { level };
	return cc3501e_request(ctx,
	                       ALP_CC3501E_CMD_DIAG_LOG_LEVEL,
	                       payload,
	                       sizeof(payload),
	                       NULL,
	                       0,
	                       NULL,
	                       CC3501E_REQ_TMO_MS);
}
