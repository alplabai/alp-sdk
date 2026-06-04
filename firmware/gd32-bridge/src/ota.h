/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32-bridge OTA — Path A state machine (application bootloader over the
 * bridge).  Wired in from src/bootloader/bootloader.c (bl_dispatch_ota).
 *
 * Safe-by-default: unless built with -DBRIDGE_OTA_PARTITIONED (paired with
 * the partitioned bootloader layout + slot-linked app), ota_dispatch()
 * returns STATUS_NOSUPPORT and performs no flash writes.
 */
#ifndef GD32_BRIDGE_OTA_H
#define GD32_BRIDGE_OTA_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

/* OTA opcode handler (cmd in 0xF0..0xFF). */
gd32_bridge_status_t ota_dispatch(uint8_t cmd, const uint8_t *req_payload, size_t req_payload_len,
                                  uint8_t *reply_payload, size_t reply_payload_cap,
                                  size_t *reply_payload_len);

#endif /* GD32_BRIDGE_OTA_H */
