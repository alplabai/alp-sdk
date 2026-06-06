/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32-bridge firmware: transport seam declarations.
 *
 * The transport sources (transport_spi.c / transport_i2c.c) are
 * SILICON-FREE: they implement framing, CRC, staging and the
 * protocol_dispatch() hand-off, and expose the byte-level "seam"
 * functions below.  The seams are driven from two directions:
 *
 *   - In hardware-less unit tests, a host harness calls them directly
 *     to exercise the wire protocol with no GD32 dependency.
 *   - On real silicon, the GigaDevice-specific ISRs in
 *     hal/transport_hw_gd32.c (gd32 backend only) call them from the
 *     SPI1 / I2C0 / EXTI interrupt handlers.
 *
 * transport_*_init() resets the staging state and then calls the weak
 * bridge_transport_*_hw_init() hook.  The default hook (defined weak in
 * the transport sources) is a no-op so the stub backend links with no
 * vendor library; the gd32 backend overrides it with the real
 * peripheral bring-up.
 */
#ifndef GD32_BRIDGE_TRANSPORT_H
#define GD32_BRIDGE_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

/* ---- lifecycle (called from main.c) ---------------------------- */
void transport_spi_init(void);
void transport_i2c_init(void);

/* ---- weak HW bring-up hooks (strong impl in transport_hw_gd32.c) - */
void bridge_transport_spi_hw_init(void);
void bridge_transport_i2c_hw_init(void);

/* ---- SPI slave seams (defined in transport_spi.c) -------------- */
void    spi_slave_cs_low(void);       /* CS falling edge: reset RX staging   */
void    spi_slave_rx_byte(uint8_t b); /* one received byte (per SPI RBNE)     */
void    spi_slave_cs_high(void);      /* CS rising edge: decode + dispatch    */
uint8_t spi_slave_tx_next_byte(void); /* next reply byte (per SPI TBE), 0xFF idle */
bool    spi_slave_tx_pending(void);   /* true while staged reply has bytes left   */

/* ---- I2C slave seams (defined in transport_i2c.c) -------------- */
void    i2c_slave_write_start(void);  /* START + addressed write: reset RX    */
void    i2c_slave_rx_byte(uint8_t b); /* one received byte (write phase)      */
bool    i2c_slave_write_end(void);    /* STOP / repeated-START: dispatch+stage */
uint8_t i2c_slave_tx_next_byte(void); /* next reply byte (read phase), 0xFF idle */

#endif /* GD32_BRIDGE_TRANSPORT_H */
