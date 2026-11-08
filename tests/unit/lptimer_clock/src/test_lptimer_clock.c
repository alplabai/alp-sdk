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
};

static struct fake          hw;
static struct lptimer_clock clk;

static void hw_advance(uint32_t n)
{
	while (n--) {
		if (hw.cur == 0U) {
			hw.cur     = hw.load;
			hw.pending = true;
		} else {
			hw.cur--;
		}
	}
}

/* Ticks elapsed as the virtual (free-running, down-counting) value reports them. */
static uint32_t elapsed(void)
{
	return LPTIMER_CLK_FREE_LOAD - lptimer_clock_value(&clk, hw.cur, hw.alarm && hw.pending);
}

static void drv_start(void)
{
	lptimer_clock_reset(&clk);
	hw = (struct fake){ .cur = LPTIMER_CLK_FREE_LOAD, .load = LPTIMER_CLK_FREE_LOAD };
}

static void drv_set_alarm(uint32_t ticks)
{
	lptimer_clock_fold(&clk, hw.cur, false, ticks - 1U);
	hw.load  = clk.load; /* reload: the hardware count restarts from the new load */
	hw.cur   = hw.load;
	hw.alarm = true;
}

static void drv_restore_free_run(bool underflowed)
{
	lptimer_clock_fold(&clk, hw.cur, underflowed, LPTIMER_CLK_FREE_LOAD);
	hw.load    = LPTIMER_CLK_FREE_LOAD;
	hw.cur     = hw.load;
	hw.alarm   = false;
	hw.pending = false;
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
	hw_advance(1); /* cur 0 -> reload */
	zassert_true(hw.pending);
	drv_isr();
	zassert_equal(elapsed(), 1U);
}

ZTEST_SUITE(lptimer_clock, NULL, NULL, NULL, NULL, NULL);

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
