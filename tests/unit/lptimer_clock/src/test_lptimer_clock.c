/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #2944: set_alarm() reloads the single LPTIMER channel, which used to throw the
 * running count away.  This replays the driver's sequences (start, set_alarm,
 * alarm ISR, cancel_alarm) over a fake down-counter and checks the virtual value
 * never jumps (in elapsed-time terms).
 */
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/ztest.h>

#include "counter_alif_lptimer_clock.h"

/* Fake LPTIMER channel: down-counter, reloads LOADCOUNT after reaching 0. */
struct fake {
	uint32_t cur;
	uint32_t load;
	bool     pending; /* underflow seen since the last EOI */
	bool     alarm;
	uint32_t shown;      /* what CURRENTVAL reads while stale_clks > 0 */
	uint32_t stale_clks; /* clocks left before a reload shows up */
};

static struct fake          hw;
static struct lptimer_clock clk;
static uint32_t             stale_next; /* what the next reload shows while stale */

static void hw_advance(uint32_t n)
{
	while (n--) {
		if (hw.stale_clks) {
			hw.stale_clks--; /* reload not visible yet: segment not started */
		} else if (hw.cur == 0U) {
			hw.cur     = hw.load;
			hw.pending = true;
		} else {
			hw.cur--;
		}
	}
}

/* CURRENTVAL as the CPU reads it: stale for a clock after a reload. */
static uint32_t rd_cur(void)
{
	return hw.stale_clks ? hw.shown : hw.cur;
}

static void hw_reload(uint32_t load, uint32_t stale)
{
	hw.load       = load;
	hw.cur        = load;
	hw.shown      = stale;
	hw.stale_clks = 1U;
}

/* lptimer_reload()'s bounded poll until the new segment is visible. */
static void drv_wait_seen(void)
{
	for (int i = 0; i < 4 && !lptimer_clock_start_seen(&clk, rd_cur()); i++) {
		hw_advance(1);
	}
	zassert_true(lptimer_clock_start_seen(&clk, rd_cur()));
}

/* Ticks elapsed as the virtual (free-running, down-counting) value reports them. */
static uint32_t elapsed(void)
{
	return LPTIMER_CLK_FREE_LOAD - lptimer_clock_value(&clk, rd_cur(), hw.alarm && hw.pending);
}

static void drv_start(void)
{
	lptimer_clock_reset(&clk);
	hw         = (struct fake){ .cur = LPTIMER_CLK_FREE_LOAD, .load = LPTIMER_CLK_FREE_LOAD };
	stale_next = 0U;
}

static void drv_set_alarm(uint32_t ticks)
{
	lptimer_clock_fold(&clk, rd_cur(), false, ticks - 1U);
	hw_reload(clk.load, stale_next);
	hw.alarm = true;
	drv_wait_seen();
}

static void drv_restore_free_run(bool underflowed)
{
	lptimer_clock_fold(&clk, rd_cur(), underflowed, LPTIMER_CLK_FREE_LOAD);
	hw_reload(LPTIMER_CLK_FREE_LOAD, stale_next);
	hw.alarm   = false;
	hw.pending = false;
	drv_wait_seen();
}

static void drv_isr(void)
{
	zassert_true(hw.pending);
	drv_restore_free_run(true);
}

static void drv_cancel(void)
{
	drv_restore_free_run(hw.pending);
}

ZTEST(lptimer_clock, test_value_continuous_across_set_alarm_and_fire)
{
	drv_start();
	hw_advance(1000);
	zassert_equal(elapsed(), 1000U);

	drv_set_alarm(100);
	/* The bug: the reload made the count jump; now it holds. */
	zassert_equal(elapsed(), 1000U);
	hw_advance(60);
	zassert_equal(elapsed(), 1060U);

	hw_advance(40); /* LOADCOUNT 99 + 1 clocks: cur is 0 now */
	hw_advance(1);  /* reload clock: underflow pending */
	zassert_true(hw.pending);
	/* Visible before the ISR runs: no backwards step. */
	zassert_equal(elapsed(), 1101U);

	hw_advance(4); /* ISR latency */
	drv_isr();
	zassert_equal(elapsed(), 1105U);
	hw_advance(50);
	zassert_equal(elapsed(), 1155U);
	zassert_equal(hw.load, LPTIMER_CLK_FREE_LOAD);
}

ZTEST(lptimer_clock, test_value_continuous_across_cancel)
{
	drv_start();
	hw_advance(500);
	drv_set_alarm(1000);
	hw_advance(300);
	drv_cancel();
	zassert_equal(elapsed(), 800U);
	hw_advance(200);
	zassert_equal(elapsed(), 1000U);

	/* Cancel after underflow but before the ISR: the period is not lost. */
	drv_set_alarm(10);
	hw_advance(10 + 1 + 3);
	drv_cancel();
	zassert_equal(elapsed(), 1000U + 10U + 1U + 3U);
}

ZTEST(lptimer_clock, test_free_run_wrap_then_alarm)
{
	uint32_t before;

	drv_start();
	hw.cur = 5U; /* about to wrap the 32-bit down-counter */
	hw_advance(10);
	/* Free-run wrap: virtual value == CURRENTVAL, it wraps without a jump. */
	zassert_equal(lptimer_clock_value(&clk, hw.cur, false), hw.cur);
	zassert_equal(hw.cur, LPTIMER_CLK_FREE_LOAD - 4U);

	before = lptimer_clock_value(&clk, hw.cur, false);
	drv_set_alarm(7);
	zassert_equal(lptimer_clock_value(&clk, hw.cur, false), before);
	hw_advance(8); /* 7 clocks + the reload clock */
	drv_isr();
	hw_advance(3);
	zassert_equal(lptimer_clock_value(&clk, hw.cur, false), before - 8U - 3U);
}

ZTEST(lptimer_clock, test_ticks_one_alarm_period)
{
	drv_start();
	drv_set_alarm(1); /* LOADCOUNT 0 */
	zassert_equal(hw.load, 0U);
	/* load 0 reads as "seen" even on the stale 0, so one stale clock may remain. */
	hw_advance(2); /* (stale clock) + cur 0 -> reload */
	zassert_true(hw.pending);
	drv_isr();
	zassert_equal(elapsed(), 1U);
}

/* E8 silicon (E1M-AEN803): right after LOADCOUNT is reprogrammed, CURRENTVAL
 * still shows the previous segment's count for a timer clock.  The value must
 * not jump ahead by the new period in that window. */
ZTEST(lptimer_clock, test_stale_currentval_after_reload_does_not_jump)
{
	struct lptimer_clock c;

	lptimer_clock_reset(&c);
	uint32_t before = lptimer_clock_value(&c, 0xFFFFFE00U, false);

	lptimer_clock_fold(&c, 0xFFFFFE00U, false, 199U);
	/* Stale read: still the old free-run count, far above the new load. */
	uint32_t stale = lptimer_clock_value(&c, 0xFFFFFE00U, false);

	zassert_equal(stale, before, "value jumped on a stale CURRENTVAL");
	/* First real tick of the new segment. */
	zassert_equal(lptimer_clock_value(&c, 198U, false), before - 1U);
}

/* Reload back to free-run with a stale small count (or the 0 placeholder) on
 * CURRENTVAL: the `> load` clamp cannot catch it (nothing is above 0xFFFFFFFF),
 * the start-seen poll must; then re-arm from the callback, still right after
 * the reload. */
ZTEST(lptimer_clock, test_stale_read_after_reload_to_free_run_and_rearm)
{
	for (uint32_t st = 0U; st <= 150U; st += 150U) {
		uint32_t before;

		drv_start();
		hw_advance(1000);
		drv_set_alarm(200);
		hw_advance(201);
		stale_next = st; /* 0 = placeholder, 150 = previous segment's count */
		drv_isr();
		/* The ISR's callback value needs no CURRENTVAL read. */
		zassert_equal(lptimer_clock_value(&clk, LPTIMER_CLK_FREE_LOAD, false),
		              LPTIMER_CLK_FREE_LOAD - clk.base);
		before = elapsed();
		zassert_equal(before, 1000U + 201U);

		drv_set_alarm(50); /* re-arm from the callback */
		zassert_equal(elapsed(), before);
		hw_advance(10);
		zassert_equal(elapsed(), before + 10U);
		drv_cancel();
		zassert_equal(elapsed(), before + 10U);
	}
}

ZTEST(lptimer_clock, test_start_seen_rejects_stale_and_placeholder)
{
	struct lptimer_clock c = { .base = 0U, .load = LPTIMER_CLK_FREE_LOAD };

	zassert_false(lptimer_clock_start_seen(&c, 0U));
	zassert_false(lptimer_clock_start_seen(&c, 1234U));
	zassert_true(lptimer_clock_start_seen(&c, LPTIMER_CLK_FREE_LOAD));
	zassert_true(lptimer_clock_start_seen(&c, LPTIMER_CLK_FREE_LOAD - 2U));
	c.load = 2U; /* the 0 placeholder is within the window of a tiny load */
	zassert_false(lptimer_clock_start_seen(&c, 0U));
	zassert_true(lptimer_clock_start_seen(&c, 1U));
	c.load = 0U;
	zassert_true(lptimer_clock_start_seen(&c, 0U));
	c.load = 99U;
	zassert_false(lptimer_clock_start_seen(&c, 5000U)); /* previous free-run count */
	zassert_true(lptimer_clock_start_seen(&c, 98U));
}

ZTEST_SUITE(lptimer_clock, NULL, NULL, NULL, NULL, NULL);
