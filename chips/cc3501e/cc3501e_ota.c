/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * CC3501E OTA firmware update -- stream a new image over the bridge
 * (opcodes 0x40..0x44, 0x46).  See <alp/chips/cc3501e/ota.h> for the
 * public API.
 */

#include <string.h>
#include <stdint.h>

#include "cc3501e_internal.h"
#include "../../src/common/alp_checked_arith.h"

alp_status_t cc3501e_ota_begin(cc3501e_t *ctx, uint32_t total_len, uint32_t timeout_ms)
{
	uint8_t req[4];
	req[0] = (uint8_t)(total_len & 0xFFu);
	req[1] = (uint8_t)((total_len >> 8) & 0xFFu);
	req[2] = (uint8_t)((total_len >> 16) & 0xFFu);
	req[3] = (uint8_t)((total_len >> 24) & 0xFFu);
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_OTA_BEGIN, req, sizeof(req), NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ota_write(cc3501e_t     *ctx,
                               uint32_t       offset,
                               const uint8_t *data,
                               size_t         len,
                               uint32_t       timeout_ms)
{
	if (data == NULL || len == 0u || len > ALP_CC3501E_OTA_MAX_CHUNK) {
		return ALP_ERR_INVAL;
	}
	/* Frame = offset(LE32) followed by the raw image bytes (<= MAX_PAYLOAD). */
	uint8_t buf[4u + ALP_CC3501E_OTA_MAX_CHUNK];
	buf[0] = (uint8_t)(offset & 0xFFu);
	buf[1] = (uint8_t)((offset >> 8) & 0xFFu);
	buf[2] = (uint8_t)((offset >> 16) & 0xFFu);
	buf[3] = (uint8_t)((offset >> 24) & 0xFFu);
	memcpy(&buf[4], data, len);
	/* The device stages each chunk synchronously into RAM (no flash until FINISH),
	 * so a WRITE neither blocks nor disrupts the bridge -- a plain re-send poll is
	 * safe + fast (the device is idempotent on the cursor).  ALL the OTA flash, and
	 * thus the only bridge-DMA-disruption window, is FINISH. */
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_OTA_WRITE, buf, 4u + len, NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ota_finish(cc3501e_t *ctx, uint32_t timeout_ms)
{
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_OTA_FINISH, NULL, 0, NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ota_abort(cc3501e_t *ctx, uint32_t timeout_ms)
{
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_OTA_ABORT, NULL, 0, NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ota_promote(cc3501e_t *ctx, uint32_t timeout_ms)
{
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_OTA_PROMOTE, NULL, 0, NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ota_status(cc3501e_t *ctx, alp_cc3501e_ota_status_t *out, uint32_t timeout_ms)
{
	if (out == NULL) return ALP_ERR_INVAL;
	uint8_t      reply[12] = { 0 };
	size_t       got       = 0;
	alp_status_t s         = poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_OTA_STATUS, NULL, 0, reply, sizeof(reply), &got, timeout_ms);
	if (s != ALP_OK) return s;
	if (got < sizeof(reply)) return ALP_ERR_IO;
	out->state         = reply[0];
	out->reserved[0]   = reply[1];
	out->reserved[1]   = reply[2];
	out->reserved[2]   = reply[3];
	out->bytes_written = (uint32_t)reply[4] | ((uint32_t)reply[5] << 8) |
	                     ((uint32_t)reply[6] << 16) | ((uint32_t)reply[7] << 24);
	out->total_len = (uint32_t)reply[8] | ((uint32_t)reply[9] << 8) | ((uint32_t)reply[10] << 16) |
	                 ((uint32_t)reply[11] << 24);
	return ALP_OK;
}

alp_status_t
cc3501e_ota_update(cc3501e_t *ctx, const uint8_t *image, size_t len, uint32_t timeout_ms)
{
	if (image == NULL || len == 0u) return ALP_ERR_INVAL;

	/* OTA_BEGIN's total_len is a wire LE32 (<alp/protocol/cc3501e.h>), so the
	 * wire width is the only bound the HOST can know -- it is NOT the real
	 * image maximum.  The device enforces a much smaller one of its own at
	 * BEGIN (CC3501E_OTA_IMAGE_MAX, firmware/cc3501e/hal/ti/cc3501e_hw_ti_ota.c
	 * -- 64 KiB in the TI HAL today, sized by the RAM buffer that stages the
	 * whole image before FINISH), rejecting an oversize BEGIN with
	 * ERR_INVAL before any image data is streamed.  That value is HAL-private
	 * and unpublished on the wire, and a firmware rev that resizes the staging
	 * buffer moves it, so the host deliberately does NOT duplicate it: a
	 * hardcoded copy here would start falsely rejecting valid images the day
	 * the buffer grows.  Enforce only what the wire itself constrains, and
	 * leave the real limit to the device that owns it.
	 *
	 * Reject anything that would not round-trip BEFORE issuing BEGIN,
	 * converting len to the wire width exactly ONCE (#732): every offset
	 * streamed by the loop below is < len, so it is already proven to fit and
	 * the per-chunk (uint32_t)off cast needs no re-validation. */
	uint32_t total_len_u32;
	if (!alp_size_to_u32(len, &total_len_u32)) {
		return ALP_ERR_INVAL;
	}

	alp_status_t s = cc3501e_ota_begin(ctx, total_len_u32, timeout_ms);
	if (s != ALP_OK) return s;

	/* 256 B = the CC35 flash page / psa_fwu_write granularity (the validated
	 * SELFTEST installer used CC3501E_OTA_WRITE_CHUNK 256).  Non-page-sized
	 * chunks make the device psa_fwu_write fail -> the host loops on IO until the
	 * per-frame timeout (silicon 2026-06-19).  Keep host chunks page-aligned.
	 * (The final remainder chunk is < 256 B; psa_fwu accepts the partial tail,
	 * as the selftest's last write did.) */
	const size_t chunk = 256u;
	for (size_t off = 0u; off < len;) {
		size_t n = len - off;
		if (n > chunk) {
			n = chunk;
		}
		s = cc3501e_ota_write(ctx, (uint32_t)off, image + off, n, timeout_ms);
		if (s != ALP_OK) {
			/* A lost reply can leave the host unsure whether the chunk landed.
			 * OTA_WRITE is NOT idempotent (a re-sent already-written offset is
			 * rejected as out-of-order), so re-sync to the device's actual write
			 * cursor before deciding: if it already advanced past this chunk the
			 * write took -- continue; otherwise abort + report.  off and n are
			 * both already proven <= total_len_u32 (the BEGIN bound above), so
			 * the narrowing + addition below is done through the checked
			 * helpers rather than a raw `(uint32_t)(off + n)` cast (#732). */
			alp_cc3501e_ota_status_t st;
			uint32_t                 off_u32, n_u32, expect_u32;
			if (cc3501e_ota_status(ctx, &st, timeout_ms) == ALP_OK &&
			    st.state == ALP_CC3501E_OTA_STATE_WRITING && alp_size_to_u32(off, &off_u32) &&
			    alp_size_to_u32(n, &n_u32) && alp_u32_add_checked(off_u32, n_u32, &expect_u32) &&
			    st.bytes_written == expect_u32) {
				/* chunk landed; the reply was lost -- proceed. */
			} else {
				(void)cc3501e_ota_abort(ctx, timeout_ms);
				return s;
			}
		}
		off += n;
	}

	return cc3501e_ota_finish(ctx, timeout_ms);
}
