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

/** @brief Clear queued/running/fault-injection state. Call between tests. */
void fake_i2s_reset(void);

/** @brief True once a TX START trigger has actually succeeded. */
bool fake_i2s_tx_running(void);

/**
 * @brief Make the NEXT TX START trigger fail with @p neg_errno regardless
 *        of queue state, instead of the normal empty-queue -ENOMEM check.
 *        Consumed after one use.
 */
void fake_i2s_force_start_fail(int neg_errno);

#endif /* ALP_TEST_FAKE_I2S_H */
