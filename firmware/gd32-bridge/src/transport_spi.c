/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32-bridge firmware: SPI-slave transport.
 *
 * Wire framing (see ../docs/gd32-bridge-protocol.md §4):
 *
 *   REQ  : SOF | CMD    | PAYLOAD | CRC(SOF..PAYLOAD)
 *   REPLY: SOF | STATUS | PAYLOAD | CRC(SOF..PAYLOAD)
 *
 * On the V2N module the GD32 sits as the slave on SPI1
 * (PA8/PA9/PA10/PB15).  The slave-side ISR feeds bytes into a
 * staging buffer; once the CRC for a complete request envelope
 * checks out, the ISR calls protocol_dispatch() to compute the
 * reply, stages the reply envelope, and (re-)enables the TX FIFO
 * so the host's next-transaction reads get the reply bytes.
 *
 * THIS FILE is a SCAFFOLD.  The byte-level GigaDevice firmware
 * library hookups (SPI1 init, ISR wiring, TX FIFO management) are
 * marked with TODO blocks; today the file compiles standalone and
 * can be exercised against a unit-test mock that feeds the
 * receive-byte path directly via the spi_slave_rx_byte() entry
 * point.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "protocol.h"
#include "transport.h"

/* Weak default: the stub backend links this no-op so it needs no vendor
 * library.  The gd32 backend's hal/transport_hw_gd32.c overrides it with
 * the real SPI1 slave + CS-EXTI bring-up. */
__attribute__((weak)) void bridge_transport_spi_hw_init(void) { }

/* Maximum SPI envelope = SOF + (CMD or STATUS) + PAYLOAD + CRC. */
#define SPI_MAX_FRAME_BYTES (1u + 1u + GD32_BRIDGE_MAX_PAYLOAD_BYTES + 2u)

/* Receive-side staging buffer (filled byte-by-byte by the ISR). */
static uint8_t  spi_rx_buf[SPI_MAX_FRAME_BYTES];
static size_t   spi_rx_len;

/* Reply staging buffer.  When the host completes the request
 * transaction (CS de-asserted), the dispatcher result lives here
 * ready for the host's follow-up read transaction.  Length is
 * encoded as the absolute byte count to be clocked back. */
static uint8_t  spi_tx_buf[SPI_MAX_FRAME_BYTES];
static size_t   spi_tx_len;
static size_t   spi_tx_cursor;

static void stage_reply(uint8_t status, const uint8_t *payload, size_t payload_len)
{
    spi_tx_buf[0] = GD32_BRIDGE_SOF;
    spi_tx_buf[1] = status;
    if (payload_len > 0u && payload != NULL) {
        memcpy(&spi_tx_buf[2], payload, payload_len);
    }
    const size_t crc_covered = 2u + payload_len;
    const uint16_t crc       = crc16_ccitt_false(spi_tx_buf, crc_covered);
    spi_tx_buf[crc_covered]      = (uint8_t)(crc & 0xFFu);
    spi_tx_buf[crc_covered + 1u] = (uint8_t)((crc >> 8) & 0xFFu);
    spi_tx_len    = crc_covered + 2u;
    spi_tx_cursor = 0u;
}

/* For early-error replies that don't have a CMD context yet, fall
 * back to STATUS_IO so the host re-syncs gracefully. */
static void stage_error_reply(uint8_t status)
{
    stage_reply(status, NULL, 0u);
}

/* Decode an in-buffer request envelope; on success, dispatch and
 * stage the reply.  Called once the byte stream looks complete --
 * see spi_slave_cs_high() which fires on CS de-assert. */
static void decode_and_dispatch(void)
{
    /* Minimum envelope: SOF + CMD + 0-byte payload + CRC = 4 bytes. */
    if (spi_rx_len < 4u) { stage_error_reply(STATUS_IO); return; }
    if (spi_rx_buf[0] != GD32_BRIDGE_SOF) { stage_error_reply(STATUS_IO); return; }

    const size_t payload_len   = spi_rx_len - 4u; /* SOF + CMD + .. + CRC(2) */
    const uint16_t got_crc     = (uint16_t)spi_rx_buf[2u + payload_len]
                               | (uint16_t)spi_rx_buf[2u + payload_len + 1u] << 8;
    const uint16_t expect_crc  = crc16_ccitt_false(spi_rx_buf, 2u + payload_len);
    if (got_crc != expect_crc) { stage_error_reply(STATUS_IO); return; }

    const uint8_t cmd          = spi_rx_buf[1];
    uint8_t       reply_pl[GD32_BRIDGE_MAX_PAYLOAD_BYTES];
    size_t        reply_pl_len = 0u;
    const gd32_bridge_status_t st = protocol_dispatch(cmd,
                                                      payload_len > 0u ? &spi_rx_buf[2] : NULL,
                                                      payload_len,
                                                      reply_pl, sizeof(reply_pl),
                                                      &reply_pl_len);
    stage_reply((uint8_t)st, reply_pl, reply_pl_len);
}

/* --------------------------------------------------------------- */
/* ISR hooks (testable; HW-side wiring lives in the GigaDevice HAL)  */
/* --------------------------------------------------------------- */

/* Call on CS falling-edge.  Resets the RX staging buffer. */
void spi_slave_cs_low(void)
{
    spi_rx_len = 0u;
}

/* Call once per received byte (per the GD32 SPI ISR). */
void spi_slave_rx_byte(uint8_t b)
{
    if (spi_rx_len < sizeof(spi_rx_buf)) {
        spi_rx_buf[spi_rx_len++] = b;
    }
    /* Else: silently drop -- the CRC check at CS-high will fail
     * since the trailing CRC bytes never landed in the buffer. */
}

/* Call on CS rising-edge: signals end of request envelope. */
void spi_slave_cs_high(void)
{
    decode_and_dispatch();
}

/* Called by the SPI TX FIFO-empty ISR when the host clocks bytes
 * during the reply transaction.  Returns the next byte to clock out;
 * after the reply is exhausted, returns 0xFF so the host sees the
 * idle pattern. */
uint8_t spi_slave_tx_next_byte(void)
{
    if (spi_tx_cursor < spi_tx_len) {
        return spi_tx_buf[spi_tx_cursor++];
    }
    return 0xFFu;
}

void transport_spi_init(void)
{
    spi_rx_len    = 0u;
    spi_tx_len    = 0u;
    spi_tx_cursor = 0u;
    /* SPI1 slave + CS-EXTI bring-up lives in the gd32 HAL backend
     * (hal/transport_hw_gd32.c); the stub backend's weak no-op keeps
     * this hardware-free for host-side protocol tests. */
    bridge_transport_spi_hw_init();
}
