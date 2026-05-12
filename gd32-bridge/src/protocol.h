/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal header for the gd32-bridge firmware.  Declares the
 * protocol opcodes + the dispatcher entry that both transports
 * (SPI + I2C) feed into.
 *
 * The wire-side spec is in ../docs/gd32-bridge-protocol.md; opcode
 * numbering MUST stay in sync with <alp/chips/gd32g553.h> on the
 * host side.
 */

#ifndef GD32_BRIDGE_PROTOCOL_H
#define GD32_BRIDGE_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --------------------------------------------------------------- */
/* Wire constants -- keep in sync with the host header.              */
/* --------------------------------------------------------------- */

#define GD32_BRIDGE_SOF                  0xA5u
#define GD32_BRIDGE_I2C_REG_CMD          0x00u
#define GD32_BRIDGE_ADC_MAX_SAMPLES      8u
#define GD32_BRIDGE_BUILD_ID_LEN         20u

#define PROTOCOL_VERSION_MAJOR           0u
#define PROTOCOL_VERSION_MINOR           2u
#define PROTOCOL_VERSION_PATCH           0u

/* Maximum wire payload either direction.  See host-side header for
 * the matching ADC_READ-driven limit. */
#define GD32_BRIDGE_MAX_PAYLOAD_BYTES    (1u + (GD32_BRIDGE_ADC_MAX_SAMPLES * 2u))

typedef enum {
    CMD_PING                  = 0x00,
    CMD_GET_VERSION           = 0x01,
    CMD_GET_BUILD_ID          = 0x02,
    CMD_RESET_REASON          = 0x03,
    CMD_GPIO_READ             = 0x10,
    CMD_GPIO_WRITE            = 0x11,
    CMD_PWM_SET               = 0x20,
    CMD_PWM_GET               = 0x21,
    CMD_ADC_READ              = 0x30,
    CMD_DA9292_STATUS_FORWARD = 0x40,
    /* v0.2 additions -- the GD32 carries every E1M-standard analog
     * and counter peripheral on V2N (per gd32-io-mcu-map.tsv); the
     * SDK's portable surface routes through these. */
    CMD_DAC_SET               = 0x50,
    CMD_DAC_GET               = 0x51,
    CMD_QENC_READ             = 0x60,
    CMD_QENC_RESET            = 0x61,
    CMD_COUNTER_READ          = 0x70,
} gd32_bridge_cmd_t;

/* Wire-side status byte; mirrors the table in docs/gd32-bridge-protocol.md §6.
 * Note the unsigned magnitude vs the host's negative alp_status_t -- the
 * firmware doesn't depend on the host's signed enum. */
typedef enum {
    STATUS_OK            = 0x00,
    STATUS_INVAL         = 0x01,
    STATUS_NOT_READY     = 0x02,
    STATUS_BUSY          = 0x03,
    STATUS_TIMEOUT       = 0x04,
    STATUS_IO            = 0x05,
    STATUS_NOSUPPORT     = 0x06,
    STATUS_NOMEM         = 0x07,
    STATUS_OUT_OF_RANGE  = 0x08,
    STATUS_NO_PENDING    = 0x80, /* I2C-only: read before any matching write */
} gd32_bridge_status_t;

/* --------------------------------------------------------------- */
/* Dispatcher                                                         */
/* --------------------------------------------------------------- */

/*
 * protocol_dispatch -- called by either transport when a complete
 * request envelope has been validated (CRC OK, framing OK).
 *
 * Inputs:
 *   cmd            -- opcode (one of CMD_*).
 *   req_payload    -- pointer to N request payload bytes (may be
 *                     NULL when req_payload_len == 0).
 *   req_payload_len-- length of req_payload.
 *
 * Outputs (caller-supplied):
 *   reply_payload      -- buffer for M reply payload bytes.
 *   reply_payload_cap  -- capacity of reply_payload.
 *   reply_payload_len  -- [out] M (bytes actually written).
 *
 * Return:  STATUS_OK on success; STATUS_NOSUPPORT for unknown
 *          opcodes; STATUS_INVAL on bad payload lengths /
 *          out-of-range args; STATUS_TIMEOUT / STATUS_IO for
 *          downstream bus errors (e.g. DA9292 polling failures).
 */
gd32_bridge_status_t protocol_dispatch(uint8_t cmd,
                                       const uint8_t *req_payload,
                                       size_t req_payload_len,
                                       uint8_t *reply_payload,
                                       size_t reply_payload_cap,
                                       size_t *reply_payload_len);

/* --------------------------------------------------------------- */
/* CRC-16 / CCITT-FALSE -- shared between transports.                */
/* --------------------------------------------------------------- */

uint16_t crc16_ccitt_false(const uint8_t *buf, size_t len);

#endif /* GD32_BRIDGE_PROTOCOL_H */
