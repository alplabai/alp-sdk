/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * One-call bring-up of the E1M-AEN SoM's CC3501E Wi-Fi 6 / BLE coprocessor over the
 * inter-chip SPI bridge -- the SoM bring-up TEMPLATE for applications.
 *
 * The CC3501E is part of the SoM (module U4 = BDE-BW35N): the application does NOT
 * touch the raw SPI bus or the WIFI_EN / nRESET control pins.  It calls
 * cc3501e_bridge_bringup() once, gets a ready @ref cc3501e_t, and from there uses the
 * portable surfaces:
 *   - cc3501e_* (chips/cc3501e)            -- MAC / Wi-Fi / BLE / GPIO-proxy / OTA
 *   - alp_gpio_open(ALP_E1M_GPIO_IOxx)         -- proxied E1M IOs (when the proxy is built)
 *
 * To reuse in your own AEN application: copy this pair (cc3501e_bridge.{c,h}) into your
 * app, or call cc3501e_bridge_bringup() directly.  The bus / pins / clock are the
 * E1M-AEN SoM defaults below; a board variant overrides only those macros -- the SoM
 * stays swappable without touching application code.
 */

#ifndef CC3501E_BRIDGE_H
#define CC3501E_BRIDGE_H

#include <alp/peripheral.h>    /* alp_status_t */
#include <alp/chips/cc3501e.h> /* cc3501e_t */

/* ---- E1M-AEN SoM bridge defaults (override per board variant) ---------------- */

/* Inter-chip SPI: Alif = master, CC3501E = slave.  P14_7 is muxed as the
 * dwc-ssi hardware SS0 chip-select; ALP_SPI_NO_CS means "no software GPIO CS"
 * here, so the controller frames each protocol phase.  Mode 0 matches the
 * CC3501E vendor image frameFormat. */
#ifndef CC3501E_BRIDGE_SPI_BUS_ID
#define CC3501E_BRIDGE_SPI_BUS_ID 1u
#endif
#ifndef CC3501E_BRIDGE_SPI_FREQ_HZ
/* 25 MHz = 200 MHz SSI functional clock / 8.
 *
 * The CC3501E peripheral-mode maximum is 30 MHz, NOT the 15 MHz this comment
 * claimed for most of the bring-up: datasheet SWRS343A section 6.14.2.3.3,
 * `fsclk SPI clock frequency, Peripheral Mode, MAX 30 MHz` (Controller Mode is
 * 40 MHz).  Every earlier ceiling argument here was built on the wrong number.
 *
 * 25 MHz is the fastest LEGAL step available: the DW SSI BAUDR divisor must be
 * EVEN, so from 200 MHz the neighbours are /8 = 25 MHz and /6 = 33.3 MHz, and
 * 33.3 is 11% over the part's maximum.  Reaching exactly 30 MHz would require
 * retargeting the SSI functional clock itself (AE822 HWRM section 8.3.5) --
 * ALIF_SPI_CLK is a frequency-only dummy in the dtsi with no divider control,
 * so there is no software knob for it here.
 *
 * REQUIRES rx-delay = <2> on the SPI node (see the board overlay).  At a 40 ns
 * bit period the MISO capture needs the delay; with rx-delay 0 the link fails
 * outright, and 4 is too much.
 *
 * It does not raise throughput -- silicon-measured
 * 2026-08-24: 682 kB/s at 25 MHz vs 678 kB/s at 14.29 MHz on PIO, 704 vs 701 on
 * DMA, because wire time is only ~554 us of a ~2459 us transaction and the rest
 * is per-frame protocol cost on both ends.  It is worth taking anyway: each
 * transfer occupies the bus for ~44% less time, which is CPU and bus budget
 * handed back to everything else on the SoM.
 *
 * The 14.29 MHz predecessor was scope-confirmed at 14.20-14.26 MHz, which is
 * what validates the 200 MHz functional-clock figure the divider assumes.  A
 * board with longer traces should re-check MISO capture before adopting 25 MHz.
 *
 * SEPARATE, PRE-EXISTING DEFECT (do not blame this clock for it): a bulk read
 * intermittently desyncs partway through -- roughly 1 run in 2-3, at anywhere
 * from 61 kB to 212 kB in -- and then never recovers for the caller's whole
 * retry budget.  It reproduces at 14.29 MHz TOO, so it is not a clock artifact.
 * Signature (host-side, request-header phase of CMD_SOCK_RECV 0x23): the
 * slave's in-band idle marker reads [02 00 00 00] once and then [00 00 00 00]
 * forever instead of A5 A5 A5 A5 -- i.e. the slave is stuck mid-reply on a
 * transfer it armed earlier and never completed.  With
 * SPI_TRANSFER_RETURN_PARTIAL deliberately off, a CS deassert does not complete
 * it, and NEITHER slave self-heal fires (g_resync_count does not move -- the
 * slave is not misframing; g_arm_fail_count does not move -- the arm succeeded).
 * Clocking a full idle frame from the host to satisfy the outstanding count was
 * tried and did NOT recover it.  The fix likely belongs on the slave: a
 * stall watchdog that calls bridge_transport_spi_hw_reinit() when an armed
 * transfer has not completed for N ms. */
#define CC3501E_BRIDGE_SPI_FREQ_HZ 25000000u
#endif

/* CC3501E control pins on the Alif LP-GPIO island (NOT E1M edge pads):
 * WIFI_EN = supply gate (P15_5), nRESET = reset (P15_1_FLEX). */
#ifndef CC3501E_BRIDGE_PIN_WIFI_EN
#define CC3501E_BRIDGE_PIN_WIFI_EN 0u
#endif
#ifndef CC3501E_BRIDGE_PIN_NRST
#define CC3501E_BRIDGE_PIN_NRST 1u
#endif
/* OPTIONAL host-IRQ/READY input (CC35 GPIO17 -> Alif P2_6, alp_pins[2]).  When
 * the board wires it, cc3501e_request() gates reply phases on it (HIGH = slave
 * armed) instead of a fixed delay.  Absent -> ready_pin NULL -> legacy gap. */
#ifndef CC3501E_BRIDGE_PIN_READY
#define CC3501E_BRIDGE_PIN_READY 2u
#endif

/* DW SSI SPI1 base (0x48104000) + RX_SAMPLE_DLY to run the bridge SCLK above
 * 1 MHz.  spi_dw never writes RX_SAMPLE_DLY (0xf0) so it defaults to 0 -> the
 * master samples MISO at the SCLK edge, before the on-SoM trace + crossed-data
 * round-trip returns the CC35's bit, so >1 MHz mis-samples.  Setting it delays
 * the capture by N ssi_clk (200 MHz) cycles.  6 is silicon-tuned for ~14.3 MHz on
 * e1m-aen-evk-01 with a WIDE window (4..8 all clean cold+warm).  0 disables it
 * (falls back to 1 MHz).  Re-sweep if the SoM trace lengths change. */
#ifndef CC3501E_BRIDGE_SPI1_BASE
#define CC3501E_BRIDGE_SPI1_BASE 0x48104000u
#endif
#ifndef CC3501E_BRIDGE_RX_SAMPLE_DLY
#define CC3501E_BRIDGE_RX_SAMPLE_DLY 6u
#endif

/**
 * @brief Bring up the SoM's CC3501E coprocessor over the inter-chip bridge.
 *
 * Opens the hardware-SS0 bridge SPI + the WIFI_EN / nRESET control pins, binds
 * them to @p fw, attaches the GPIO proxy (when CONFIG_ALP_SDK_GPIO_CC3501E_PROXY is
 * built), and runs the power + reset sequence (TI SWRU626 + the Puya cold-boot
 * hard-reset workaround).  Blocks ~900 ms for the boot budget; leaves WIFI_EN HIGH.
 *
 * @param fw  Caller-owned handle, populated on success.  Use it with cc3501e_*.
 * @return ALP_OK with @p fw ready; ALP_ERR_NOT_PRESENT_ON_THIS_SOC if the SPI bus /
 *         control pins are absent (check the board overlay); otherwise the reset
 *         sequence status.  On any failure @p fw is left un-bound (do not use it).
 */
alp_status_t cc3501e_bridge_bringup(cc3501e_t *fw);

#endif /* CC3501E_BRIDGE_H */
