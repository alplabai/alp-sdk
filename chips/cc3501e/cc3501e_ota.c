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

	alp_status_t s = cc3501e_ota_begin(ctx, (uint32_t)len, timeout_ms);
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
			 * write took -- continue; otherwise abort + report. */
			alp_cc3501e_ota_status_t st;
			if (cc3501e_ota_status(ctx, &st, timeout_ms) == ALP_OK &&
			    st.state == ALP_CC3501E_OTA_STATE_WRITING &&
			    st.bytes_written == (uint32_t)(off + n)) {
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
