/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32-bridge OTA — flash layout + on-flash structures (Path A).
 *
 * OTA_PAGE_SIZE below is the LAYOUT granule (2 KB), not necessarily the
 * silicon erase page: the V2N's GD32G553 ships in DUAL-BANK mode
 * (OBCTL.DBS = 1, silicon-verified 2026-06-04), where the real page is
 * 1 KB (MAIN_FLASH_PAGE_SIZE_DBANK) and addresses >= 0x08040000 are
 * bank 1.  2 KB is a multiple of the page size in both bank modes, so
 * all region bases/sizes stay valid; hal/fmc_ota.c walks the REAL page
 * size and selects the bank per address.  Public slot map:
 *
 *   0x08000000  bootloader   32 KB   (never erased by OTA)
 *   0x08008000  metadata      8 KB   (A/B records, one per page)
 *   0x0800A000  slot A      236 KB
 *   0x08045000  slot B      236 KB
 *   0x08080000  end
 *
 * NOTE: OTA self-flashing is only safe when the firmware is built with
 * the partitioned bootloader layout (-DBRIDGE_OTA_PARTITIONED, paired
 * with the bootloader image + slot-linked app).  The default full-flash
 * build leaves OTA inert (ota_dispatch -> STATUS_NOSUPPORT).  Until the
 * bootloader path is validated on real silicon, GD32 updates go through
 * an external SWD probe (host-driven SWD reflash is not wired this HW rev).
 */
#ifndef GD32_BRIDGE_OTA_LAYOUT_H
#define GD32_BRIDGE_OTA_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

#define OTA_PAGE_SIZE 0x00000800u /* 2 KB single-bank page */

#define OTA_BOOTLOADER_BASE 0x08000000u
#define OTA_BOOTLOADER_SIZE 0x00008000u /* 32 KB */
#define OTA_META_BASE       0x08008000u
#define OTA_META_SIZE       0x00002000u /* 8 KB = 4 pages */
#define OTA_SLOT_A_BASE     0x0800A000u
#define OTA_SLOT_B_BASE     0x08045000u
#define OTA_SLOT_SIZE       0x0003B000u /* 236 KB */
#define OTA_FLASH_END       0x08080000u

/* Two metadata records on separate pages -> power-fail-safe alternation. */
#define OTA_META_REC0 (OTA_META_BASE + 0u * OTA_PAGE_SIZE)
#define OTA_META_REC1 (OTA_META_BASE + 1u * OTA_PAGE_SIZE)

enum { OTA_SLOT_A = 0u, OTA_SLOT_B = 1u };

/* Image header — lives at the start of a slot, carried in the image body
 * (written via OTA_WRITE_CHUNK).  The signature is reserved; verified only
 * when signing is enabled (recommended default: CRC32 enforced, signature
 * hook reserved-but-off until factory key provisioning exists). */
#define OTA_IMG_MAGIC       0x4F544131u /* "OTA1" */
#define OTA_IMG_FMT_VERSION 1u
#define OTA_SIG_LEN         64u /* ECDSA-P256 (r||s) */

typedef struct {
	uint32_t magic;       /* OTA_IMG_MAGIC */
	uint32_t fmt_version; /* OTA_IMG_FMT_VERSION */
	uint32_t img_len;     /* bytes of the image body (incl. this header) */
	uint32_t fw_version;  /* firmware semver, packed */
	uint32_t body_crc32;  /* CRC-32 over the body excluding this field+sig */
	uint8_t  signature[OTA_SIG_LEN];
} ota_img_header_t;

/* A/B metadata record — the bootloader picks the highest `counter` with a
 * valid `rec_crc32`, then boots `active_slot` if the slot's metadata-recorded
 * CRC verifies over the slot contents.
 *
 * The image descriptors are PER-SLOT (struct v2): a ROLLBACK only flips
 * `active_slot` and must leave the rolled-to slot's len/CRC intact for the
 * bootloader to validate against — a single active-slot descriptor (v1)
 * left rollback with nothing valid to write, bricking the part on the
 * next boot. */
#define OTA_META_MAGIC      0x4F544D31u /* "OTM1" */
#define OTA_META_STRUCT_VER 2u

typedef struct {
	uint32_t magic;          /* OTA_META_MAGIC */
	uint32_t struct_version; /* OTA_META_STRUCT_VER */
	uint32_t counter;        /* monotonic; highest valid record wins */
	uint8_t  active_slot;    /* OTA_SLOT_A | OTA_SLOT_B */
	uint8_t  slot_valid;     /* bit0 = slot A valid, bit1 = slot B valid */
	uint8_t  _pad[2];
	uint32_t fw_version[2]; /* per-slot firmware semver (A, B); 0 = unknown */
	uint32_t img_len[2];    /* per-slot image length (A, B) */
	uint32_t img_crc32[2];  /* per-slot image CRC-32 (A, B) */
	uint32_t rec_crc32;     /* CRC-32 over this record excluding this field */
} ota_meta_record_t;

/* Derive a slot's flash base with EXPLICIT validation.  The old
 * `ota_slot_base` silently mapped every non-B value (incl. a corrupt
 * active_slot from persisted metadata) to slot A, so callers could not
 * tell a rejected slot from slot A and might erase / program / verify /
 * boot the wrong region (#741).  This returns false for any slot that
 * is not OTA_SLOT_A/B; callers MUST check before using *base_out. */
static inline bool ota_slot_base_checked(uint8_t slot, uint32_t *base_out)
{
	if (slot == OTA_SLOT_A) {
		*base_out = OTA_SLOT_A_BASE;
		return true;
	}
	if (slot == OTA_SLOT_B) {
		*base_out = OTA_SLOT_B_BASE;
		return true;
	}
	return false;
}

/* Minimum bootable image = at least the initial-MSP + reset-vector
 * words at the slot base (the boot path jumps via those two). */
#define OTA_IMG_MIN_LEN 8u

/* GD32G553 SRAM window a plausible initial MSP must fall in.  Upper
 * bound is generous (covers >128 KB parts) so a valid image is never
 * rejected; the point is to reject a garbage MSP, not size RAM exactly. */
#define OTA_SRAM_BASE 0x20000000u
#define OTA_SRAM_END  0x20040000u

/* Semantic bootability check for a slot's image, beyond the host CRC
 * (#755).  A CRC-valid one-byte / truncated / vector-less image would
 * otherwise pass COMMIT and boot -- the bootloader then reads incomplete
 * MSP/reset words and branches to an invalid address, bricking the part.
 * Require: length covers the MSP+reset head; the initial MSP points into
 * SRAM and is word-aligned; the reset vector lands inside the image and
 * has the Thumb bit set.
 *
 * @p base is the slot's ABSOLUTE flash base (for the reset-range check);
 * @p img is a readable pointer to the image bytes (identical to base on
 * memory-mapped silicon; a HAL-provided pointer under the OTA fake-flash
 * unit tests); @p len is the metadata-recorded, CRC-checked length. */
static inline bool ota_image_bootable(uint32_t base, const uint8_t *img, uint32_t len)
{
	if (len < OTA_IMG_MIN_LEN || img == 0) {
		return false;
	}
	uint32_t msp, reset;
	/* byte-wise load: img may be unaligned and const */
	msp   = (uint32_t)img[0] | ((uint32_t)img[1] << 8) | ((uint32_t)img[2] << 16) |
	        ((uint32_t)img[3] << 24);
	reset = (uint32_t)img[4] | ((uint32_t)img[5] << 8) | ((uint32_t)img[6] << 16) |
	        ((uint32_t)img[7] << 24);
	if ((msp & 3u) != 0u) {
		return false; /* MSP must be word-aligned */
	}
	if (msp < OTA_SRAM_BASE || msp > OTA_SRAM_END) {
		return false; /* MSP must point into SRAM */
	}
	if ((reset & 1u) == 0u) {
		return false; /* reset vector must have the Thumb bit set */
	}
	const uint32_t reset_addr = reset & ~1u;
	if (reset_addr < base || reset_addr >= base + len) {
		return false; /* reset must land inside the image */
	}
	return true;
}

#endif /* GD32_BRIDGE_OTA_LAYOUT_H */
