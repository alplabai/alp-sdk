/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: OTA firmware-update command-family handlers
 * (over-the-bridge PSA-FWU streaming, 0x40..0x47) -- v0.2.  Split out
 * of protocol.c (issue #461); protocol_dispatch() in protocol.c still
 * owns the single command-family switch that routes here.
 */

#include "protocol_internal.h"
/* OTA_UPDATE_MODE reads + arms the bridge SPI's boot mode; the seam lives in
 * transport.h (strong impl hal/ti/transport_hw_ti_spi.c, weak no-ops in
 * transport_spi.c so the stub/CI build still links this TU without a HAL). */
#include "transport.h"

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

/* OTA_UPDATE_MODE (0x47): req = mode(1) { 0 = the normal DMA/callback bridge,
 * 1 = the polled OTA update mode }.  reply = alp_cc3501e_ota_update_mode_t
 * { mode(1) | ota_state(1) | reserved(2) }.
 *
 * WHY THIS OPCODE EXISTS (E1M-AEN801 silicon, 2026-08-21): a SPI_MODE_CALLBACK
 * (DMA) SPI_open on the bridge slave PERMANENTLY prevents psa_fwu_start() and
 * psa_fwu_write() from returning, and SPI_close() does not undo the claim.  The
 * only way out is to never open the slave in callback mode on the boot that runs
 * the flash writes -- hence a persisted flag plus a warm reboot.
 *
 * IDEMPOTENT BY CONTRACT: if the device is ALREADY in the requested mode this
 * replies OK with that mode and does NOT reboot.  The host confirms entry by
 * re-issuing this same opcode until the reply's mode byte matches, so a
 * non-idempotent handler would reboot the device in a loop.
 *
 * OK means QUEUED, not "the mode is live" -- the same property OTA_BEGIN has.
 * The flag is latched and a warm reboot is armed; the existing ack-before-reboot
 * latch is what guarantees the host sees this reply before the link drops
 * (cc3501e_hw_request_reset() only clears its latch once the transport reports
 * the reply PAYLOAD phase fully clocked, which holds on the POLLED path too --
 * poll_service drives the same on_transfer phase machine). */
alp_cc3501e_resp_t handle_ota_update_mode(const uint8_t *req,
                                          size_t         req_len,
                                          uint8_t       *reply_data,
                                          size_t         reply_cap,
                                          size_t        *reply_data_len)
{
	*reply_data_len = 0u;
	if (req_len != 1u) return ALP_CC3501E_RESP_ERR_INVALID;
	if (req[0] > 1u) return ALP_CC3501E_RESP_ERR_INVALID;
	if (reply_cap < 4u) return ALP_CC3501E_RESP_ERR_NO_MEM;

	const bool want = (req[0] == 1u);
	const bool now  = bridge_transport_spi_polled();

	/* Out pointers are individually optional (hal/cc3501e_hw.h); the rc is ignored
	 * because a HAL with no OTA support must still answer the mode query. */
	uint8_t state = 0u;
	(void)cc3501e_hw_ota_status(&state, NULL, NULL);

	reply_data[0]   = now ? 1u : 0u; /* the mode running RIGHT NOW, not the one asked for */
	reply_data[1]   = state;
	reply_data[2]   = 0u;
	reply_data[3]   = 0u;
	*reply_data_len = 4u;

	if (want != now) {
		if (want) {
			bridge_transport_spi_request_polled_boot();
		} else {
			bridge_transport_spi_clear_polled_boot();
		}
		/* Deferred: fires from cc3501e_hw_tick() once this reply has drained. */
		cc3501e_hw_request_reset();
	}
	return ALP_CC3501E_RESP_OK;
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
	/* reserved[1]: a window flush is queued or running (#1610).  While this is
	 * non-zero the host MUST hold off payload-bearing WRITE frames and poll
	 * header-only -- clocking payload into a slave whose DMA is torn down is
	 * exactly what broke the 2026-06-19 per-chunk-flash attempt.  Additive into
	 * an existing reserved byte, so no ALP_CC3501E_PROTOCOL_VERSION bump: a host
	 * that predates it reads 0, which is what the old firmware always sent. */
	reply_data[2] = cc3501e_hw_ota_flush_pending() ? 1u : 0u;
	/* reserved[2]: which psa_fwu_* call failed the last flush (#1610 bench
	 * triage; 0 = none).  The CC3501E has no UART on the debug probe, so this
	 * is the only way a flush fault can name itself.  The matching
	 * psa_status_t low byte rides GET_DIAG_INFO's reserved[1]. */
	{
		uint8_t fs = 0u;
		cc3501e_hw_ota_fault(&fs, 0);
		/* No psa fault latched -> report the TRANSPORT PHASE instead (0x40 | phase),
		 * so the host can see where a polled frame stalled.  0x40 keeps it clear of
		 * the small fail_stage codes. */
		/* Bit 0x80 = the bridge is in POLLED mode.  This is the ONE determinant of
		 * whether psa_fwu_* can return at all (a DMA/callback SPI_open permanently
		 * blocks psa_fwu_start/psa_fwu_write on this silicon; a polled open does
		 * not), and update mode is supposed to guarantee it -- so report it rather
		 * than assume it.  0xC0|phase = polled, 0x40|phase = DMA. */
		reply_data[3] = (fs != 0u)
		                    ? fs
		                    : (uint8_t)(0x40u | (bridge_transport_spi_polled() ? 0x80u : 0u) |
		                                bridge_transport_spi_phase());
	}
	put_le32(&reply_data[4], written);
	put_le32(&reply_data[8], total);
	*reply_data_len = 12u;
	return ALP_CC3501E_RESP_OK;
}
