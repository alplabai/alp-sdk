/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32-bridge: CRC-32 (IEEE 802.3, reflected, init 0xFFFFFFFF, xorout
 * 0xFFFFFFFF).  Shared by the OTA state machine (src/ota.c) and the
 * bootloader (src/boot/boot_main.c).  Single-call use: seed with 0.
 */
#ifndef GD32_BRIDGE_CRC32_H
#define GD32_BRIDGE_CRC32_H

#include <stddef.h>
#include <stdint.h>

uint32_t ota_crc32(uint32_t crc, const uint8_t *data, size_t len);

#endif /* GD32_BRIDGE_CRC32_H */
