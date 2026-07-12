/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: OTA firmware-update command-family handlers
 * (over-the-bridge PSA-FWU streaming, 0x40..0x46) -- v0.2.  Split out
 * of protocol.c (issue #461); protocol_dispatch() in protocol.c still
 * owns the single command-family switch that routes here.
 */

#include "protocol_internal.h"

/* OTA_BEGIN (0x40): req = alp_cc3501e_ota_begin_t { total_len LE32 }.  Opens
 * the streaming session; the HAL picks the non-primary vendor slot. */
alp_cc3501e_resp_t handle_ota_begin(const uint8_t *req,
                                    size_t         req_len,
                                    uint8_t       *reply_data,
                                    size_t         reply_cap,
                                    size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != 4u) return ALP_CC3501E_RESP_ERR_INVALID;
	return hw_to_resp(cc3501e_hw_ota_begin(get_le32(&req[0])));
}

/* OTA_WRITE (0x41): req = offset(LE32) followed by 1..OTA_MAX_CHUNK image
 * bytes.  Streams the chunk into the slot (sequential offsets enforced). */
alp_cc3501e_resp_t handle_ota_write(const uint8_t *req,
                                    size_t         req_len,
                                    uint8_t       *reply_data,
                                    size_t         reply_cap,
                                    size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len < 5u || req_len > (size_t)(4u + ALP_CC3501E_OTA_MAX_CHUNK)) {
		return ALP_CC3501E_RESP_ERR_INVALID;
	}
	const uint32_t offset = get_le32(&req[0]);
	return hw_to_resp(cc3501e_hw_ota_write(offset, &req[4], (uint32_t)(req_len - 4u)));
}

/* OTA_FINISH (0x42): no payload.  Installs + arms the deferred swap reboot. */
alp_cc3501e_resp_t handle_ota_finish(const uint8_t *req,
                                     size_t         req_len,
                                     uint8_t       *reply_data,
                                     size_t         reply_cap,
                                     size_t        *reply_data_len)
{
	(void)req;
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	return hw_to_resp(cc3501e_hw_ota_finish());
}

/* OTA_ABORT (0x43): no payload.  Cancels the in-flight session. */
alp_cc3501e_resp_t handle_ota_abort(const uint8_t *req,
                                    size_t         req_len,
                                    uint8_t       *reply_data,
                                    size_t         reply_cap,
                                    size_t        *reply_data_len)
{
	(void)req;
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	return hw_to_resp(cc3501e_hw_ota_abort());
}

/* OTA_PROMOTE (0x46): no payload.  Requests the swap-reboot for an image already
 * committed to STAGED -- unjams a slot left pending by a bare reset (which
 * carried no swap request), which FINISH can no longer re-reach. */
alp_cc3501e_resp_t handle_ota_promote(const uint8_t *req,
                                      size_t         req_len,
                                      uint8_t       *reply_data,
                                      size_t         reply_cap,
                                      size_t        *reply_data_len)
{
	(void)req;
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	return hw_to_resp(cc3501e_hw_ota_promote());
}

/* OTA_STATUS (0x44): reply = alp_cc3501e_ota_status_t
 * { state(1) | reserved(3) | bytes_written(LE32) | total_len(LE32) }. */
alp_cc3501e_resp_t handle_ota_status(const uint8_t *req,
                                     size_t         req_len,
                                     uint8_t       *reply_data,
                                     size_t         reply_cap,
                                     size_t        *reply_data_len)
{
	(void)req;
	*reply_data_len = 0u;
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	if (reply_cap < 12u) return ALP_CC3501E_RESP_ERR_NO_MEM;
	uint8_t   state   = 0u;
	uint32_t  written = 0u;
	uint32_t  total   = 0u;
	const int rv      = cc3501e_hw_ota_status(&state, &written, &total);
	if (rv != CC3501E_HW_OK) return hw_to_resp(rv);
	reply_data[0] = state;
	/* reserved[0] carries the last swap-reboot rc (0 = none / success; non-zero =
	 * the swap was refused, e.g. BL2 anti-rollback on a downgrade). */
	reply_data[1] = (uint8_t)cc3501e_hw_ota_reboot_rc();
	reply_data[2] = 0u;
	reply_data[3] = 0u;
	put_le32(&reply_data[4], written);
	put_le32(&reply_data[8], total);
	*reply_data_len = 12u;
	return ALP_CC3501E_RESP_OK;
}
