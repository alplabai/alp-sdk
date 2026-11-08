/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * MCUboot boot-metadata provider for <alp/update_log.h> (issue #263).
 *
 * Reads the booted image's real version and SHA-256 straight from
 * MCUboot's own public, stable image-trailer wire format (bootutil's
 * IMAGE_MAGIC header + IMAGE_TLV_SHA256 trailer TLV) via Zephyr's public
 * flash_area API on the active slot -- not the blinfo retention seam:
 * blinfo_lookup() only exposes TLV_MAJOR_BLINFO facts (mode / running
 * slot / bootloader version), and MCUboot's measured-boot shared-data TLV
 * (TLV_MAJOR_IAS / SW_BOOT_RECORD) carries a CBOR-encoded attestation
 * report, not a plain version/hash pair -- pulling in a CBOR decoder for
 * this single field was a bigger dependency than reading the image's own
 * trailer directly.
 *
 * Overrides the weak default alp_update_log_boot_metadata_read()
 * (src/update_log_boot_metadata.c) with a strong definition when
 * CONFIG_ALP_SDK_UPDATE_LOG_BOOT_MCUBOOT is set. The TLV walk and header
 * magic/version decode are pure functions in update_log/boot_providers.c,
 * unit-tested on native_sim without any bootloader present; this file is
 * only the platform glue that fetches the bytes.
 */

#include <string.h>

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

#include "update_log/boot_metadata.h"
#include "update_log/boot_providers.h"

#if defined(CONFIG_ALP_SDK_UPDATE_LOG_BOOT_MCUBOOT)

/* Generous enough for every real MCUboot TLV area (signature + hash +
 * optional dependency/boot-record TLVs) while keeping the scan to one
 * bounded stack buffer -- see ulog_mcuboot_tlv_find_sha256()'s truncation
 * handling for what happens if a board's TLV area is ever bigger. */
#define ULOG_MCUBOOT_TLV_SCAN_MAX 512

alp_status_t alp_update_log_boot_metadata_read(alp_update_log_entry_t *entry_out)
{
	if (entry_out == NULL) {
		return ALP_ERR_INVAL;
	}
	memset(entry_out, 0, sizeof(*entry_out));

	uint8_t                  area_id = boot_fetch_active_slot();
	const struct flash_area *fap     = NULL;

	if (flash_area_open(area_id, &fap) != 0 || fap == NULL) {
		return ALP_ERR_NOSUPPORT;
	}

	uint8_t header[32];

	if (flash_area_read(fap, 0, header, sizeof(header)) != 0) {
		flash_area_close(fap);
		return ALP_ERR_NOSUPPORT;
	}

	uint16_t hdr_size  = (uint16_t)(header[8] | ((uint16_t)header[9] << 8));
	uint16_t prot_size = (uint16_t)(header[10] | ((uint16_t)header[11] << 8));
	uint32_t img_size  = (uint32_t)header[12] | ((uint32_t)header[13] << 8) |
	                     ((uint32_t)header[14] << 16) | ((uint32_t)header[15] << 24);

	size_t  region_off = (size_t)hdr_size + (size_t)img_size;
	uint8_t tlv_blob[ULOG_MCUBOOT_TLV_SCAN_MAX];
	uint8_t hash[32];
	bool    found = false;

	/* The SHA-256 TLV can land in either the protected TLV area (part of
	 * what the signature covers) or the unprotected one, depending on
	 * the signing config -- scan whichever areas actually exist rather
	 * than assuming one. Protected area first, since it directly
	 * follows the image body at a known offset. */
	if (prot_size > 0) {
		size_t n = MIN((size_t)prot_size, sizeof(tlv_blob));

		if (flash_area_read(fap, region_off, tlv_blob, n) == 0) {
			found = ulog_mcuboot_tlv_find_sha256(tlv_blob, n, hash);
		}
		region_off += prot_size;
	}

	if (!found) {
		/* Peek the unprotected area's own image_tlv_info header first
		 * to learn its real length before deciding how much to read;
		 * the TLV walk itself re-checks that length against our
		 * bounded read, never trusting it past what was fetched. */
		uint8_t info[4];

		if (flash_area_read(fap, region_off, info, sizeof(info)) == 0) {
			uint16_t tot = (uint16_t)(info[2] | ((uint16_t)info[3] << 8));
			size_t   n   = MIN((size_t)tot, sizeof(tlv_blob));

			if (n >= sizeof(info) && flash_area_read(fap, region_off, tlv_blob, n) == 0) {
				found = ulog_mcuboot_tlv_find_sha256(tlv_blob, n, hash);
			}
		}
	}

	flash_area_close(fap);

	if (!found) {
		/* No authenticated SHA-256 available: report NOSUPPORT rather
		 * than append an entry with a fabricated or zeroed hash. */
		return ALP_ERR_NOSUPPORT;
	}

	return ulog_mcuboot_build_entry(header, hash, boot_is_img_confirmed(), entry_out);
}

#endif /* CONFIG_ALP_SDK_UPDATE_LOG_BOOT_MCUBOOT */
