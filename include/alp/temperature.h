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
 *      declares one; a preset without it declares none) -- never from
 *      `<alp/soc_caps.h>` / `<alp/cap.h>`, which are generated per-SoC and
 *      would wrongly claim the sensor for every SoM sharing that die.
 *
 * @par Die temperature.
 *      alp_temperature_read_die_milli_c() reads the SoC's own junction sensor
 *      through the upstream Zephyr `die-temp0` alias; it is a SoC fact (the
 *      alias comes from the SoC / board devicetree), unlike the SoM-level
 *      ambient part above.
 *      This repository's SoM board trees do not declare the alias; a SoC or
 *      board tree must provide it (upstream or vendor tree), else the call
 *      returns @ref ALP_ERR_NOSUPPORT.
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
 * @par SoC die temperature is a separate entry.
 *      @ref alp_temperature_read_die_milli_c reports the silicon's own
 *      junction temperature (on V2N/V2M, the Linux thermal zones the
 *      on-die TSU units feed).  It is a different physical quantity from
 *      the on-module ambient sensor above and is never folded into
 *      @ref alp_temperature_read_milli_c (issue #2066), so a SoM can
 *      have either, both or neither.
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
 *                                 temperature sensor (any non-AEN
 *                                 target today), or when a
 *                                 sensor is declared but no driver ever
 *                                 bound the device at all.
 *          @ref ALP_ERR_NOT_READY when a sensor is declared and a driver
 *                                 bound the device, but it did not come
 *                                 up (e.g. it NACKs on its bus at boot).
 *          @ref ALP_ERR_IO on a transfer fault while reading.
 */
alp_status_t alp_temperature_read_milli_c(int32_t *milli_c);

/**
 * @brief Read the SoC die (junction) temperature, in milli-degrees C. [ABI-EXPERIMENTAL]
 *
 * Distinct from @ref alp_temperature_read_milli_c -- that is a SoM-level
 * ambient sensor, this is the processor's own thermal sensing, which
 * reads far above ambient under load.  Units and sign match: integer
 * milli-degrees Celsius, signed.  When the SoC has several die sensors
 * the hottest one is reported -- the figure a throttling or protection
 * decision cares about.
 *
 * @par Linux (Yocto) reads the kernel thermal zones.
 *      It reads `/sys/class/thermal/thermal_zone[N]/temp` for every
 *      zone whose `type` begins with `cpu-thermal` (zones are chosen by
 *      type, never by index: the index depends on probe order) and
 *      returns the hottest.  The kernel owns the TSU; nothing here
 *      touches its registers.
 *
 * @par Zephyr binds the UPSTREAM `die-temp0` devicetree alias.
 *      It goes through the upstream sensor API (`SENSOR_CHAN_DIE_TEMP`),
 *      so any SoC or board tree that declares an upstream
 *      die-temperature node answers with no Alp driver.  Baremetal
 *      builds return @ref ALP_ERR_NOSUPPORT.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      Function-granularity marker; see docs/abi-markers.md.
 *
 * @param[out] milli_c  Set to the reading on @ref ALP_OK.  Left
 *                       untouched on any error.
 *
 * @return  @ref ALP_OK on a valid read.
 *          @ref ALP_ERR_INVAL when @p milli_c is NULL.
 *          @ref ALP_ERR_NOSUPPORT when this build has no thermal source:
 *                                 baremetal, a Zephyr tree with no
 *                                 `die-temp0` node (or no driver bound
 *                                 to it), or a Linux kernel exposing no
 *                                 matching zone.
 *          @ref ALP_ERR_NOT_READY when the Zephyr device bound but did
 *                                 not come up.
 *          @ref ALP_ERR_IO on a transfer fault while reading (Zephyr),
 *                          or when matching Linux zones exist but none
 *                          could be read.
 */
alp_status_t alp_temperature_read_die_milli_c(int32_t *milli_c);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_TEMPERATURE_H */
