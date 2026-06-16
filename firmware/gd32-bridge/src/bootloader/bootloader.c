/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32-bridge application bootloader -- OTA opcode entry.  Routes the
 * reserved 0xF0..0xFF range into the Path-A state machine (src/ota.c),
 * which is safe-by-default: no flash writes unless built with
 * -DBRIDGE_OTA_PARTITIONED on a backend with real FMC support, otherwise
 * it returns STATUS_NOSUPPORT.
 */

#include "bootloader.h"
#include "../ota.h"

gd32_bridge_status_t bl_dispatch_ota(uint8_t        cmd,
                                     const uint8_t *req_payload,
                                     size_t         req_payload_len,
                                     uint8_t       *reply_payload,
                                     size_t         reply_payload_cap,
                                     size_t        *reply_payload_len)
{
	return ota_dispatch(
	    cmd, req_payload, req_payload_len, reply_payload, reply_payload_cap, reply_payload_len);
}
