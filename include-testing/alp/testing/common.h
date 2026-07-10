/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file common.h
 * @brief Cross-class reset for `alp/testing` test doubles.
 *
 * Every class double keeps its injected state in a per-class instance
 * table (src/testing/instance_table.c) keyed by the class's public
 * instance id, plus the shared virtual clock (`<alp/testing/clock.h>`).
 * @ref alp_testing_reset_all wipes all of it in one call so a test
 * suite can start each case from a clean slate without caring how many
 * classes are compiled in.
 */

#ifndef ALP_TESTING_COMMON_H
#define ALP_TESTING_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reset every `alp/testing` double to its initial state.
 *
 * Resets the virtual clock (@ref alp_testing_clock_reset) and wipes
 * every registered class instance table (injected levels, armed
 * edges, callbacks, counters -- everything a test may have injected
 * or that a prior open() populated).  Call this in a test fixture's
 * setup/teardown so cases don't leak state into one another.
 */
void alp_testing_reset_all(void);

#ifdef __cplusplus
}
#endif

#endif /* ALP_TESTING_COMMON_H */
