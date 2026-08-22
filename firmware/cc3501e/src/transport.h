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
 * backend (the weak default below), and a no-op in OTA update mode while a handle
 * exists -- see bridge_transport_spi_polled() below. */
void bridge_transport_spi_hw_reinit(void);

/* True while the SPI slave has NO handle -- every SPI_open retry failed.  The
 * tick's desync / arm-failure self-heals both key off counters that only move
 * when a handle exists, so this is the only way that state is observable
 * (#1610).  Weak-stubbed false on backends without a real SPI slave. */
bool bridge_transport_spi_is_dead(void);

/* Stand the bridge slave DOWN (cancel the armed transfer + SPI_close, releasing its
 * DMA) for the DURATION of a radio op that re-arbitrates the shared HIF DMA -- BLE
 * controller enable, where the NWP HIF handshake hangs if the bridge DMA contends.
 * Call BEFORE the op; pair with bridge_transport_spi_hw_reinit() AFTER.  No-op on the
 * stub backend (weak default below).
 *
 * All three are also NO-OPS in OTA update mode: nothing is ever torn down there, and
 * SPI_close/SPI_transferCancel against a polled slave deadlock or hang.  Callers do
 * NOT need their own bridge_transport_spi_polled() guard -- see below. */
void bridge_transport_spi_hw_suspend(void);
void bridge_transport_spi_hw_release(void);
void bridge_transport_spi_hw_quiesce(bool on);

/* ---- OTA update mode (polled bridge) --------------------------- */
/* A callback/DMA SPI_open PERMANENTLY prevents psa_fwu_start() and
 * psa_fwu_write() from returning (bench-proven, silicon 2026-08-21), and
 * SPI_close() does not undo the claim.  So OTA is run in a dedicated BOOT MODE
 * in which the bridge slave is opened POLLED (SPI_MODE_BLOCKING /
 * SPI_WAIT_FOREVER) and nothing else runs: the device services the bridge
 * frame-by-frame and pumps the OTA flush at frame boundaries.  See the ti
 * backend (hal/ti/transport_hw_ti_spi.c) for the mechanism and its limits. */

/**
 * @brief True for the WHOLE of this boot: the bridge SPI is open POLLED and the
 *        device is running in OTA update mode.
 *
 * Idempotent and self-initialising -- the FIRST call READS AND CLEARS the
 * persisted flag and latches the answer, so it is safe (and required) to call
 * before transport_spi_init().  Clearing on read is what stops a wedged
 * update-mode boot from becoming a permanent update-mode boot loop.
 *
 * @return true in OTA update mode; false in the normal DMA/callback bridge.
 *         Weak default (backends without a real SPI slave): false.
 */
bool bridge_transport_spi_polled(void);

/**
 * @brief Arm the persisted flag so the NEXT boot comes up in OTA update mode.
 *
 * Survives the device's own warm reset (NVIC_SystemReset), NOT a host
 * WIFI_EN/nRESET cold cycle -- that asymmetry is the host's escape hatch.
 * Weak default: no-op.
 */
void bridge_transport_spi_request_polled_boot(void);

/**
 * @brief Disarm the persisted flag so the NEXT boot comes up in the normal
 *        DMA/callback bridge.
 *
 * Must be called before a successful OTA FINISH/PROMOTE arms its swap reboot,
 * or the freshly-swapped firmware comes up deaf to the radio.  Weak default:
 * no-op.
 */
void bridge_transport_spi_clear_polled_boot(void);

/**
 * @brief Bench proof channel for the update-mode boot flag, reported through
 *        GET_DIAG_INFO reserved[2].
 *
 * bit7 = this boot is polled/update mode; bits[6:0] = warm-boot counter mod
 * 128.  The CC3501E has no UART on the debug probe, so this is the ONLY way to
 * prove the persisted flag survived the warm reset: if the counter never leaves
 * 1 across warm resets, RAM did not survive and the whole design is dead.
 *
 * @return the packed mode + counter byte.  Weak default: 0.
 */
uint8_t bridge_transport_spi_boot_mark(void);

/**
 * @brief Run ONE complete polled bridge frame (all four SS0-framed phases).
 *
 * Blocks inside SPI_transfer until the host clocks each phase, so it must own
 * its calling loop -- only the update-mode loop may call it.  Returns at the
 * frame boundary, which is the safe point for flash work (nothing in flight).
 * In polled mode this function is the SOLE owner of the READY line.
 *
 * @return true if at least one phase completed; false when nothing is armed.
 */
bool    bridge_transport_spi_poll_service(void);
uint8_t bridge_transport_spi_phase(void);

/* Bridge READY/host-IRQ flow-control (the READY GPIO noted below).  Weak no-ops
 * in worker.c; the ti backend (cc3501e_hw_ti.c) drives a real GPIO -- CC35
 * GPIO17 / E1M IO16 -> Alif P2_6.  busy() = LOW (a radio op is running, the
 * SPI-slave DMA is dead, host must not clock); ready() = HIGH (slave re-armed).
 * The worker drives busy() before every blocking radio op and ready() after the
 * post-op re-arm, so the host master never clocks into a dead slave. */
void cc3501e_bridge_busy(void);
void cc3501e_bridge_ready(void);

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
