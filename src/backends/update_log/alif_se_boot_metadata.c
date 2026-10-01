/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif SE verified-ATOC boot-metadata provider for <alp/update_log.h>
 * (issue #263).
 *
 * The wrapped se_service_* client (se_services/zephyr/include/se_service.h,
 * the same transport src/backends/soc_info/alif_se.c and
 * src/backends/mproc/alif_se_boot.c already drive) has no per-entry TOC
 * query of its own, so this backend builds the raw SERVICE_SYSTEM_MGMT_
 * GET_TOC_INFO request packet and sends it over the public generic
 * se_service_send_request() transport -- the same pattern
 * src/backends/security/se_cryptocell.c and src/backends/ext/alif/
 * storage.c already use for services with no dedicated wrapper.
 *
 * The ATOC has no per-entry digest field (SERVICES_toc_info_t carries
 * image_identifier/version/store_address/image_size/flags, not a hash), so
 * this backend computes the SHA-256 itself over the TOC entry's own
 * [store_address, store_address + image_size) MRAM range -- the same
 * bytes the SE already verified against the entry's signature before
 * this image ran. That range is independently bounded by
 * CONFIG_ALP_SDK_UPDATE_LOG_BOOT_ALIF_SE_MAX_IMAGE_SIZE before it is ever
 * hashed, so a corrupt/huge image_size field cannot turn this into an
 * unbounded read or a multi-second stall.
 *
 * Overrides the weak default alp_update_log_boot_metadata_read()
 * (src/update_log_boot_metadata.c). The identity/verify policy (image-id
 * match, verify-bit -> status mapping) is the pure
 * ulog_alif_se_build_entry() in update_log/boot_providers.c, unit-tested
 * on native_sim without any SE present; this file is only the platform
 * glue that fetches the TOC entry and computes the digest.
 */

#include <string.h>

#include <se_service.h>

#include "update_log/boot_metadata.h"
#include "update_log/boot_providers.h"
#include "update_log/sha256.h"

#if defined(CONFIG_ALP_SDK_UPDATE_LOG_BOOT_ALIF_SE)

alp_status_t alp_update_log_boot_metadata_read(alp_update_log_entry_t *entry_out)
{
	if (entry_out == NULL) {
		return ALP_ERR_INVAL;
	}
	memset(entry_out, 0, sizeof(*entry_out));

	uint8_t expect_id[8] = { 0 };
	size_t  idlen        = strlen(CONFIG_ALP_SDK_UPDATE_LOG_BOOT_ALIF_SE_IMAGE_ID);

	if (idlen > sizeof(expect_id)) {
		idlen = sizeof(expect_id);
	}
	memcpy(expect_id, CONFIG_ALP_SDK_UPDATE_LOG_BOOT_ALIF_SE_IMAGE_ID, idlen);

	uint32_t toc_n = 0;

	if (se_service_get_toc_number(&toc_n) != 0) {
		return ALP_ERR_NOSUPPORT;
	}
	if (toc_n > SERVICES_NUMBER_OF_TOC_ENTRIES) {
		toc_n = SERVICES_NUMBER_OF_TOC_ENTRIES;
	}

	for (uint32_t i = 0; i < toc_n; i++) {
		get_toc_data_t pkt = { 0 };

		pkt.header.hdr_service_id = SERVICE_SYSTEM_MGMT_GET_TOC_INFO;
		pkt.send_entry_idx        = i;

		if (se_service_send_request((uint32_t *)&pkt, (uint32_t)sizeof(pkt)) != 0) {
			continue; /* transport miss on this entry; try the next */
		}
		if (pkt.header.hdr_error_code != 0 || pkt.resp_error_code != 0) {
			continue;
		}

		uint8_t got_id[8];

		memcpy(got_id, (const void *)pkt.resp_toc_entry.resp_image_identifier, sizeof(got_id));
		if (memcmp(got_id, expect_id, sizeof(got_id)) != 0) {
			continue;
		}

		uint32_t size = pkt.resp_toc_entry.resp_image_size;

		if (size == 0 || size > CONFIG_ALP_SDK_UPDATE_LOG_BOOT_ALIF_SE_MAX_IMAGE_SIZE) {
			/* Matches our configured image id but the TOC entry
			 * itself is malformed or implausible: never fabricate
			 * a hash over a size we cannot trust. */
			return ALP_ERR_NOSUPPORT;
		}

		uint32_t store = pkt.resp_toc_entry.resp_store_address;

		if (store == 0u) {
			/* SES v1.110 reports store_address 0 for every TOC entry
			 * (bench, E1M-AEN803, including A32_APP and BOOTLOAD that
			 * SETOOLS places at 0x80020000 / 0x80002000). Hashing
			 * [0, size) would digest live TCM, not the image. */
			return ALP_ERR_NOSUPPORT;
		}

		uint8_t hash[32];

		ulog_sha256((const unsigned char *)(uintptr_t)store, size, hash);

		return ulog_alif_se_build_entry(
		    got_id,
		    expect_id,
		    pkt.resp_toc_entry.resp_version,
		    (char)pkt.resp_toc_entry.resp_flags_string[ULOG_ALIF_TOC_FLAG_STRING_VERIFY_IDX],
		    hash,
		    true,
		    entry_out);
	}

	/* No TOC entry matched this build's configured image id: never
	 * report metadata for a different image on the same table. */
	return ALP_ERR_NOSUPPORT;
}

#endif /* CONFIG_ALP_SDK_UPDATE_LOG_BOOT_ALIF_SE */
