/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32-bridge application bootloader -- internal header.
 *
 * Wires the OTA opcodes `0xF0..0xFF` reserved in
 * docs/gd32-bridge-protocol.md §10 into a separate dispatcher above
 * protocol.c.  Handler bodies + integration detail are tracked by
 * the maintainer outside this tree; the scaffold replies
 * STATUS_NOSUPPORT so the wire path is exercisable.
 */

#ifndef GD32_BRIDGE_BOOTLOADER_H
#define GD32_BRIDGE_BOOTLOADER_H

#include <stddef.h>
#include <stdint.h>

#include "../protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* OTA opcodes (reserved range 0xF0..0xFF in the bridge protocol). */
#define CMD_OTA_BEGIN 0xF0u
#define CMD_OTA_WRITE_CHUNK 0xF1u
#define CMD_OTA_VERIFY 0xF2u
#define CMD_OTA_COMMIT 0xF3u
#define CMD_OTA_ROLLBACK 0xF4u
#define CMD_OTA_GET_STATE 0xF5u
#define CMD_OTA_ABORT 0xF6u

/* Bridge-protocol handler.  Called by protocol.c when an opcode in
 * the reserved OTA range arrives. */
gd32_bridge_status_t bl_dispatch_ota(uint8_t cmd, const uint8_t *req_payload,
                                     size_t req_payload_len, uint8_t *reply_payload,
                                     size_t reply_payload_cap, size_t *reply_payload_len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GD32_BRIDGE_BOOTLOADER_H */
