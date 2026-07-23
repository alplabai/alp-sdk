/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cc3501e.h
 * @brief Alif-side host driver for the on-module TI CC3501E
 *
 * @par Verification status: [BENCH-VERIFIED] -- silicon-validated radio + GPIO
 *   coprocessor on E1M-AEN801 (Alif Ensemble E8, M55) in v0.8.0.
 *   Core SS0 link, Wi-Fi scan, Wi-Fi STA connect, BLE scan, and GPIO proxy
 *   validated on real silicon (Wi-Fi + BLE not yet concurrent -- conf-gated,
 *   not a code limit); warm-programmed and shipped on two boards (FIB v0.0.207).
 *        Wi-Fi 6 + BLE 5.4 coprocessor.
 *
 * Wraps the inter-chip SPI1 host-control protocol defined in
 * `<alp/protocol/cc3501e.h>` with a synchronous request/response
 * call shape plus an async-event callback.  Sits below
 * `<alp/iot.h>` and `<alp/ble.h>` -- those public surfaces
 * dispatch through this driver when the active SoM is an
 * E1M-AEN family member.
 *
 * This is a thin AGGREGATE umbrella: the declarations live in the
 * per-subsystem subheaders below (under `alp/chips/cc3501e/`), split by
 * subsystem for reviewability.  Including this umbrella pulls in every
 * subsystem, so existing `#include <alp/chips/cc3501e.h>` call sites keep
 * working unchanged; new code that only needs one subsystem may include
 * the matching subheader directly instead.
 *
 * Lifecycle:
 *
 * @code
 *   alp_spi_t *bus = alp_spi_open(&(alp_spi_config_t){
 *       .bus_id = ALP_E1M_SPI1,   // inter-chip SPI bus
 *       .freq_hz = 14000000,
 *       .mode = ALP_SPI_MODE_0,
 *       .bits_per_word = 8,
 *   });
 *   cc3501e_t fw;
 *   cc3501e_init(&fw, bus);
 *   cc3501e_reset(&fw);            // pulses E_WIFI.NRST + WIFI.EN
 *   uint16_t version = 0;
 *   cc3501e_get_version(&fw, &version);
 *   if (version != ALP_CC3501E_PROTOCOL_VERSION) { ... }
 * @endcode
 *
 * The Alif drives `WIFI.EN` (P15_5) and `E_WIFI.NRST`
 * (P15_1_FLEX) through alp_gpio_*; the driver reads those pin
 * IDs from the studio-supplied config or falls back to
 * compile-time defaults baked from
 * `metadata/e1m_modules/aen/inter-chip.tsv`.
 */

#ifndef ALP_CHIPS_CC3501E_H
#define ALP_CHIPS_CC3501E_H

#include "alp/chips/cc3501e/core.h"
#include "alp/chips/cc3501e/diag.h"
#include "alp/chips/cc3501e/wifi.h"
#include "alp/chips/cc3501e/sockets.h"
#include "alp/chips/cc3501e/ble.h"
#include "alp/chips/cc3501e/gpio.h"
#include "alp/chips/cc3501e/camera.h"
#include "alp/chips/cc3501e/power.h"
#include "alp/chips/cc3501e/ota.h"
#include "alp/chips/cc3501e/events.h"

#endif /* ALP_CHIPS_CC3501E_H */
