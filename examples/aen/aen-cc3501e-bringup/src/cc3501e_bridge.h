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
 * NOT ADOPTED -- 25 MHz is silicon-UNSTABLE on this SoM.  The transfer desyncs
 * partway through a bulk read (NET first-miss at 116-149 kB) and no rx-delay
 * value clears it: rx-delay 0 fails outright, 2 passes 2 runs of 3, 4 fails
 * again, so the MISO capture is simply marginal at a 40 ns bit period over these
 * traces.  14.29 MHz (70 ns) is the validated rate.  Revisit with a scope on
 * MISO at 25 MHz before raising it.
 *
 * It would not have bought much anyway -- silicon-measured
 * 2026-08-24: 682 kB/s at 25 MHz vs 678 kB/s at 14.29 MHz on PIO, 704 vs 701 on
 * DMA, because wire time is only ~554 us of a ~2459 us transaction and the rest
 * is per-frame protocol cost on both ends.  It is worth taking anyway: each
 * transfer occupies the bus for ~44% less time, which is CPU and bus budget
 * handed back to everything else on the SoM.
 *
 * The 14.29 MHz predecessor was scope-confirmed at 14.20-14.26 MHz, which is
 * what validates the 200 MHz functional-clock figure the divider assumes.
 * RX_SAMPLE_DLY was tuned at 14.3 MHz; 25 MHz samples clean on
 * e1m-aen-evk-01 (soak ping_fail=0), but a board with longer traces should
 * re-check MISO capture before adopting it. */
#define CC3501E_BRIDGE_SPI_FREQ_HZ 14000000u
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
