/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file rtc.h
 * @brief Alp SDK real-time-clock abstraction.
 *
 * Wraps the SoC's wall-clock RTC.  Time is exchanged in a struct
 * shaped after `struct tm` so apps can format-print without an extra
 * conversion step.
 *
 * Backends:
 *   - Zephyr   : `rtc_*` driver class (Zephyr 3.5+).
 *   - Yocto    : `clock_gettime(CLOCK_REALTIME)` + `/dev/rtc0`.
 *   - Baremetal: vendor HAL RTC peripheral.
 *
 * Typical usage:
 * @code
 *     alp_rtc_t *rtc = alp_rtc_open(0);
 *     alp_rtc_time_t t = {
 *         .year = 2026, .month = 5, .day = 10,
 *         .hour = 14,  .minute = 30, .second = 0,
 *     };
 *     alp_rtc_set_time(rtc, &t);
 * @endcode
 *
 * @par ABI status: [ABI-STABLE]
 *      v0.2.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_RTC_H
#define ALP_RTC_H

#include <stdint.h>

#include <alp/cap_instance.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Calendar time in human-readable fields.  No timezone — wall clock only. */
typedef struct {
    uint16_t year;            /**< Full year, e.g. 2026. */
    uint8_t  month;           /**< 1..12 */
    uint8_t  day;             /**< 1..31 */
    uint8_t  weekday;         /**< 0..6, 0 = Sunday.  0 = "unknown". */
    uint8_t  hour;            /**< 0..23 */
    uint8_t  minute;          /**< 0..59 */
    uint8_t  second;          /**< 0..59 */
    uint16_t millisecond;     /**< 0..999.  0 if the RTC has 1 s resolution. */
} alp_rtc_time_t;

/** Opaque RTC handle.  Allocate via @ref alp_rtc_open. */
typedef struct alp_rtc alp_rtc_t;

/**
 * @brief Acquire an RTC handle.
 *
 * Most SoMs expose exactly one RTC; pass @c rtc_id = 0.  Multi-RTC
 * SoMs (e.g. RZ/V2N has the SoC RTC + a battery-backed PMIC RTC)
 * map to higher ids via the `alp-rtc<N>` devicetree alias.
 *
 * @param[in] rtc_id  Studio-resolved RTC index (0..1).
 * @return Open handle on success, or NULL on resolution failure.
 */
alp_rtc_t   *alp_rtc_open(uint32_t rtc_id);

/**
 * @brief Set the wall-clock time.
 *
 * @param[in] rtc   Handle from @ref alp_rtc_open.
 * @param[in] time  New time.  All fields are validated against the
 *                  documented ranges; out-of-range values return
 *                  ALP_ERR_INVAL without modifying the RTC.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_IO.
 */
alp_status_t alp_rtc_set_time(alp_rtc_t *rtc, const alp_rtc_time_t *time);

/**
 * @brief Read the current wall-clock time.
 *
 * @param[in]  rtc   Handle from @ref alp_rtc_open.
 * @param[out] time  Receives the current time.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_IO.
 */
alp_status_t alp_rtc_get_time(alp_rtc_t *rtc, alp_rtc_time_t *time);

/** @brief Release the handle.  Does not stop the RTC.  NULL is a no-op. */
void         alp_rtc_close(alp_rtc_t *rtc);

/**
 * @brief Query the capabilities of an opened RTC handle.
 *
 * @param rtc  Handle from @ref alp_rtc_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p rtc is NULL.
 */
const alp_capabilities_t *alp_rtc_capabilities(const alp_rtc_t *rtc);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_RTC_H */
