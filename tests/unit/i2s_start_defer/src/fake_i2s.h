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

/** @brief Clear queued/running/fault-injection state (both TX and RX). Call
 *  between tests that must not see each other's leftovers -- but NOT
 *  between two opens in the SAME test when the point is to prove state
 *  that crosses close()/open() (or doesn't). */
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
 * @brief Same one-shot-hook shape as fake_i2s_set_write_hook(), but run
 *        from INSIDE fake_i2s_write() at the exact instant it is about to
 *        return -EIO for I2S_STATE_ERROR -- i.e. the real race window
 *        z_write() has: this call's own i2s_write() has already failed,
 *        but z_write() has not yet taken its lock to run its own PREPARE.
 *        Lets a single-threaded ztest run a REAL alp_i2s_write() /
 *        alp_i2s_stop() / alp_i2s_start() call on the SAME handle right
 *        there, standing in for a second thread (issue #2137 review round
 *        3, finding 2 -- replaces a round-2 knob,
 *        fake_i2s_tx_simulate_concurrent_recovery(), that cleared
 *        tx_error directly with no trigger and no flag update, a state no
 *        real caller could ever produce).
 *
 * Consumed after one call (set back to NULL); pass NULL to clear it
 * early. There is exactly one hook slot, same caveat as the write hook.
 */
void fake_i2s_set_write_fail_hook(fake_i2s_write_hook_t hook);

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
 *        fake_i2s_force_start_fail(). Also consumed by an RX PREPARE
 *        trigger -- the knob is shared across directions since no test
 *        exercises both in the same fake_i2s_reset() cycle.
 */
void fake_i2s_force_prepare_fail(int neg_errno, unsigned int count);

/**
 * @brief Make the NEXT @p count TX write() calls fail with @p neg_errno,
 *        checked AFTER the ERROR-state refusal so it can specifically
 *        target a RETRY that runs once a PREPARE has already cleared
 *        I2S_STATE_ERROR (issue #2137 review round 2, finding 5) --
 *        proves z_write()'s retry-after-PREPARE path frees the block
 *        exactly once on a retry failure distinct from the original
 *        underrun's own -EIO. Same one-shot-countdown shape as
 *        fake_i2s_force_start_fail().
 */
void fake_i2s_force_write_fail(int neg_errno, unsigned int count);

/**
 * @brief Make the NEXT @p count DROP triggers (either direction) fail
 *        with @p neg_errno instead of DROP's normal always-succeeds
 *        behaviour -- the only way to reach z_stop()'s both-refused
 *        return path in a test (issue #2137 review round 2, finding 5,
 *        optional case). Same one-shot-countdown shape as
 *        fake_i2s_force_start_fail().
 */
void fake_i2s_force_drop_fail(int neg_errno, unsigned int count);

/** @brief Count of TRIGGER_PREPARE calls this fake has seen since the
 *  last fake_i2s_reset(), regardless of outcome or direction. The way a
 *  test proves the backend actually ATTEMPTED an underrun/overrun
 *  recovery, rather than merely observing a status code a backend with
 *  no recovery logic at all would also happen to return (issue #2137). */
size_t fake_i2s_prepare_call_count(void);

/** @brief Count of TRIGGER_STOP/TRIGGER_DRAIN calls this fake has seen
 *  since the last fake_i2s_reset(), regardless of outcome or direction
 *  (STOP and DRAIN share one counter -- this backend only ever issues
 *  DRAIN). The way a test proves z_stop()'s finding-2/3 skip-DRAIN path
 *  actually skipped the real trigger, rather than issuing it and just
 *  happening to still land on the right status (issue #2137 review
 *  round 2, finding 2). */
size_t fake_i2s_drain_call_count(void);

/**
 * @brief The REAL Zephyr k_mem_slab's current free-block count -- the
 *        TX slab captured at configure() time (src/backends/i2s/
 *        zephyr_drv.c's 2-block ping-pong slab). The direct, ground-
 *        truth way to prove a sequence neither leaked a block (count
 *        stays below the slab's capacity) nor double-freed one (which
 *        would corrupt the free list, not simply over-report the
 *        count -- so this is a necessary, not sufficient, check; pair
 *        it with a clean, crash-free run). 0 before any open() (no slab
 *        configured yet).
 */
size_t fake_i2s_slab_free_count(void);

/** @brief True once an RX START trigger has actually succeeded and no
 *  STOP/DRAIN/DROP has run since (issue #2137 review round 2, finding
 *  1). */
bool fake_i2s_rx_running(void);

/** @brief True while the fake is in the post-overrun I2S_STATE_ERROR
 *  (issue #2137 review round 2, finding 1) -- i.e. after
 *  fake_i2s_rx_simulate_overrun() and before a successful PREPARE or
 *  DROP. */
bool fake_i2s_rx_in_error(void);

/** @brief The REAL Zephyr k_mem_slab's current free-block count for the
 *  RX slab captured at configure() time -- same ground-truth role as
 *  fake_i2s_slab_free_count(), for the RX overrun-leak proof (issue
 *  #2137 review round 2, finding 1). 0 before any RX open(). */
size_t fake_i2s_rx_slab_free_count(void);

/**
 * @brief Force an RX overrun: mirrors zephyr/drivers/i2s/i2s_dw.c's RX
 *        IRQ handler failing to hand off the just-filled block (either
 *        its k_mem_slab_alloc() for the next block, or its queue_put()
 *        of this one, i2s_dw.c:630-648) -- issue #2137 review round 2,
 *        finding 1. A no-op unless the fake is genuinely RUNNING. Models
 *        the FIXED driver: frees the active block back to the slab
 *        rather than leaking it, moves to I2S_STATE_ERROR exactly like
 *        the TX underrun knob (same trigger refusals apply, see
 *        fake_i2s_tx_simulate_underrun()'s doc).
 */
void fake_i2s_rx_simulate_overrun(void);

/**
 * @brief Complete one RX frame: mirrors the ISR's SUCCESS path (i2s_dw.c's
 *        offset >= size branch) -- moves the active (filling) block to
 *        the completed-read ring and allocates a fresh active block. A
 *        no-op unless genuinely RUNNING with an active block. Lets a
 *        test prove a full start-overrun-recover-and-actually-transfer-
 *        data cycle, not just that the slab free count returns to where
 *        it started.
 */
void fake_i2s_rx_complete_block(void);

#endif /* ALP_TEST_FAKE_I2S_H */
