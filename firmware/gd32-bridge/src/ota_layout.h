/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32-bridge OTA — flash layout + on-flash structures (Path A).
 *
 * Single-bank, 2 KB page (MAIN_FLASH_PAGE_SIZE_SBANK = 0x800, per the
 * vendor FMC driver).  Layout matches the design proposal
 * (alp-sdk-internal/docs/gd32-bridge-ota-path-a-design.md §2):
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

#include <stdint.h>

#define OTA_PAGE_SIZE          0x00000800u           /* 2 KB single-bank page */

#define OTA_BOOTLOADER_BASE    0x08000000u
#define OTA_BOOTLOADER_SIZE    0x00008000u           /* 32 KB */
#define OTA_META_BASE          0x08008000u
#define OTA_META_SIZE          0x00002000u           /* 8 KB = 4 pages */
#define OTA_SLOT_A_BASE        0x0800A000u
#define OTA_SLOT_B_BASE        0x08045000u
#define OTA_SLOT_SIZE          0x0003B000u           /* 236 KB */
#define OTA_FLASH_END          0x08080000u

/* Two metadata records on separate pages -> power-fail-safe alternation. */
#define OTA_META_REC0          (OTA_META_BASE + 0u * OTA_PAGE_SIZE)
#define OTA_META_REC1          (OTA_META_BASE + 1u * OTA_PAGE_SIZE)

enum { OTA_SLOT_A = 0u, OTA_SLOT_B = 1u };

/* Image header — lives at the start of a slot, carried in the image body
 * (written via OTA_WRITE_CHUNK).  The signature is reserved; verified only
 * when signing is enabled (recommended default: CRC32 enforced, signature
 * hook reserved-but-off until factory key provisioning exists). */
#define OTA_IMG_MAGIC          0x4F544131u           /* "OTA1" */
#define OTA_IMG_FMT_VERSION    1u
#define OTA_SIG_LEN            64u                    /* ECDSA-P256 (r||s) */

typedef struct {
    uint32_t magic;            /* OTA_IMG_MAGIC */
    uint32_t fmt_version;      /* OTA_IMG_FMT_VERSION */
    uint32_t img_len;          /* bytes of the image body (incl. this header) */
    uint32_t fw_version;       /* firmware semver, packed */
    uint32_t body_crc32;       /* CRC-32 over the body excluding this field+sig */
    uint8_t  signature[OTA_SIG_LEN];
} ota_img_header_t;

/* A/B metadata record — the bootloader picks the highest `counter` with a
 * valid `rec_crc32`, then boots `active_slot` if its image header verifies. */
#define OTA_META_MAGIC         0x4F544D31u           /* "OTM1" */
#define OTA_META_STRUCT_VER    1u

typedef struct {
    uint32_t magic;            /* OTA_META_MAGIC */
    uint32_t struct_version;   /* OTA_META_STRUCT_VER */
    uint32_t counter;          /* monotonic; highest valid record wins */
    uint8_t  active_slot;      /* OTA_SLOT_A | OTA_SLOT_B */
    uint8_t  slot_valid;       /* bit0 = slot A valid, bit1 = slot B valid */
    uint8_t  _pad[2];
    uint32_t fw_version;       /* version of the active slot */
    uint32_t img_len;          /* length of the active slot image */
    uint32_t img_crc32;        /* CRC-32 of the active slot image */
    uint32_t rec_crc32;        /* CRC-32 over this record excluding this field */
} ota_meta_record_t;

static inline uint32_t ota_slot_base(uint8_t slot)
{
    return (slot == OTA_SLOT_B) ? OTA_SLOT_B_BASE : OTA_SLOT_A_BASE;
}

#endif /* GD32_BRIDGE_OTA_LAYOUT_H */
