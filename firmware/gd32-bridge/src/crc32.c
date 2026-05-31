/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32-bridge: CRC-32 (IEEE 802.3, reflected). See crc32.h.
 */

#include "crc32.h"

uint32_t ota_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0u; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88820u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}
