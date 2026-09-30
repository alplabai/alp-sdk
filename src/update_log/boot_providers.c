/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <string.h>

#include "update_log/boot_providers.h"

static uint16_t get_u16le(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

bool ulog_mcuboot_tlv_find_sha256(const uint8_t *tlv_area, size_t area_len, uint8_t hash_out[32])
{
	if (tlv_area == NULL || hash_out == NULL || area_len < 4) {
		return false;
	}

	uint16_t magic = get_u16le(tlv_area + 0);

	if (magic != ULOG_MCUBOOT_TLV_INFO_MAGIC && magic != ULOG_MCUBOOT_TLV_PROT_MAGIC) {
		return false;
	}

	uint16_t tot = get_u16le(tlv_area + 2);
	/* The caller's read may have been bounded below the area's real
	 * total length; never scan past what was actually fetched. */
	size_t avail = (tot < area_len) ? (size_t)tot : area_len;
	size_t off   = 4;

	while (off + 4 <= avail) {
		uint16_t type = get_u16le(tlv_area + off);
		uint16_t len  = get_u16le(tlv_area + off + 2);

		off += 4;
		if (off + len > avail) {
			/* Truncated entry: stop rather than read past what was
			 * fetched or trust a partial payload. */
			return false;
		}
		if (type == ULOG_MCUBOOT_TLV_SHA256 && len == 32) {
			memcpy(hash_out, tlv_area + off, 32);
			return true;
		}
		off += len;
	}

	return false;
}

alp_status_t ulog_mcuboot_build_entry(const uint8_t           header32[32],
                                      const uint8_t           image_hash[32],
                                      bool                    confirmed,
                                      alp_update_log_entry_t *out)
{
	if (header32 == NULL || image_hash == NULL || out == NULL) {
		return ALP_ERR_INVAL;
	}

	uint32_t magic = (uint32_t)header32[0] | ((uint32_t)header32[1] << 8) |
	                 ((uint32_t)header32[2] << 16) | ((uint32_t)header32[3] << 24);

	if (magic != ULOG_MCUBOOT_IMAGE_MAGIC) {
		/* Not a real MCUboot header -- never fabricate a version from
		 * whatever garbage this is. */
		return ALP_ERR_NOSUPPORT;
	}

	uint8_t  ver_major = header32[20];
	uint8_t  ver_minor = header32[21];
	uint16_t ver_rev   = get_u16le(header32 + 22);

	memset(out, 0, sizeof(*out));
	int n = snprintf(out->fw_version,
	                 sizeof(out->fw_version),
	                 "%u.%u.%u",
	                 (unsigned)ver_major,
	                 (unsigned)ver_minor,
	                 (unsigned)ver_rev);
	if (n < 0) {
		return ALP_ERR_NOSUPPORT;
	}

	memcpy(out->image_hash, image_hash, ALP_UPDATE_LOG_HASH_LEN);
	out->status = confirmed ? ALP_UPDATE_STATUS_CONFIRMED : ALP_UPDATE_STATUS_PENDING_CONFIRM;
	return ALP_OK;
}

alp_status_t ulog_alif_se_build_entry(const uint8_t           toc_image_id[8],
                                      const uint8_t           expect_image_id[8],
                                      uint32_t                toc_version,
                                      char                    toc_verify_char,
                                      const uint8_t           image_hash[32],
                                      bool                    hash_valid,
                                      alp_update_log_entry_t *out)
{
	if (toc_image_id == NULL || expect_image_id == NULL || image_hash == NULL || out == NULL) {
		return ALP_ERR_INVAL;
	}
	if (memcmp(toc_image_id, expect_image_id, 8) != 0) {
		/* Wrong TOC entry -- never report metadata for a different
		 * image than the one this build configured. */
		return ALP_ERR_NOSUPPORT;
	}
	if (!hash_valid) {
		return ALP_ERR_NOSUPPORT;
	}

	memset(out, 0, sizeof(*out));
	int n = snprintf(out->fw_version,
	                 sizeof(out->fw_version),
	                 "%u.%u.%u",
	                 (unsigned)((toc_version >> 24) & 0xFFu),
	                 (unsigned)((toc_version >> 16) & 0xFFu),
	                 (unsigned)(toc_version & 0xFFFFu));
	if (n < 0) {
		return ALP_ERR_NOSUPPORT;
	}

	memcpy(out->image_hash, image_hash, ALP_UPDATE_LOG_HASH_LEN);
	out->status = (toc_verify_char == ULOG_ALIF_TOC_VERIFIED_CHAR)
	                  ? ALP_UPDATE_STATUS_CONFIRMED
	                  : ALP_UPDATE_STATUS_VERIFY_FAILED;
	return ALP_OK;
}
