/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32-bridge application bootloader -- scaffold.  Every handler
 * returns STATUS_NOSUPPORT until the real implementation lands.
 */

#include "bootloader.h"

gd32_bridge_status_t bl_dispatch_ota(uint8_t cmd,
                                     const uint8_t *req_payload,
                                     size_t req_payload_len,
                                     uint8_t *reply_payload,
                                     size_t reply_payload_cap,
                                     size_t *reply_payload_len)
{
    (void)req_payload;
    (void)req_payload_len;
    (void)reply_payload;
    (void)reply_payload_cap;

    *reply_payload_len = 0u;

    switch (cmd) {
    case CMD_OTA_BEGIN:
    case CMD_OTA_WRITE_CHUNK:
    case CMD_OTA_VERIFY:
    case CMD_OTA_COMMIT:
    case CMD_OTA_ROLLBACK:
    case CMD_OTA_GET_STATE:
    case CMD_OTA_ABORT:
        return STATUS_NOSUPPORT;
    default:
        return STATUS_NOSUPPORT;
    }
}
