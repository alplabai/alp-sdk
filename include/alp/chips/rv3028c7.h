/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file rv3028c7.h
 * @brief Micro Crystal RV-3028-C7 32.768 kHz extreme-low-power RTC.
 *
 * 1 PPM TCXO accuracy, 100 nA typical I_DD at 3.0 V, integrated
 * trickle charger, programmable alarm with INT pin.  On the
 * E1M-AEN module the RTC sits on Alif's LPI2C bus; its INT line
 * routes to the Alif's `RTC_ALARM` pin (`P15_0_FLEX`) so the
 * application can wake from the alarm.
 *
 * I2C address is fixed at **0x52** (7-bit).
 *
 * Date / time registers are BCD-encoded per datasheet table 4:
 *   0x00  Seconds   (00..59)
 *   0x01  Minutes   (00..59)
 *   0x02  Hours     (00..23 -- 24h mode used here)
 *   0x03  Weekday   (1..7)
 *   0x04  Date      (01..31)
 *   0x05  Month     (01..12)
 *   0x06  Year      (00..99 -- 2-digit, year 2000..2099)
 *
 * The driver reads / writes the seven date-time bytes in one
 * transaction so the RTC's internal latch keeps the values
 * coherent across the rollover boundary.
 */

#ifndef ALP_CHIPS_RV3028C7_H
#define ALP_CHIPS_RV3028C7_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RV3028C7_I2C_ADDR 0x52u

/** Wall-clock representation; same shape as <time.h>'s tm but
 *  packed for I2C transport with no padding. */
typedef struct {
    uint8_t  second;  /**< 0..59 */
    uint8_t  minute;  /**< 0..59 */
    uint8_t  hour;    /**< 0..23 */
    uint8_t  weekday; /**< 1..7 (1 = Sunday by convention) */
    uint8_t  day;     /**< 1..31 */
    uint8_t  month;   /**< 1..12 */
    uint16_t year;    /**< Full year 2000..2099 */
} rv3028c7_time_t;

/** Alarm match mask -- which fields participate in the comparison. */
typedef struct {
    bool match_minute;
    bool match_hour;
    bool match_day_or_weekday;
    bool use_weekday; /**< false => match by day-of-month */
} rv3028c7_alarm_match_t;

typedef struct {
    bool       initialised;
    alp_i2c_t *bus;
} rv3028c7_t;

/** @brief Probe the RTC, clear the oscillator-stop flag (set on
 *         power-on), and configure 24-hour mode. */
alp_status_t rv3028c7_init(rv3028c7_t *ctx, alp_i2c_t *bus);

/** @brief Read the current wall-clock time. */
alp_status_t rv3028c7_get_time(rv3028c7_t *ctx, rv3028c7_time_t *out);

/** @brief Write a wall-clock time (24-hour mode). */
alp_status_t rv3028c7_set_time(rv3028c7_t *ctx, const rv3028c7_time_t *t);

/** @brief Configure the alarm registers + match mask. */
alp_status_t rv3028c7_set_alarm(rv3028c7_t *ctx, const rv3028c7_time_t *when,
                                const rv3028c7_alarm_match_t *match);

/** @brief Enable (or disable) the alarm-flag -> INT pin routing. */
alp_status_t rv3028c7_alarm_int_enable(rv3028c7_t *ctx, bool enable);

/** @brief Read + clear the alarm-fired flag.  Returns *fired = true
 *         iff the alarm has triggered since the last clear. */
alp_status_t rv3028c7_alarm_check_and_clear(rv3028c7_t *ctx, bool *fired);

void         rv3028c7_deinit(rv3028c7_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_RV3028C7_H */
