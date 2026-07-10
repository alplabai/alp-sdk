/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file clock.h
 * @brief Deterministic virtual clock for `alp/testing` test doubles.
 *
 * Every `alp/testing` class double that needs to defer a stimulus
 * (e.g. @ref alp_testing_gpio_edge_at) schedules against this clock
 * instead of a wall-clock timer.  The clock never sleeps or blocks --
 * time only moves when the test calls @ref alp_testing_clock_advance_ms,
 * and every callback due at or before the new "now" fires synchronously,
 * in timestamp order, on the calling thread before the call returns.
 * This makes edge-timing tests reproducible: no scheduler jitter, no
 * real-time waits in CI.
 *
 * Available only when `CONFIG_ALP_SDK_TESTING=y` (ZTEST builds).
 * `include-testing/` is a separate include root from the public SDK --
 * production app code never sees this header.
 */

#ifndef ALP_TESTING_CLOCK_H
#define ALP_TESTING_CLOCK_H

#include <stdint.h>

#include <alp/peripheral.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reset the virtual clock to time zero and drop every pending
 *        scheduled event without firing it.
 *
 * Called by @ref alp_testing_reset_all between test cases; a test may
 * also call it directly to start a scenario at a known t=0.
 */
void alp_testing_clock_reset(void);

/**
 * @brief Read the virtual clock's current time.
 *
 * @return  Milliseconds since the last @ref alp_testing_clock_reset.
 */
uint64_t alp_testing_clock_now_ms(void);

/**
 * @brief Advance the virtual clock and deliver due events.
 *
 * Moves "now" forward by @p ms milliseconds.  Every event scheduled
 * (by a `alp/testing` double, e.g. @ref alp_testing_gpio_edge_at)
 * with `timestamp <= new now` fires, in ascending timestamp order,
 * synchronously on the calling thread -- never on an ISR or a
 * separate thread, and this call never sleeps or blocks.
 *
 * @param[in] ms  Milliseconds to advance by.
 *
 * @return ALP_OK on success; ALP_ERR_INVAL if advancing would overflow
 *         the clock's internal representation.
 */
alp_status_t alp_testing_clock_advance_ms(uint64_t ms);

#ifdef __cplusplus
}
#endif

#endif /* ALP_TESTING_CLOCK_H */
