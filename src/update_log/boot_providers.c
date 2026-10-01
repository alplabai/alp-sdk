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

#define ATOC_HDR_MAGIC     "OEMTOC01"
#define ATOC_HDR_WORD_OFF  8u
#define ATOC_ENT_OBJ       0x00u
#define ATOC_ENT_SIZE      0x04u
#define ATOC_ENT_VER       0x10u
#define ATOC_ENT_NAME      0x14u
#define CERT_MAGIC_KEY     0x53426B63u /* "ckBS" */
#define CERT_MAGIC_CONTENT 0x53426363u /* "ccBS" */
#define CERT_HDR_LEN       0x10u
#define CERT_SIG_LEN       384u
#define CERT_REC_OFF       0x1B0u /* one SW record: sha256 at 0x1B0 */
#define CERT_REC_LEN       44u    /* 11 words */
#define CERT_REC_AES_OFF   0x1D8u
#define CERT_FLAGS_NREC(f) (((f) >> 16) & 0xFu)

static uint32_t get_u32le(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Bounds-checked view of [addr, addr+n): NULL when it leaves the MRAM. */
static const uint8_t *
atoc_view(const uint8_t *mram, size_t len, uint32_t base, uint64_t addr, uint64_t n)
{
	if (addr < base || addr - base > len || n > len - (addr - base)) {
		return NULL;
	}
	return mram + (addr - base);
}

alp_status_t ulog_alif_atoc_locate(const uint8_t *mram,
                                   size_t         mram_len,
                                   uint32_t       mram_base,
                                   const uint8_t  image_id[8],
                                   uint32_t       max_len,
                                   uint32_t      *img_addr,
                                   uint32_t      *img_len,
                                   uint8_t        cert_hash[32],
                                   uint32_t      *entry_version)
{
	if (mram == NULL || image_id == NULL || img_addr == NULL || img_len == NULL ||
	    cert_hash == NULL || entry_version == NULL) {
		return ALP_ERR_INVAL;
	}
	if (mram_len < ULOG_ALIF_ATOC_TRAILER_LEN + ULOG_ALIF_ATOC_HDR_LEN) {
		return ALP_ERR_NOSUPPORT;
	}

	uint64_t       trailer_addr = (uint64_t)mram_base + mram_len - ULOG_ALIF_ATOC_TRAILER_LEN;
	const uint8_t *tr           = mram + mram_len - ULOG_ALIF_ATOC_TRAILER_LEN;
	uint64_t       hdr_addr     = get_u32le(tr + 4);
	uint64_t       pkg_start    = get_u32le(tr + 8);
	uint64_t       pkg_end      = pkg_start + get_u32le(tr + 12);

	if (atoc_view(mram, mram_len, mram_base, pkg_start, pkg_end - pkg_start) == NULL ||
	    hdr_addr < pkg_start || hdr_addr >= pkg_end ||
	    hdr_addr + ULOG_ALIF_ATOC_HDR_LEN > trailer_addr) {
		return ALP_ERR_NOSUPPORT;
	}

	const uint8_t *hdr = mram + (hdr_addr - mram_base);

	if (memcmp(hdr, ATOC_HDR_MAGIC, 8) != 0) {
		return ALP_ERR_NOSUPPORT;
	}

	uint32_t w     = get_u32le(hdr + ATOC_HDR_WORD_OFF);
	uint32_t count = w >> 16;

	if ((w & 0xFFFFu) != ULOG_ALIF_ATOC_ENTRY_LEN ||
	    hdr_addr + ULOG_ALIF_ATOC_HDR_LEN + (uint64_t)count * ULOG_ALIF_ATOC_ENTRY_LEN >
	        trailer_addr) {
		return ALP_ERR_NOSUPPORT;
	}

	const uint8_t *ent = hdr + ULOG_ALIF_ATOC_HDR_LEN;
	uint32_t       i;

	for (i = 0; i < count; i++, ent += ULOG_ALIF_ATOC_ENTRY_LEN) {
		if (memcmp(ent + ATOC_ENT_NAME, image_id, 8) == 0) {
			break;
		}
	}
	if (i == count) {
		return ALP_ERR_NOSUPPORT;
	}

	uint32_t       ent_size = get_u32le(ent + ATOC_ENT_SIZE);
	uint64_t       cert     = get_u32le(ent + ATOC_ENT_OBJ);
	const uint8_t *c        = NULL;
	uint32_t       wl       = 0;

	/* Skip key certs, then land on the content cert. */
	for (i = 0; i <= ULOG_ALIF_CERT_MAX_KEY_CERTS; i++) {
		c = atoc_view(mram, mram_len, mram_base, cert, CERT_HDR_LEN);
		if (c == NULL) {
			return ALP_ERR_NOSUPPORT;
		}
		wl = get_u32le(c + 8) & 0xFFFFu;
		if (get_u32le(c) != CERT_MAGIC_KEY) {
			break;
		}
		cert += (uint64_t)wl * 4 + CERT_SIG_LEN;
	}
	if (get_u32le(c) != CERT_MAGIC_CONTENT || CERT_FLAGS_NREC(get_u32le(c + 12)) != 1u ||
	    (uint64_t)wl * 4 < CERT_REC_LEN || (uint64_t)wl * 4 - CERT_REC_LEN != CERT_REC_OFF) {
		return ALP_ERR_NOSUPPORT;
	}

	/* Signed body + signature + 8-byte unsigned param record. */
	c = atoc_view(mram, mram_len, mram_base, cert, (uint64_t)wl * 4 + CERT_SIG_LEN + 8);
	if (c == NULL) {
		return ALP_ERR_NOSUPPORT;
	}

	const uint8_t *param = c + (size_t)wl * 4 + CERT_SIG_LEN;
	uint32_t       flash = get_u32le(param);
	uint32_t       len   = get_u32le(param + 4);

	if (get_u32le(c + CERT_REC_AES_OFF) != 0u || len != ent_size || len == 0u || len > max_len ||
	    flash < ULOG_ALIF_ATOC_FLASH_ALIAS ||
	    atoc_view(mram, mram_len, mram_base, (uint64_t)flash - ULOG_ALIF_ATOC_FLASH_ALIAS, len) ==
	        NULL) {
		return ALP_ERR_NOSUPPORT;
	}

	memcpy(cert_hash, c + CERT_REC_OFF, 32);
	*img_addr      = flash - ULOG_ALIF_ATOC_FLASH_ALIAS;
	*img_len       = len;
	*entry_version = get_u32le(ent + ATOC_ENT_VER);
	return ALP_OK;
}
