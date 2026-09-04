/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal (non-public) declarations for the hw_info EEPROM-manifest
 * reader.  Implemented in src/zephyr/hw_info_zephyr.c and split out
 * from the I2C read path so native_sim tests can classify crafted
 * 128-byte manifest buffers without a real EEPROM device.
 * NOT part of the public <alp/...> API.
 */
#ifndef ALP_SDK_HW_INFO_MANIFEST_H
#define ALP_SDK_HW_INFO_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alp/hw_info.h"
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** CRC-32 (ISO-3309 / zlib polynomial 0xEDB88320) over @p len bytes of @p buf. */
uint32_t alp_hw_info_crc32(const uint8_t *buf, size_t len);

/**
 * @brief Validate a 128-byte EEPROM manifest and populate the SoM half of @p out.
 *
 * @param[in]  manifest  A 128-byte manifest buffer as read from the EEPROM.
 * @param[out] out       Populated on success; left as-is on failure (the
 *                       caller pre-zeroes it; this function does NOT zero it).
 *
 * @return ALP_OK on a valid manifest;
 *         ALP_ERR_NOT_PROVISIONED when no ALPH magic is present (blank/erased EEPROM);
 *         ALP_ERR_IO when magic is present but schema_version or CRC32 is invalid (corruption);
 *         ALP_ERR_INVAL when either pointer is NULL.
 */
alp_status_t alp_hw_info_classify_manifest(const alp_hw_info_eeprom_t *manifest,
                                           alp_hw_info_t              *out);

/**
 * @brief Does the LIVE SoM hw_rev disagree with the one this firmware
 *        BUILD resolved?
 *
 * Pure comparison (no I/O), bounded by @c info->som_hw_rev's own size
 * (@ref ALP_HW_INFO_HW_REV_LEN) -- deliberately NOT a call through
 * @ref alp_hw_info_assert_matches_build(): that entry point's real
 * compare body is compiled out (ALP_HW_INFO_EEPROM_ENABLED == 0) on any
 * build with no EEPROM wired, so a native_sim test with that Kconfig
 * chain off could never reach it.  This entry point carries no such
 * gate, so a native_sim test can drive it with a crafted manifest,
 * without a real EEPROM or a Zephyr boot pass -- mirroring
 * @ref alp_hw_info_classify_manifest above.  The boot banner
 * (src/zephyr/alp_banner.c) is the one production caller today, and it
 * only reaches this after alp_hw_info_read() == ALP_OK, which itself
 * requires EEPROM_ENABLED == 1 -- so the two entry points behave
 * identically in production; this one is just independently testable.
 * See issue #1853.
 *
 * @param[in] info          A successful alp_hw_info_read() result.
 * @param[in] built_hw_rev  CONFIG_ALP_SDK_SOM_HW_REV -- the hw_rev this
 *                          firmware build resolved.  NULL or empty means
 *                          the build wasn't run through
 *                          alp_orchestrate.py: nothing to compare, never
 *                          a mismatch.
 *
 * @return true when @p built_hw_rev is non-empty and disagrees with
 *         info->som_hw_rev; false otherwise (match, NULL @p info, or
 *         nothing to compare).
 */
bool alp_hw_info_build_hw_rev_mismatch(const alp_hw_info_t *info, const char *built_hw_rev);

#ifdef __cplusplus
}
#endif

#endif /* ALP_SDK_HW_INFO_MANIFEST_H */
