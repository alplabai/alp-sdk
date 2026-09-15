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

/**
 * @brief One-shot hook run synchronously by the fake's write() callback,
 *        immediately after it queues the block -- i.e. from INSIDE the
 *        real i2s_write() call z_write() makes at src/backends/i2s/
 *        zephyr_drv.c:445, which is BEFORE z_write() acquires its own
 *        sidecar lock. Lets a single-threaded ztest deterministically
 *        reproduce the start/write-vs-stop race issue #2132's round-4
 *        review raised (a concurrent stop() winning the race and
 *        DROPping this exact block before this write() records
 *        tx_block_queued=true) without real threads.
 *
 * Consumed after one call (set back to NULL); pass NULL to clear it
 * early. There is exactly one hook slot -- this fake is single-threaded
 * test infrastructure, not a general instrumentation framework.
 */
typedef void (*fake_i2s_write_hook_t)(void);
void fake_i2s_set_write_hook(fake_i2s_write_hook_t hook);

#endif /* ALP_TEST_FAKE_I2S_H */
