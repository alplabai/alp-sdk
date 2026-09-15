/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test-control surface for fake_i2s.c -- not a DT/driver API, just the
 * knobs the ztest needs to arrange and observe the fake's internal state.
 */

#ifndef ALP_TEST_FAKE_I2S_H
#define ALP_TEST_FAKE_I2S_H

#include <stdbool.h>
#include <stddef.h>

/** @brief Clear queued/running/fault-injection state. Call between tests
 *  that must not see each other's leftovers -- but NOT between two
 *  opens in the SAME test when the point is to prove state that
 *  crosses close()/open() (or doesn't). */
void fake_i2s_reset(void);

/** @brief True once a TX START trigger has actually succeeded and no
 *  STOP/DRAIN/DROP has run since. */
bool fake_i2s_tx_running(void);

/** @brief Count of TX blocks currently sitting in the fake's ring,
 *  queued but not yet consumed by a START trigger. Zero after a clean
 *  DROP/STOP/DRAIN -- the direct way to prove nothing was stranded. */
size_t fake_i2s_tx_queue_depth(void);

/**
 * @brief Make the NEXT @p count TX START triggers fail with
 *        @p neg_errno regardless of queue state, instead of the normal
 *        empty-queue -ENOMEM / already-running -EIO checks. Each
 *        consumed attempt decrements the count; 0 remaining reverts to
 *        normal behaviour.
 */
void fake_i2s_force_start_fail(int neg_errno, unsigned int count);

#endif /* ALP_TEST_FAKE_I2S_H */
