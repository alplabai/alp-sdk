/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * One-call bring-up of the E1M-AEN SoM's CC3501E Wi-Fi 6 / BLE coprocessor
 * over the inter-chip SPI bridge -- copied verbatim from
 * examples/aen/aen-cc3501e-gpio/src/cc3501e_bridge.h.
 *
 * BENCH-TEST SCOPE: this app needs the bridge for exactly ONE reason -- to
 * drive the I2S0 mux SELECT + ENABLE (E1M IO13 -> CC3501E GPIO_13, E1M IO8
 * -> CC3501E GPIO_30) before touching i2s3, on the physically reworked
 * e1m-aen-evk-03 bench board (U46 74LVC157 -> 74LV3257). See the board
 * overlay and src/main.c for the full rationale; this file itself is
 * unmodified SoM bring-up boilerplate, not a mux-specific detail.
 *
 * The CC3501E is part of the SoM (module U4 = BDE-BW35N): the application
 * does NOT touch the raw SPI bus or the WIFI_EN / nRESET control pins. It
 * calls cc3501e_bridge_bringup() once, gets a ready @ref cc3501e_t, and from
 * there uses the portable surfaces:
 *   - cc3501e_* (chips/cc3501e)            -- MAC / Wi-Fi / BLE / GPIO-proxy / OTA
 *   - alp_gpio_open(ALP_E1M_GPIO_IOxx)         -- proxied E1M IOs (when the proxy is built)
 */

#ifndef CC3501E_BRIDGE_H
#define CC3501E_BRIDGE_H

#include <alp/peripheral.h>    /* alp_status_t */
#include <alp/chips/cc3501e.h> /* cc3501e_t */

/* ---- E1M-AEN SoM bridge defaults (override per board variant) ---------------- */

#ifndef CC3501E_BRIDGE_SPI_BUS_ID
#define CC3501E_BRIDGE_SPI_BUS_ID 1u
#endif
#ifndef CC3501E_BRIDGE_SPI_FREQ_HZ
/* 25 MHz = 200 MHz SSI functional clock / 8 -- see
 * examples/aen/aen-cc3501e-gpio/src/cc3501e_bridge.h for the full derivation
 * (silicon-measured on E1M-AEN803 serial 2026W36-0002). */
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
/* OPTIONAL host-IRQ/READY input -- not wired by this app's overlay (2-entry
 * alp_pins array), so alp_gpio_open() on this index returns NULL and
 * cc3501e_request() falls back to its fixed-gap delay. */
#ifndef CC3501E_BRIDGE_PIN_READY
#define CC3501E_BRIDGE_PIN_READY 2u
#endif

/* DW SSI SPI1 base (0x48104000) + RX_SAMPLE_DLY to run the bridge SCLK above
 * 1 MHz. 4 is silicon-measured at the 25 MHz working point -- see
 * examples/aen/aen-cc3501e-gpio/src/cc3501e_bridge.h for the sweep data. */
#ifndef CC3501E_BRIDGE_SPI1_BASE
#define CC3501E_BRIDGE_SPI1_BASE 0x48104000u
#endif
#ifndef CC3501E_BRIDGE_RX_SAMPLE_DLY
#define CC3501E_BRIDGE_RX_SAMPLE_DLY 4u
#endif

/**
 * @brief Bring up the SoM's CC3501E coprocessor over the inter-chip bridge.
 *
 * Opens the hardware-SS0 bridge SPI + the WIFI_EN / nRESET control pins,
 * binds them to @p fw, attaches the GPIO proxy (when
 * CONFIG_ALP_SDK_GPIO_CC3501E_PROXY is built), and runs the power + reset
 * sequence. Blocks ~900 ms for the boot budget; leaves WIFI_EN HIGH.
 *
 * @param fw  Caller-owned handle, populated on success. Use it with cc3501e_*.
 * @return ALP_OK with @p fw ready; ALP_ERR_NOT_PRESENT_ON_THIS_SOC if the SPI
 *         bus / control pins are absent (check the board overlay); otherwise
 *         the reset sequence status. On any failure @p fw is left un-bound.
 */
alp_status_t cc3501e_bridge_bringup(cc3501e_t *fw);

#endif /* CC3501E_BRIDGE_H */
