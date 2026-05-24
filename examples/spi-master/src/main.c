/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * spi-master -- discrete SPI master.  Send a known byte pattern
 * out MOSI, log whatever clocks back on MISO.
 *
 * SPI is full-duplex: every byte you send out also clocks a byte
 * back from the slave.  This example demonstrates three usage
 * shapes layered on the same bus handle:
 *
 *   1. alp_spi_write()       -- TX only, MISO discarded.  The
 *                               most common pattern for "send a
 *                               command to a peripheral".
 *   2. alp_spi_transceive()  -- TX + RX simultaneously into
 *                               separate buffers.  The pattern
 *                               for "send a register address,
 *                               read its value back" on chips
 *                               that don't have a separate
 *                               command phase.
 *   3. alp_spi_read()        -- RX only, MOSI sends 0xFF.  The
 *                               pattern for "drain N bytes from
 *                               a slave that's already in
 *                               streaming mode".
 *
 * Contrasts with examples/spi-loopback which exercises both ends
 * on the same chip (single-bus self-test).  This example is the
 * production starting point: replace the byte pattern with your
 * chip's command + register sequence.
 *
 * What success looks like:
 *
 *   [spi-master] open E1M_SPI1 @ 1 MHz mode 0
 *   [spi-master] write -> 0
 *   [spi-master] transceive -> 0  rx={de,ad,be,ef}
 *   [spi-master] read -> 0  rx={..}
 *   [spi-master] done
 *
 * On native_sim the spi-emul controller echoes 0x00 back on every
 * read (no slave is registered), but the open / write / read /
 * transceive / close path runs and the harness latches `done`.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp/e1m_pinout.h"

/* Sentinel meaning "the SPI controller manages CS internally" --
 * the SDK's controller-managed-CS path picks the right pin from
 * the devicetree chip-select array.  Many SoCs handle CS this way;
 * for multi-slave buses you'd assign a discrete GPIO here instead.
 *
 * Defined locally to keep the example self-contained; future
 * <alp/peripheral.h> revisions may export this as a named constant. */
#define ALP_SPI_NO_CS 0xFFFFFFFFu

/* Test pattern.
 *
 *   0xAA = 0b10101010 -- alternating bits, every transition exercises
 *                         the line.  Stuck-at-0 / stuck-at-1 faults
 *                         show immediately because the bit string is
 *                         non-constant.
 *   0x55 = 0b01010101 -- the inverse; combined with 0xAA it provides
 *                         full edge coverage.
 *   0xDE 0xAD         -- non-symmetric pair -- highlights endian or
 *                         byte-order bugs in the slave.
 *
 * Real apps replace this with the chip's command sequence (e.g.
 * 0x9F for SPI-flash read-id, 0x03 + 24-bit address for read,
 * etc.). */
static const uint8_t TX_PATTERN[] = { 0xAA, 0x55, 0xDE, 0xAD };

int                  main(void)
{
    printf("[spi-master] open E1M_SPI1 @ 1 MHz mode 0\n");

    /* Open the SPI bus.  Configuration knobs in order:
     *
     *   bus_id          -- portable instance, E1M_SPI1 here.
     *   freq_hz         -- 1 MHz is the conservative default.
     *                       Bump after confirming the slave's max
     *                       clock and short wires.
     *   mode            -- CPOL/CPHA combination.  MODE_0 (idle-
     *                       low clock, sample on rising edge) is
     *                       what most modern chips expect.
     *   bits_per_word   -- 8 is universal; 16/32 if your slave
     *                       genuinely wants larger words.
     *   cs_pin_id       -- ALP_SPI_NO_CS = let the controller
     *                       manage it; otherwise the discrete
     *                       GPIO that drives /CS. */
    alp_spi_t *bus = alp_spi_open(&(alp_spi_config_t){
        .bus_id        = E1M_SPI1,
        .freq_hz       = 1000000,
        .mode          = ALP_SPI_MODE_0,
        .bits_per_word = 8,
        .cs_pin_id     = ALP_SPI_NO_CS,
    });
    if (bus == NULL) {
        /* Likely causes:
         *   * No `alp-spi0` alias on this build (board overlay
         *     missing or wrong).
         *   * Requested freq_hz exceeds the controller's max
         *     (returns NOSUPPORT via alp_last_error).
         *   * On native_sim without the emul overlay we ship,
         *     the alias resolves to NULL. */
        printf("[spi-master] open failed: alp_last_error=%d\n", (int)alp_last_error());
        printf("[spi-master] done\n");
        return 0;
    }

    /* -------- 1.  Half-duplex write (TX only). --------
     *
     * Use this when you're sending a command + don't care what
     * the slave puts on MISO (it's usually high-Z or garbage
     * during the command phase). */
    alp_status_t s = alp_spi_write(bus, TX_PATTERN, sizeof TX_PATTERN);
    printf("[spi-master] write -> %d\n", (int)s);

    /* -------- 2.  Full-duplex transceive (TX + RX). --------
     *
     * Every byte we send out also clocks a byte in.  rx_buf
     * receives whatever the slave drives on MISO during each
     * clock cycle.  This is the canonical "read register N"
     * pattern when the slave responds in-band (no separate
     * command/response phase). */
    uint8_t rx_buf[sizeof TX_PATTERN] = { 0 };
    s = alp_spi_transceive(bus, TX_PATTERN, rx_buf, sizeof TX_PATTERN);
    printf("[spi-master] transceive -> %d  rx={%02x %02x %02x %02x}\n", (int)s, rx_buf[0],
           rx_buf[1], rx_buf[2], rx_buf[3]);

    /* -------- 3.  Half-duplex read (RX only). --------
     *
     * The wrapper clocks the bus while sending 0xFF (the
     * convention for "I don't care what you see on MOSI") and
     * collects MISO into our buffer.  Used when the slave is
     * already streaming -- e.g. an ADC in continuous-conversion
     * mode, or a NAND-flash page read after the address has
     * already been clocked in. */
    uint8_t in_buf[sizeof TX_PATTERN] = { 0 };
    s                                 = alp_spi_read(bus, in_buf, sizeof in_buf);
    printf("[spi-master] read -> %d  rx={%02x %02x %02x %02x}\n", (int)s, in_buf[0], in_buf[1],
           in_buf[2], in_buf[3]);

    /* Clean shutdown.  CS line returns to its idle state (high
     * for active-low CS); SCK + MOSI go to whatever the
     * controller's idle line state is.  The bus handle's slot
     * goes back to the SDK pool. */
    alp_spi_close(bus);
    printf("[spi-master] done\n");
    return 0;
}
