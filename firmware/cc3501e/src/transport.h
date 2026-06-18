/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: transport seam declarations.
 *
 * The host-control link between the Alif and the CC3501E is
 * CUSTOMER-SELECTABLE:
 *
 *   - SPI0 slave (the CC3501E's SPI0; the Alif master is SPI1)  -- the
 *     DEFAULT and always-available baseline.  Low
 *     pin count, never conflicts with anything else on the SoM.
 *   - SDIO slave  -- OPTIONAL, higher throughput for Wi-Fi data.  The
 *     Alif Ensemble has a SINGLE SDIO controller, shared at board
 *     level (via mux resistors) with the micro-SD slot, so SDIO is
 *     available to the CC3501E only on boards that do NOT populate an
 *     SD card.  When an SD card is used, SDIO is blocked and the link
 *     falls back to SPI.  See docs/cc3501e-bridge.md "Inter-chip
 *     wiring" + "Selectable host-control transport".
 *
 * Exactly one control transport is active per build/boot
 * (CC3501E_CONTROL_TRANSPORT, default spi).  Whichever is active, it
 * feeds the SAME protocol_dispatch() -- one framing format, one
 * command set, one set of reply codes; only the byte-level transport
 * differs (the gd32-bridge SPI + I2C model).
 *
 * The transport sources (transport_spi.c / transport_sdio.c) are
 * SILICON-FREE: framing, staging and the protocol_dispatch() hand-off
 * only.  The byte-level peripheral wiring (TI SimpleLink / driverlib
 * slave init + ISRs) lives in the ti backend under hal/ti/ and drives
 * the seams below.  The stub backend leaves the weak bring-up hooks as
 * no-ops so a host unit test can feed the same seams directly.
 */

#ifndef CC3501E_BRIDGE_TRANSPORT_H
#define CC3501E_BRIDGE_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

/* ---- lifecycle (called from main.c) ---------------------------- */
void transport_spi_init(void);
void transport_sdio_init(void);

/* ---- weak HW bring-up hooks (strong impl in hal/ti/) ----------- */
void bridge_transport_spi_hw_init(void);
void bridge_transport_sdio_hw_init(void);

/* Re-establish the armed SPI slave from a clean state (SPI_close + SPI_open +
 * re-arm the first request header).  The ti backend (hal/ti/transport_hw_ti_spi.c)
 * implements it; bridge_transport_spi_hw_init() is the first-time path through
 * the same code.  Called by the ti HAL after Wlan_Start() so the bridge slave
 * re-claims the host-DMA channel->peripheral mux the Wi-Fi HIF stole when it
 * brought the radio up (the Wlan_Start DMA-coexistence fix).  No-op on the stub
 * backend (the weak default below). */
void bridge_transport_spi_hw_reinit(void);

/* ---- SPI slave seams (defined in transport_spi.c) -------------- */
/* The HW backend (or a host test) drives one request transaction as
 * cs_low -> rx_byte* -> cs_high, then clocks the staged reply back via
 * tx_next_byte while tx_pending() is true.  Request and reply ride
 * SEPARATE transactions; the firmware signals "reply ready" to the
 * host out-of-band (a READY GPIO -- the v0.x bring-up handshake noted
 * in chips/cc3501e/cc3501e.c). */
void    spi_slave_cs_low(void);       /* CS falling edge: reset RX staging    */
void    spi_slave_rx_byte(uint8_t b); /* one received request byte            */
void    spi_slave_cs_high(void);      /* CS rising edge: decode + dispatch     */
uint8_t spi_slave_tx_next_byte(void); /* next staged reply byte; 0xFF when idle */
bool    spi_slave_tx_pending(void);   /* true while a staged reply has bytes left */

/* ---- SDIO slave seams (defined in transport_sdio.c) ------------ */
/* SDIO carries the same request/reply frames inside its data blocks.
 * The seam is frame-oriented (SDIO delivers whole blocks, not a byte
 * stream): the HW backend hands a complete received request frame to
 * sdio_slave_on_request() and clocks back the staged reply that
 * sdio_slave_reply()/sdio_slave_reply_len() expose. */
void           sdio_slave_on_request(const uint8_t *frame, uint16_t len);
const uint8_t *sdio_slave_reply(void);
uint16_t       sdio_slave_reply_len(void);

#endif /* CC3501E_BRIDGE_TRANSPORT_H */
