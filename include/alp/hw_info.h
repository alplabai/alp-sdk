/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hw_info.h
 * @brief Runtime hardware identification -- read the assembled
 *        board's identifiers at boot and assert they match the
 *        firmware build.
 *
 * The SoM hardware revision is identified by ONE authoritative
 * surface:
 *
 *   **On-module EEPROM manifest** -- a fixed-layout 128-byte block
 *   at offset 0x0000 of the SoM's on-module 24C128.  Carries the
 *   exact MPN string, hw_rev, factory serial number, and
 *   manufacturing date.  Programmed at production-test time by
 *   `scripts/program_eeprom.py`; read + integrity-checked (magic +
 *   schema_version + CRC32) by the SDK at boot.  The EEPROM travels
 *   with the SoM, so it IS the module's identity -- there is no
 *   ADC resistor-divider cross-check on the SoM side.
 *
 * Carrier boards may still encode their own revision on a board-side
 * BOARD_ID resistor divider (see `board_hw_rev` / `board_id_mv`
 * below); that is a separate, board-side path and is independent of
 * the SoM revision.
 *
 * Typical app usage (place this early in `main()`):
 *
 * @code
 * alp_hw_info_t info;
 * alp_status_t  s = alp_hw_info_read(&info);
 * if (s != ALP_OK) {
 *     // EEPROM unprogrammed (NOT_PROVISIONED), corrupted (IO),
 *     // unreachable (NOT_READY), or no bus configured (NOSUPPORT).
 *     // Decide app policy: halt, degrade, log + continue, ...
 * }
 * s = alp_hw_info_assert_matches_build(&info,
 *                                      ALP_HW_BUILD_SOM_SKU,
 *                                      ALP_HW_BUILD_SOM_HW_REV);
 * if (s != ALP_OK) {
 *     // Firmware was built for a different SKU / rev.  Halt.
 * }
 * @endcode
 *
 * The `ALP_HW_BUILD_*` constants come from `<alp/hw_info_build.h>`,
 * which `scripts/alp_project.py` emits from `board.yaml` at
 * configure time.  Apps pass NULL for a given field to skip
 * matching it.
 *
 * The app-level assert above is opt-in and SKU-aware; separately, when
 * `CONFIG_ALP_SDK_BANNER` and `CONFIG_ALP_SDK_HW_INFO` are both on (the
 * default), the SDK's own boot banner does a narrower, automatic
 * best-effort check of its own: it compares the live manifest's hw_rev
 * against `CONFIG_ALP_SDK_SOM_HW_REV` (the revision the firmware's
 * pad-routing tables were compiled for) and prints a warning on a
 * disagreement, WITHOUT refusing to boot -- promote that to a hard
 * failure via `CONFIG_ALP_SDK_HW_REV_MISMATCH_FATAL`.  See
 * `src/zephyr/alp_banner.c` and issue #1853.  It never fires on a
 * missing/unprovisioned manifest, so a factory-fresh module still boots
 * silently on this front.
 *
 * The runtime EEPROM read path is implemented.  On a build with no
 * EEPROM bus configured (CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID
 * unset / < 0) both entry points return @ref ALP_ERR_NOSUPPORT and
 * the out-struct is zero-filled.
 *
 * @par ABI status: [ABI-STABLE]
 *      v0.3 EEPROM manifest.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_HW_INFO_H
#define ALP_HW_INFO_H

#include <stddef.h> /* offsetof -- see alp_secure_page_mirror_t's field-offset static_asserts */
#include <stdint.h>

#include "alp/peripheral.h" /* alp_status_t */

#ifdef __cplusplus
extern "C" {
#endif

/** Magic value at offset 0 of the EEPROM manifest -- ASCII "ALPH". */
#define ALP_HW_INFO_MAGIC 0x414C5048u

/** Manifest schema version this header understands. */
#define ALP_HW_INFO_SCHEMA_VERSION 1u

/** Field length budgets -- sized so the on-EEPROM manifest fits
 *  in 128 bytes including header + CRC, without padding the
 *  strings past values released MPNs ever need. */
#define ALP_HW_INFO_FAMILY_LEN 16u /**< e.g. "aen". */
#define ALP_HW_INFO_SKU_LEN    24u /**< e.g. "E1M-AEN801". */
#define ALP_HW_INFO_HW_REV_LEN 8u  /**< e.g. "r1". */
#define ALP_HW_INFO_SERIAL_LEN 24u /**< Factory-assigned. */

/**
 * @brief On-EEPROM manifest layout (128 bytes total at offset 0x0000).
 *
 * Programmed at production-test time by `scripts/program_eeprom.py`.
 * All strings are null-terminated.  Reserved bytes are zero on
 * first programming and stay zero until a future schema_version
 * uses them.  The CRC32 (ISO-3309, polynomial 0xEDB88320 -- the
 * same `zlib.crc32` Python uses) covers every byte from `magic`
 * through the final byte of `reserved[]`, exclusive of the CRC
 * field itself.
 *
 * The struct must be packed (no padding) and exactly 128 bytes;
 * the static_assert at the bottom of this header enforces both.
 */
typedef struct alp_hw_info_eeprom_t {
	uint32_t magic;                          /**< @ref ALP_HW_INFO_MAGIC. */
	uint32_t schema_version;                 /**< @ref ALP_HW_INFO_SCHEMA_VERSION. */
	char     family[ALP_HW_INFO_FAMILY_LEN]; /**< Family slug, e.g. "aen". */
	char     sku[ALP_HW_INFO_SKU_LEN];       /**< MPN string, e.g. "E1M-AEN801". */
	char     hw_rev[ALP_HW_INFO_HW_REV_LEN]; /**< Revision, e.g. "r1". */
	char     serial[ALP_HW_INFO_SERIAL_LEN]; /**< Factory-assigned serial. */
	uint16_t mfg_year;                       /**< Manufacturing year (e.g. 2026). */
	uint8_t  mfg_month;                      /**< 1..12. */
	uint8_t  mfg_day;                        /**< 1..31. */
	uint8_t  reserved[40];                   /**< Zero-padded; covered by CRC. */
	uint32_t crc32;                          /**< CRC-32 over preceding bytes. */
} alp_hw_info_eeprom_t;

/** Magic value at offset 0 of the Secure Data Page mirror -- ASCII "ALSP"
 *  ("Alp Lab Secure Page"), deliberately different from @ref
 *  ALP_HW_INFO_MAGIC so the two 24C128 address spaces (the array manifest
 *  at `0x50` vs this mirror at `0x58` selector `0x00`) can never be
 *  mistaken for each other by a magic check alone. */
#define ALP_SECURE_PAGE_MAGIC 0x414C5350u

/** Schema version this header understands for the Secure Data Page mirror.
 *  Independent of @ref ALP_HW_INFO_SCHEMA_VERSION -- the mirror is a
 *  narrower, separately-versioned 64-byte format that gets permanently
 *  frozen per unit at the lock (@ref eeprom_24c128_secure_page_lock), so it
 *  cannot share a version counter with the still-rewritable array. */
#define ALP_SECURE_PAGE_SCHEMA_VERSION 1u

/**
 * @brief Field length budgets for @ref alp_secure_page_mirror_t.
 *
 * Chosen from what the current data actually needs plus headroom sized by
 * how likely each field is to outgrow it -- NOT copied from the array
 * manifest's @ref ALP_HW_INFO_SKU_LEN / @ref ALP_HW_INFO_HW_REV_LEN /
 * @ref ALP_HW_INFO_SERIAL_LEN, whose widths sum to exactly 64 and leave no
 * room for @ref ALP_SECURE_PAGE_MAGIC or @ref ALP_SECURE_PAGE_SCHEMA_VERSION.
 * Full reasoning + a worked test vector: `EEPROM-MANIFEST-SPEC.md`'s Secure
 * Data Page section in alp-sdk-internal.  Once a unit's page is locked,
 * these widths are frozen for that unit forever -- there is no second
 * schema-bump chance the way the still-rewritable array manifest has, so
 * the headroom split matters: `sku` gets the most of it (16, not just 10+1)
 * because it is both the field the array itself budgets the most headroom
 * for (24 bytes against a 10-char pattern) and the one this mirror exists
 * to recover -- a future SKU the array could still hold but this locked
 * mirror could not represent would defeat the mirror's whole purpose.
 * `hw_rev` tracks a physical Altium board revision, the field least likely
 * to grow, so it gets less (12, still 1.7x today's 7 chars). */
#define ALP_SECURE_PAGE_SKU_LEN 16u /**< 10 chars today; sized to the array's 24-byte precedent. */
#define ALP_SECURE_PAGE_HW_REV_LEN 12u /**< 7 chars today; least growth-prone, so least headroom. */
#define ALP_SECURE_PAGE_SERIAL_LEN 23u /**< 12 chars today; the budget's remainder. */

/**
 * @brief On-EEPROM Secure Data Page mirror (64 bytes total).
 *
 * Lives at the onsemi N24S128's second device-select header: I2C address
 * `0x58` (the array's `0x50` + 8), first pointer byte (selector) `0x00`.
 * See `include/alp/chips/eeprom_24c128.h`'s
 * `EEPROM_24C128_ALT_ADDR_OFFSET` / `EEPROM_24C128_SECURE_PAGE_BYTES` and
 * @ref eeprom_24c128_read_identity.
 *
 * A MIRROR, never the primary store: the 128-byte array manifest
 * (@ref alp_hw_info_eeprom_t) at `0x50` offset `0x0000` stays authoritative,
 * and nothing in the SDK's runtime read path depends on this mirror
 * existing.  It exists only to survive the one hazard the array manifest
 * cannot protect itself against -- the array has no write protection at
 * all, so a single stray `eeprom_24c128_write(ctx, 0, ...)` from customer
 * code can wipe SKU / hw_rev / serial with nothing left to recover from.
 * This mirror is written once and then permanently LOCKED during
 * provisioning (@ref eeprom_24c128_secure_page_lock) -- see
 * `docs/som-batch-provisioning-procedure.md` §7 in alp-sdk-internal for the
 * full write/cold-cycle/verify/lock order and why the lock is irreversible.
 *
 * Carries only the immutable core needed to recover a board's identity --
 * SKU, hw_rev, serial, manufacturing date, CRC.  Deliberately does NOT
 * carry `family` or a `reserved[]` block the way @ref alp_hw_info_eeprom_t
 * does: recovering the family is not essential (it is derivable from the
 * SKU) and the 64-byte page has no room to spare for future-proofing that
 * isn't needed today -- see @ref ALP_SECURE_PAGE_SKU_LEN's doc comment for
 * where the budget went instead.
 *
 * The struct must be packed (no padding) and exactly 64 bytes; the
 * static_asserts at the bottom of this header enforce both -- the overall
 * size AND (individually) that @ref alp_secure_page_mirror_t::mfg_year and
 * @ref alp_secure_page_mirror_t::crc32 land at their exact expected offsets,
 * not merely that the struct's total size comes out right.  Every field's
 * offset here lands on its own natural alignment boundary (the 16/12/23
 * string widths were chosen so this is true), so no compiler needs to
 * insert padding for this to hold on any target this SDK supports -- but a
 * total-size check alone cannot tell "no padding" apart from "padding here,
 * one fewer padding byte there", which is exactly why the two field-offset
 * asserts exist as a second, independent check.
 *
 * Forward/backward compatibility: a reader MUST parse this struct only when
 * `magic == ALP_SECURE_PAGE_MAGIC` AND `schema_version` is a value it has
 * explicit code for (today, only `1`) -- never attempt to interpret a
 * `schema_version` it does not recognise, whether lower (an earlier layout
 * it never implemented) or higher (a later layout it predates), the same
 * way @ref alp_hw_info_t's reader refuses an unrecognised
 * @ref ALP_HW_INFO_SCHEMA_VERSION.  This matters more here than for the
 * rewritable array: a locked page can never be corrected, so a reader bug
 * that mis-parses an unrecognised version is permanent for that unit, not
 * a re-flash away.  See EEPROM-MANIFEST-SPEC.md's Secure Data Page section
 * for the full rule.
 *
 * Graceful degradation: the approved footprint-compatible alternate part
 * (STMicro M24128-BFMH6TG) has no second device-select header at all, so
 * on a board populated with that part these bytes never arrive --
 * @ref eeprom_24c128_read_identity still returns ::ALP_OK with
 * `secure_page_valid` false, not an I/O error.  The array manifest is
 * unaffected either way.
 */
typedef struct alp_secure_page_mirror_t {
	uint32_t magic;                              /**< @ref ALP_SECURE_PAGE_MAGIC. */
	uint8_t  schema_version;                     /**< @ref ALP_SECURE_PAGE_SCHEMA_VERSION. */
	char     sku[ALP_SECURE_PAGE_SKU_LEN];       /**< MPN, e.g. "E1M-AEN801". */
	char     hw_rev[ALP_SECURE_PAGE_HW_REV_LEN]; /**< Revision, e.g. "2626-r2". */
	char     serial[ALP_SECURE_PAGE_SERIAL_LEN]; /**< Factory-assigned serial. */
	uint16_t mfg_year;                           /**< Manufacturing year (e.g. 2026). */
	uint8_t  mfg_month;                          /**< 1..12. */
	uint8_t  mfg_day;                            /**< 1..31. */
	uint32_t crc32;                              /**< CRC-32 ISO-3309 over every
	                                                  preceding byte (offsets
	                                                  `[0x00, 0x3C)`); same
	                                                  algorithm, same
	                                                  little-endian trailer
	                                                  placement as @ref
	                                                  alp_hw_info_eeprom_t::crc32. */
} alp_secure_page_mirror_t;

/**
 * @brief Combined runtime board info as returned by @ref alp_hw_info_read.
 *
 * The SoM fields come from the on-module EEPROM manifest (the
 * authoritative SoM identity).  The board-side fields are decoded
 * from the board preset's BOARD_ID path and stay zero/empty when no
 * board is declared in `board.yaml` or no board-side BOARD_ID ADC
 * channel is wired.
 */
typedef struct alp_hw_info_t {
	/* SoM identifiers -- sourced from the on-module EEPROM. */
	char     som_family[ALP_HW_INFO_FAMILY_LEN];
	char     som_sku[ALP_HW_INFO_SKU_LEN];
	char     som_hw_rev[ALP_HW_INFO_HW_REV_LEN];
	char     som_serial[ALP_HW_INFO_SERIAL_LEN];
	uint16_t som_mfg_year;
	uint8_t  som_mfg_month;
	uint8_t  som_mfg_day;

	/* Board identifiers -- decoded from the board-side BOARD_ID
     * ADC + the board preset's hw_revisions table.  No EEPROM
     * on the board today; the name comes from the build-time
     * declaration in board.yaml, not from runtime detection. */
	char     board_name[ALP_HW_INFO_SKU_LEN];
	char     board_hw_rev[ALP_HW_INFO_HW_REV_LEN];
	uint32_t board_id_mv;
} alp_hw_info_t;

/**
 * @brief Read the assembled hardware's identifiers at boot.
 *
 * Reads + integrity-checks the on-module EEPROM manifest (magic +
 * schema_version + CRC32) and copies the SoM identifiers out.  The
 * manifest is the sole source of the SoM hardware revision.  (A
 * board-side BOARD_ID decode is a future addition.)
 *
 * @param[out] out  Populated with whatever could be read.  Fields
 *                  unavailable on the running build stay
 *                  zero/empty.  Never written when the call fails
 *                  with @ref ALP_ERR_INVAL.
 *
 * @return  @ref ALP_OK on a valid manifest read.
 *          @ref ALP_ERR_INVAL when @p out is NULL.
 *          @ref ALP_ERR_NOT_PROVISIONED when the EEPROM reads back
 *                                       blank/unprogrammed (no ALPH magic).
 *          @ref ALP_ERR_IO when the manifest is corrupt (magic present
 *                          but bad schema_version / CRC).
 *          @ref ALP_ERR_NOT_READY when the EEPROM/I2C layer reports the
 *                                 device is unavailable (NAK, bus fault,
 *                                 missing chip).
 *          @ref ALP_ERR_NOSUPPORT when no EEPROM bus is configured
 *                                 (CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID
 *                                 unset / < 0).
 */
alp_status_t alp_hw_info_read(alp_hw_info_t *out);

/**
 * @brief Assert the runtime board matches the firmware build.
 *
 * Compares @p info against compile-time constants the application
 * supplies (typically from the auto-generated
 * `<alp/hw_info_build.h>`).  NULL arguments skip the matching
 * field, letting partial builds (e.g. firmware portable across
 * an MPN family) match on the bits they care about.
 *
 * @param[in] info             Runtime info from @ref alp_hw_info_read.
 * @param[in] expected_sku     If non-NULL, must equal info->som_sku.
 * @param[in] expected_hw_rev  If non-NULL, must equal info->som_hw_rev.
 *
 * @return  @ref ALP_OK on every-supplied-field match.
 *          @ref ALP_ERR_INVAL when @p info is NULL.
 *          @ref ALP_ERR_IO on any supplied-field disagreement.
 *          @ref ALP_ERR_NOSUPPORT mirrors @ref alp_hw_info_read.
 */
alp_status_t alp_hw_info_assert_matches_build(const alp_hw_info_t *info,
                                              const char          *expected_sku,
                                              const char          *expected_hw_rev);

/* ------------------------------------------------------------------ */
/* SoC identity -- silicon-level identifiers reported by the SoC's     */
/* secure / system-controller firmware (where one exists)              */
/* ------------------------------------------------------------------ */

/** Byte budget for the build-time silicon reference string. */
#define ALP_SOC_INFO_REF_LEN 32u
/** Byte budget for the secure-firmware version string (incl. NUL). */
#define ALP_SOC_INFO_FW_VERSION_LEN 80u
/** Maximum SoC unique-serial length in bytes. */
#define ALP_SOC_INFO_SERIAL_MAX_LEN 16u

/**
 * @brief Silicon-level identity as returned by @ref alp_soc_info_read.
 *
 * Complements @ref alp_hw_info_t -- the EEPROM manifest identifies the
 * assembled MODULE (SKU / hw_rev / factory serial), while this struct
 * identifies the SILICON -- the SoC die revision, the factory-fused
 * unique serial, and the version of the secure / system-controller
 * firmware servicing the die.  On SoCs whose power/boot/identity
 * services live behind a dedicated controller core, the backend
 * queries that controller; fields it cannot source stay zero/empty.
 */
typedef struct alp_soc_info_t {
	char     soc_ref[ALP_SOC_INFO_REF_LEN];                  /**< Build-time silicon reference
	                                         (soc_caps ALP_SOC_REF_STR, e.g.
	                                         "alif:ensemble:e8").  Always
	                                         filled, even when the runtime
	                                         query fails. */
	char     secure_fw_version[ALP_SOC_INFO_FW_VERSION_LEN]; /**< Secure / system-controller
	                                                          firmware version string
	                                                          (NUL-terminated).  Empty when
	                                                          the platform has no queryable
	                                                          controller firmware. */
	uint32_t part_number;                         /**< SoC part-number code as reported by the
	                           silicon (vendor-defined encoding); 0 when
	                           unavailable. */
	uint32_t revision_id;                         /**< SoC die-revision identifier; 0 when
	                           unavailable. */
	uint32_t lifecycle;                           /**< Secure-lifecycle state code
	                           (implementation-defined encoding; see the
	                           per-SoM HW reference for the legend).  0
	                           when unavailable. */
	uint8_t  serial[ALP_SOC_INFO_SERIAL_MAX_LEN]; /**< Factory-fused SoC
	                                                  unique serial bytes. */
	uint8_t  serial_len;                          /**< Valid bytes in @ref serial (0 when
	                         unavailable). */
} alp_soc_info_t;

/**
 * @brief Read the silicon-level identity of the running SoC.
 *
 * Zero-fills @p out, stamps @ref alp_soc_info_t::soc_ref from the
 * build-time silicon reference, then asks the active backend to fill
 * the runtime fields (secure-firmware version, part number, die
 * revision, lifecycle, unique serial).  On platforms without a
 * queryable secure / system controller the call returns
 * @ref ALP_ERR_NOSUPPORT with @c soc_ref as the only populated field
 * -- callers that just need "which silicon is this build for" can
 * use that best-effort result unconditionally.
 *
 * @param[out] out  Identity destination.  Zero-filled + @c soc_ref
 *                  stamped even on failure (except INVAL).
 *
 * @return  @ref ALP_OK on a full runtime read.
 *          @ref ALP_ERR_INVAL when @p out is NULL.
 *          @ref ALP_ERR_NOSUPPORT when no runtime identity source
 *                                 exists on this build (soc_ref is
 *                                 still valid).
 *          @ref ALP_ERR_NOT_READY when the controller firmware is
 *                                 asleep/unreachable (retryable).
 *          @ref ALP_ERR_IO on a transport fault or a
 *                          controller-rejected request; fields that
 *                          were read before the fault stay filled.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      New in v0.9 -- portable SoC-identity surface (first consumer:
 *      the AEN SE-service examples).
 */
alp_status_t alp_soc_info_read(alp_soc_info_t *out);

/**
 * @brief Liveness ping of the SoC's secure / system-controller firmware.
 *
 * A bounded round-trip that proves the controller answers before the
 * caller trusts identity or power-profile reads.  Purely diagnostic:
 * no state is read or written.
 *
 * @return  @ref ALP_OK when the controller answered.
 *          @ref ALP_ERR_NOSUPPORT when this build has no controller
 *                                 transport (e.g. native_sim).
 *          @ref ALP_ERR_NOT_READY when the controller is
 *                                 asleep/unreachable (retryable).
 *          @ref ALP_ERR_IO on a transport fault.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      New in v0.9 -- companion to @ref alp_soc_info_read.
 */
alp_status_t alp_soc_secure_fw_ping(void);

/* Compile-time guard: the manifest must be exactly 128 bytes and
 * tightly packed so the production programmer's binary lines up
 * byte-for-byte with the runtime reader's view. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(alp_hw_info_eeprom_t) == 128,
               "alp_hw_info_eeprom_t must be 128 bytes (check struct packing)");
#endif

/* Compile-time guard: the Secure Data Page mirror must be exactly 64 bytes
 * (the physical page size) and tightly packed -- see
 * alp_secure_page_mirror_t's doc comment for why every field's offset was
 * chosen to make that hold without a packed attribute. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(alp_secure_page_mirror_t) == 64,
               "alp_secure_page_mirror_t must be 64 bytes (check struct packing)");
/* A total-size check alone cannot distinguish "no padding" from "padding
 * here, one fewer padding byte there" -- pin two individual field offsets
 * too (the doc comment above explains why these two).  If either ever
 * fires, some target's ABI padded a field this layout assumed would be
 * naturally aligned; the fix is a new schema_version + magic, not a
 * packed attribute retrofitted onto an already-locked format. */
_Static_assert(offsetof(alp_secure_page_mirror_t, mfg_year) == 0x38,
               "alp_secure_page_mirror_t.mfg_year drifted off offset 0x38 -- unexpected padding?");
_Static_assert(offsetof(alp_secure_page_mirror_t, crc32) == 0x3C,
               "alp_secure_page_mirror_t.crc32 drifted off offset 0x3C -- unexpected padding?");
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_HW_INFO_H */
