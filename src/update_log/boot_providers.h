/* SPDX-License-Identifier: Apache-2.0
 *
 * Pure, hardware-independent helpers behind the two real boot-metadata
 * providers (issue #263): MCUboot image-TLV parsing and the Alif SE
 * verified-ATOC policy. No Zephyr/vendor include here -- these take raw
 * bytes/fields the platform glue (src/backends/update_log/mcuboot_boot_
 * metadata.c, alif_se_boot_metadata.c) already fetched, so they build and
 * are unit-tested on native_sim/host without any bootloader or SE present.
 *
 * Deterministic bad-data policy throughout: a short read, a bad magic, a
 * missing TLV, or an image-id mismatch returns ALP_ERR_NOSUPPORT. Nothing
 * here ever fabricates a well-formed-looking entry from incomplete input.
 */
#ifndef ALP_UPDATE_LOG_BOOT_PROVIDERS_H
#define ALP_UPDATE_LOG_BOOT_PROVIDERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alp/update_log.h"

/* MCUboot's public, stable image-trailer wire format (bootutil/image.h):
 * IMAGE_MAGIC, the TLV-info magics, and IMAGE_TLV_SHA256. Re-stated here
 * (rather than including bootutil/image.h) so this file has zero MCUboot/
 * Zephyr build dependency and links into a plain host unit test. */
#define ULOG_MCUBOOT_IMAGE_MAGIC    0x96f3b83dUL
#define ULOG_MCUBOOT_TLV_INFO_MAGIC 0x6907u
#define ULOG_MCUBOOT_TLV_PROT_MAGIC 0x6908u
#define ULOG_MCUBOOT_TLV_SHA256     0x10u

/*
 * Scan one MCUboot image-trailer TLV area for the SHA-256 TLV.
 *
 * @param tlv_area   Bytes starting at the area's image_tlv_info header
 *                    (2-byte magic + 2-byte total-length, both little
 *                    endian), followed by back-to-back (type,len,data)
 *                    entries.
 * @param area_len    Number of bytes actually available at @p tlv_area
 *                    (the caller's read may have been truncated to a
 *                    bound -- this function never reads past it).
 * @param hash_out    On a true return, the 32-byte SHA-256 digest.
 * @return true iff a well-formed 32-byte SHA-256 TLV was found. false for
 *         a bad/absent magic, a truncated entry, or no matching TLV --
 *         the caller must treat false as "no authenticated hash", not
 *         retry with a different interpretation.
 */
bool ulog_mcuboot_tlv_find_sha256(const uint8_t *tlv_area, size_t area_len, uint8_t hash_out[32]);

/*
 * Build a trusted boot-metadata entry from an MCUboot image header + a
 * SHA-256 already extracted from its TLV area.
 *
 * @param header32   The raw 32-byte MCUboot image header (as read from
 *                    offset 0 of the active slot).
 * @param image_hash  32-byte SHA-256 from @ref ulog_mcuboot_tlv_find_sha256.
 * @param confirmed   The platform's boot_is_img_confirmed() result.
 * @param[out] out    Populated on ALP_OK; @c seq is left 0 (the append
 *                     engine assigns it).
 * @return ALP_OK, or ALP_ERR_NOSUPPORT if @p header32 does not start with
 *         MCUboot's image magic (a malformed/absent header -- never
 *         fabricate a version from it); ALP_ERR_INVAL on NULL args.
 */
alp_status_t ulog_mcuboot_build_entry(const uint8_t           header32[32],
                                      const uint8_t           image_hash[32],
                                      bool                    confirmed,
                                      alp_update_log_entry_t *out);

/* The SE reports verification as a CHARACTER in the TOC entry's
 * flags_string, not as a bit of its numeric flags word:
 * se_services/include/services_lib_protocol.h's FLAG_STRING_VERIFY (2) is
 * an index into flags_string, and a verified entry reads 'V' there
 * (bench, E1M-AEN803: flags=0x00000063, flags_string "uLVB"). */
#define ULOG_ALIF_TOC_FLAG_STRING_VERIFY_IDX 2u
#define ULOG_ALIF_TOC_VERIFIED_CHAR          'V'

/*
 * Build a trusted boot-metadata entry from one Alif ATOC TOC entry plus a
 * digest the caller computed over that entry's image bytes.
 *
 * @param toc_image_id   8-byte TOC image_identifier the SE returned.
 * @param expect_image_id 8-byte identifier of THIS build's own image
 *                         (board Kconfig), to reject a TOC entry that
 *                         belongs to a different image on the same table.
 * @param toc_version     The TOC entry's version field, packed
 *                         major<<24 | minor<<16 | patch (as SETOOLS
 *                         prints it); rendered "major.minor.patch".
 * @param toc_verify_char The TOC entry's flags_string character at
 *                         ULOG_ALIF_TOC_FLAG_STRING_VERIFY_IDX.
 * @param image_hash      32-byte digest the caller computed over
 *                         [store_address, store_address + image_size).
 * @param hash_valid       False if the caller could not safely compute a
 *                          digest (e.g. an implausible image_size) --
 *                          propagates as NOSUPPORT rather than a
 *                          zero-filled fabricated hash.
 * @param[out] out         Populated on ALP_OK.
 * @return ALP_OK; ALP_ERR_NOSUPPORT when @p toc_image_id does not match
 *         @p expect_image_id or @p hash_valid is false; ALP_ERR_INVAL on
 *         NULL args. @c status is ALP_UPDATE_STATUS_CONFIRMED when the
 *         SE's verify flag reads 'V', ALP_UPDATE_STATUS_VERIFY_FAILED
 *         otherwise -- this function never returns OK while silently
 *         dropping a failed-verify signal.
 */
alp_status_t ulog_alif_se_build_entry(const uint8_t           toc_image_id[8],
                                      const uint8_t           expect_image_id[8],
                                      uint32_t                toc_version,
                                      char                    toc_verify_char,
                                      const uint8_t           image_hash[32],
                                      bool                    hash_valid,
                                      alp_update_log_entry_t *out);

#endif /* ALP_UPDATE_LOG_BOOT_PROVIDERS_H */
