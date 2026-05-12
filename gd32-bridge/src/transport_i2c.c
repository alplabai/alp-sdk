/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32-bridge firmware: I2C-slave transport.
 *
 * Wire framing (see ../docs/gd32-bridge-protocol.md §5):
 *
 *   WRITE: [S+ADDR+W] reg=0x00 CMD PAYLOAD CRC(CMD..PAYLOAD)
 *   READ : [S+ADDR+R] STATUS  PAYLOAD CRC(STATUS..PAYLOAD)
 *
 * On the V2N module the GD32 sits as a slave on I2C0 (PA15/PB9) at
 * 7-bit address GD32_BRIDGE_DEFAULT_I2C_ADDR (0x70 by default).
 * The slave-side ISR accumulates bytes during the write phase;
 * when the host issues a repeated-start read, the ISR clock-stretches
 * (holds SCL low) just long enough to run protocol_dispatch() before
 * clocking out the reply bytes.
 *
 * THIS FILE is a SCAFFOLD.  The byte-level GigaDevice firmware
 * library hookups (I2C0 init, slave-address ACK, clock-stretching)
 * are marked TODO; today the file compiles standalone and exercises
 * the protocol-dispatch path via the i2c_slave_rx_byte() /
 * i2c_slave_repeated_start() entry points.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "protocol.h"

#define I2C_MAX_WRITE_BYTES (1u /* reg */ + 1u /* CMD */ + GD32_BRIDGE_MAX_PAYLOAD_BYTES + 2u /* CRC */)
#define I2C_MAX_READ_BYTES  (1u /* STATUS */ + GD32_BRIDGE_MAX_PAYLOAD_BYTES + 2u /* CRC */)

static uint8_t i2c_rx_buf[I2C_MAX_WRITE_BYTES];
static size_t  i2c_rx_len;

static uint8_t i2c_tx_buf[I2C_MAX_READ_BYTES];
static size_t  i2c_tx_len;
static size_t  i2c_tx_cursor;

/* "No pending command since last START" sentinel -- see §5 of the
 * protocol doc.  Set when a read transaction comes in before any
 * matching write completed. */
static bool    pending_reply_valid;

static void stage_no_pending(void)
{
    i2c_tx_buf[0] = STATUS_NO_PENDING;
    /* Empty payload; CRC covers just the status byte. */
    const uint16_t crc = crc16_ccitt_false(i2c_tx_buf, 1u);
    i2c_tx_buf[1] = (uint8_t)(crc & 0xFFu);
    i2c_tx_buf[2] = (uint8_t)((crc >> 8) & 0xFFu);
    i2c_tx_len    = 3u;
    i2c_tx_cursor = 0u;
}

static void stage_reply(uint8_t status, const uint8_t *payload, size_t payload_len)
{
    i2c_tx_buf[0] = status;
    if (payload_len > 0u && payload != NULL) {
        memcpy(&i2c_tx_buf[1], payload, payload_len);
    }
    const size_t crc_covered = 1u + payload_len;
    const uint16_t crc       = crc16_ccitt_false(i2c_tx_buf, crc_covered);
    i2c_tx_buf[crc_covered]      = (uint8_t)(crc & 0xFFu);
    i2c_tx_buf[crc_covered + 1u] = (uint8_t)((crc >> 8) & 0xFFu);
    i2c_tx_len    = crc_covered + 2u;
    i2c_tx_cursor = 0u;
}

/* --------------------------------------------------------------- */
/* ISR hooks (testable; HW-side wiring lives in the GigaDevice HAL)  */
/* --------------------------------------------------------------- */

/* Call on START + addressed-write: resets the RX staging buffer. */
void i2c_slave_write_start(void)
{
    i2c_rx_len = 0u;
    pending_reply_valid = false;
}

/* Call per received byte during the write phase. */
void i2c_slave_rx_byte(uint8_t b)
{
    if (i2c_rx_len < sizeof(i2c_rx_buf)) {
        i2c_rx_buf[i2c_rx_len++] = b;
    }
    /* Else: overrun.  The CRC validation at end-of-write will fail
     * because the trailing bytes never landed. */
}

/* Call on STOP or repeated-START after a write transaction: validates
 * the buffered request and stages the reply.  Returns true on success
 * (reply staged), false on framing/CRC failure (NO_PENDING sentinel
 * staged in its place so the next read still gets a valid envelope). */
bool i2c_slave_write_end(void)
{
    /* Smallest valid envelope: reg(1) + cmd(1) + 0-byte payload + crc(2). */
    if (i2c_rx_len < 4u || i2c_rx_buf[0] != GD32_BRIDGE_I2C_REG_CMD) {
        stage_no_pending();
        return false;
    }
    const size_t payload_len   = i2c_rx_len - 4u; /* reg + cmd + ... + crc(2) */
    const uint16_t got_crc     = (uint16_t)i2c_rx_buf[2u + payload_len]
                               | (uint16_t)i2c_rx_buf[2u + payload_len + 1u] << 8;
    /* CRC covers CMD..PAYLOAD (not the leading reg byte). */
    const uint16_t expect_crc  = crc16_ccitt_false(&i2c_rx_buf[1], 1u + payload_len);
    if (got_crc != expect_crc) {
        stage_no_pending();
        return false;
    }

    uint8_t        reply_pl[GD32_BRIDGE_MAX_PAYLOAD_BYTES];
    size_t         reply_pl_len = 0u;
    const gd32_bridge_status_t st = protocol_dispatch(i2c_rx_buf[1],
                                                      payload_len > 0u ? &i2c_rx_buf[2] : NULL,
                                                      payload_len,
                                                      reply_pl, sizeof(reply_pl),
                                                      &reply_pl_len);
    stage_reply((uint8_t)st, reply_pl, reply_pl_len);
    pending_reply_valid = true;
    return true;
}

/* Call on addressed-read: returns the next byte to clock out.  When
 * the reply is exhausted, returns 0xFF as an idle pattern. */
uint8_t i2c_slave_tx_next_byte(void)
{
    if (!pending_reply_valid && i2c_tx_cursor == 0u) {
        /* Read before any matching write since the last START. */
        stage_no_pending();
    }
    if (i2c_tx_cursor < i2c_tx_len) {
        return i2c_tx_buf[i2c_tx_cursor++];
    }
    return 0xFFu;
}

void transport_i2c_init(void)
{
    i2c_rx_len          = 0u;
    i2c_tx_len          = 0u;
    i2c_tx_cursor       = 0u;
    pending_reply_valid = false;
    /* TODO: GigaDevice firmware library init for I2C0 slave mode on
     *       PA15/PB9, slave address GD32_BRIDGE_DEFAULT_I2C_ADDR.
     *       Hook the START/STOP/byte-received/byte-transmit interrupts
     *       to the i2c_slave_* entry points above. */
}
