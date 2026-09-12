/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file temperature.h
 * @brief Portable read of the SoM's on-module ambient-temperature sensor.
 *
 * A SoM-resident chip (e.g. TI TMP112 on E1M-AEN, `chips/tmp112`) travels
 * with the module the same way the on-module EEPROM identity does --
 * `alp_hw_info_read()` is boot-time identity, this is polled telemetry, and
 * the two stay separate headers on purpose (see issue #2066: folding this
 * into `<alp/hw_info.h>` would collide with the on-die multi-sensor count a
 * future SoC could add there).
 *
 * One entry point, handle-less and index-less: the on-module manifest
 * schema carries at most one `temperature_sensor` per SoM, so there is
 * nothing to open, hold, or select between.
 *
 * @par Presence is a SoM fact, not a SoC one.
 *      Whether a build has an on-module sensor comes from the SoM preset's
 *      `on_module` device population (e.g. `metadata/e1m_modules/E1M-AEN801.yaml`
 *      declares one; `E1M-NX9101.yaml` declares none) -- never from
 *      `<alp/soc_caps.h>` / `<alp/cap.h>`, which are generated per-SoC and
 *      would wrongly claim the sensor for every SoM sharing that die.
 *
 * @par Today's coverage.
 *      Implemented on the Zephyr AEN backend only, binding the
 *      metadata-emitted `alp-temp0` devicetree alias through the upstream
 *      Zephyr sensor API (gated on `CONFIG_SENSOR` and the alias's DT
 *      status) -- never by opening the bus directly: AEN's BRD_I2C is
 *      standard-mode with no external pull-up, and `alp_i2c_open()` has no
 *      exclusivity check, so a portable open at a different bitrate would
 *      both race an app's own handle and be a real electrical fault on
 *      this board.  Every other build target -- including V2N/V2M, where
 *      no board devicetree carries a tmp112 node and the SoM preset's own
 *      address is still unverified on real silicon -- returns
 *      @ref ALP_ERR_NOSUPPORT until a consumer needs it there.
 *
 * @code
 * int32_t milli_c;
 * alp_status_t s = alp_temperature_read_milli_c(&milli_c);
 * if (s == ALP_OK) {
 *     printk("Temp: %d milli-degC\n", (int)milli_c);
 * }
 * @endcode
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.17 new.  See docs/abi-markers.md for the convention.
 */

#ifndef ALP_TEMPERATURE_H
#define ALP_TEMPERATURE_H

#include <stdint.h>

#include "alp/peripheral.h" /* alp_status_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read the on-module ambient-temperature sensor.
 *
 * Promises units and sign only -- integer milli-degrees Celsius, signed.
 * The fitted part's resolution (e.g. TMP112's 62.5 milli-C/LSB) is an
 * informational fact of that chip, never part of this contract, so a
 * future SoM with a different-resolution part changes nothing a caller
 * can observe except the reading's granularity.
 *
 * @param[out] milli_c  Set to the reading on @ref ALP_OK.  Left untouched
 *                       on any error.
 *
 * @return  @ref ALP_OK on a valid read.
 *          @ref ALP_ERR_INVAL when @p milli_c is NULL.
 *          @ref ALP_ERR_NOSUPPORT when this build has no on-module
 *                                 temperature sensor (e.g. E1M-NX9101, or
 *                                 any non-AEN target today).
 *          @ref ALP_ERR_NOT_READY when a sensor is declared but the
 *                                 device did not come up.
 *          @ref ALP_ERR_IO on a transfer fault while reading.
 */
alp_status_t alp_temperature_read_milli_c(int32_t *milli_c);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_TEMPERATURE_H */
