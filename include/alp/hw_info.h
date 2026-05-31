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
 * The SDK identifies the assembled hardware via two independent
 * surfaces (see `docs/cc3501e-bridge.md` "Boot model" and
 * `docs/board-config.md` "Hardware revision tracking" for the
 * full design):
 *
 *   1. **BOARD_ID ADC + resistor divider** -- one ADC pin per
 *      board (SoM-side and board-side) fed by a 1.8 V resistor
 *      divider.  Encodes the coarse rev string (r1, r2, ...).
 *      Per-rev resistor + nominal mV values live in
 *      `metadata/e1m_modules/<family>/hw-revisions.yaml` and the
 *      board's `board.yaml`.
 *
 *   2. **On-module EEPROM manifest** -- a fixed-layout 128-byte
 *      block at offset 0x0000 of the SoM's on-module 24C128 (AEN
 *      family populates one by default).  Carries the exact MPN
 *      string, hw_rev, factory serial number, and manufacturing
 *      date.  Programmed at production test time by
 *      `scripts/program_eeprom.py`; read by the SDK at boot.
 *
 * The two are cross-checked: the EEPROM's `hw_rev` must agree
 * with the rev the ADC voltage decodes to.  If they disagree
 * (production-line error, swapped components) the read fails
 * loudly so the boot path can halt before the firmware brings up
 * anything that depends on the wrong hardware.
 *
 * Typical app usage (place this early in `main()`):
 *
 * @code
 * alp_hw_info_t info;
 * alp_status_t  s = alp_hw_info_read(&info);
 * if (s != ALP_OK) {
 *     // EEPROM unprogrammed, corrupted, or ADC mismatch.
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
 * The `ALP_HW_BUILD_*` constants come from a future
 * `<alp/hw_info_build.h>` that `scripts/alp_project.py` emits
 * from `board.yaml` (v0.3.x deliverable).  Until that lands,
 * apps pass NULL to skip the matching field.
 *
 * v0.3 ships the API contract only; the runtime EEPROM + ADC
 * read paths land in v0.3.x once the per-family BOARD_ID ADC
 * channels are filled in by the user-supplied HW writeups.  Until
 * then both entry points return @ref ALP_ERR_NOSUPPORT and the
 * out-struct is zero-filled.
 *
 * @par ABI status: [ABI-STABLE]
 *      v0.3 EEPROM manifest + BOARD_ID ADC.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_HW_INFO_H
#define ALP_HW_INFO_H

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
#define ALP_HW_INFO_SKU_LEN 24u    /**< e.g. "E1M-AEN701". */
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
    char     sku[ALP_HW_INFO_SKU_LEN];       /**< MPN string, e.g. "E1M-AEN701". */
    char     hw_rev[ALP_HW_INFO_HW_REV_LEN]; /**< Revision, e.g. "r1". */
    char     serial[ALP_HW_INFO_SERIAL_LEN]; /**< Factory-assigned serial. */
    uint16_t mfg_year;                       /**< Manufacturing year (e.g. 2026). */
    uint8_t  mfg_month;                      /**< 1..12. */
    uint8_t  mfg_day;                        /**< 1..31. */
    uint8_t  reserved[40];                   /**< Zero-padded; covered by CRC. */
    uint32_t crc32;                          /**< CRC-32 over preceding bytes. */
} alp_hw_info_eeprom_t;

/**
 * @brief Combined runtime board info as returned by @ref alp_hw_info_read.
 *
 * Populated from BOTH the EEPROM manifest (authoritative MPN +
 * serial + dates) and the BOARD_ID ADC readings (cross-check on
 * hw_rev).  Board-side fields stay zero/empty when no board
 * is declared in `board.yaml` or no board-side BOARD_ID ADC
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
    /** Measured SoM BOARD_ID ADC reading, mV.  0 when not read. */
    uint32_t som_board_id_mv;

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
 * Reads, in order:
 *
 *   -# The on-module EEPROM manifest (24C128 on the AEN family)
 *      and validates @ref ALP_HW_INFO_MAGIC + schema_version +
 *      CRC32 over the manifest bytes.
 *   -# The SoM-side BOARD_ID ADC channel + cross-checks the mV
 *      reading against the hw_rev row in the family's
 *      hw-revisions table (so a swapped or relabelled module
 *      surfaces here).
 *   -# The board-side BOARD_ID ADC channel (when the board
 *      preset declares one) and decodes the board hw_rev.
 *
 * @param[out] out  Populated with whatever could be read.  Fields
 *                  unavailable on the running build stay
 *                  zero/empty.  Never written when the call fails
 *                  with @ref ALP_ERR_INVAL.
 *
 * @return  @ref ALP_OK on a valid, fully populated manifest.
 *          @ref ALP_ERR_INVAL when @p out is NULL.
 *          @ref ALP_ERR_NOT_READY when no EEPROM is reachable
 *                                 (bus error, missing chip).
 *          @ref ALP_ERR_NOT_PROVISIONED when the EEPROM reads back
 *                                       with no ALPH magic (blank /
 *                                       erased module, not yet
 *                                       programmed by the factory tool).
 *          @ref ALP_ERR_IO when magic is present but the manifest is
 *                          corrupt (bad schema_version or CRC32).
 *          @ref ALP_ERR_NOSUPPORT in v0.3 -- the runtime impl
 *          lands in v0.3.x.
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
alp_status_t alp_hw_info_assert_matches_build(const alp_hw_info_t *info, const char *expected_sku,
                                              const char *expected_hw_rev);

/* Compile-time guard: the manifest must be exactly 128 bytes and
 * tightly packed so the production programmer's binary lines up
 * byte-for-byte with the runtime reader's view. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(alp_hw_info_eeprom_t) == 128,
               "alp_hw_info_eeprom_t must be 128 bytes (check struct packing)");
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_HW_INFO_H */
