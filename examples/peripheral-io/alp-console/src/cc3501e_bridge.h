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
 *   - alp_gpio_open(E1M_GPIO_IOxx)         -- proxied E1M IOs (when the proxy is built)
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
/* 8 MHz: first bench-sweep step up from the 1 MHz cold-boot floor toward the
 * CC3501E's 15 MHz ceiling.  At >1 MHz the dwc-ssi master samples the incoming
 * MISO bit too early over the on-SoM traces (the earlier bare 8 MHz attempt
 * mis-sampled -> cold first-contact failed); this now pairs with the dwc-ssi
 * RX sample delay set via `rx-delay` on the cc3501e_spi node in the board
 * overlay (RX_SAMPLE_DLY reg 0xF0, in ssi_clk cycles).  GOAL: 15 MHz -- raise this
 * to 15000000u ONLY after 8 MHz is bench-clean AND the overlay `rx-delay` is
 * retuned for 15 MHz (see the overlay comment); MISO mis-sample shows up as a
 * -5 desync / garbage reply headers on the link.
 * (Payload-request reliability is handled by the inter-phase settle in
 * cc3501e_request -- CC3501E_PHASE_SETTLE_US -- not the clock.) */
#define CC3501E_BRIDGE_SPI_FREQ_HZ \
	1000000u /* 1 MHz: the bench-validated link rate; higher rates are a future throughput sweep */
#endif

/* CC3501E control pins on the Alif LP-GPIO island (NOT E1M edge pads):
 * WIFI_EN = supply gate (P15_5), nRESET = reset (P15_1_FLEX). */
#ifndef CC3501E_BRIDGE_PIN_WIFI_EN
#define CC3501E_BRIDGE_PIN_WIFI_EN 0u
#endif
#ifndef CC3501E_BRIDGE_PIN_NRST
#define CC3501E_BRIDGE_PIN_NRST 1u
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
