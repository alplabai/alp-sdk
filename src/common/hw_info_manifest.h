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

#ifdef __cplusplus
}
#endif

#endif /* ALP_SDK_HW_INFO_MANIFEST_H */
