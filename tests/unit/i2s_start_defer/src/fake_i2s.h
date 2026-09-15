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

/**
 * @brief Force a TX underrun: mirrors zephyr/drivers/i2s/i2s_dw.c's TX
 *        IRQ handler finding the ring empty when it goes to fetch the
 *        NEXT block right after finishing the current one
 *        (i2s_dw.c:516-524) -- issue #2137. A no-op unless the fake is
 *        genuinely RUNNING with an empty ring (fake_i2s_tx_running() &&
 *        fake_i2s_tx_queue_depth() == 0); a stream that still has a
 *        block queued ahead would just keep playing on real hardware,
 *        not underrun. On success, moves to I2S_STATE_ERROR: START
 *        needs READY and -EIOs (i2s_dw.c:278-283), STOP/DRAIN need
 *        RUNNING and -EIO too (i2s_dw.c:301-304/315-321), and write()
 *        needs RUNNING or READY and -EIOs (i2s_dw.c:390-394) -- the
 *        ONLY way out is TRIGGER_PREPARE (i2s_dw.c:340-347) or
 *        TRIGGER_DROP (i2s_dw.c:329-338).
 */
void fake_i2s_tx_simulate_underrun(void);

/** @brief True while the fake is in the post-underrun I2S_STATE_ERROR
 *  (issue #2137) -- i.e. after fake_i2s_tx_simulate_underrun() and before
 *  a successful PREPARE or DROP. */
bool fake_i2s_tx_in_error(void);

/**
 * @brief Make the NEXT @p count TX PREPARE triggers fail with
 *        @p neg_errno instead of the normal ERROR-only-else-EIO check
 *        (issue #2137). Same one-shot-countdown shape as
 *        fake_i2s_force_start_fail().
 */
void fake_i2s_force_prepare_fail(int neg_errno, unsigned int count);

/** @brief Count of TX_TRIGGER_PREPARE calls this fake has seen since the
 *  last fake_i2s_reset(), regardless of outcome. The way a test proves
 *  the backend actually ATTEMPTED an underrun recovery, rather than
 *  merely observing a status code a backend with no recovery logic at
 *  all would also happen to return (issue #2137). */
size_t fake_i2s_prepare_call_count(void);

/**
 * @brief The REAL Zephyr k_mem_slab's current free-block count -- the
 *        slab captured at configure() time (src/backends/i2s/
 *        zephyr_drv.c's 2-block ping-pong slab). The direct, ground-
 *        truth way to prove a sequence neither leaked a block (count
 *        stays below the slab's capacity) nor double-freed one (which
 *        would corrupt the free list, not simply over-report the
 *        count -- so this is a necessary, not sufficient, check; pair
 *        it with a clean, crash-free run). 0 before any open() (no slab
 *        configured yet).
 */
size_t fake_i2s_slab_free_count(void);

#endif /* ALP_TEST_FAKE_I2S_H */
