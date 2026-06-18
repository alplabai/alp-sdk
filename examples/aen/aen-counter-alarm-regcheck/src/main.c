/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-silicon UTIMER COMPARE-A *alarm* validation for the E1M-AEN801 (Alif
 * Ensemble E8) over the alp-sdk Tier-1.5 counter_alif_utimer driver.  Sibling to
 * aen-counter-utimer-regcheck (which proves the free-running counter advances);
 * this proves the COMPARE-A one-shot alarm callback actually fires.
 *
 * Why this example exists (the bugs it guards)
 * --------------------------------------------
 * On first bring-up the COMPARE-A alarm callback never fired (fired=0) even
 * though the COMPARE_EN bit, interrupt unmask, and an NVIC enable were all set.
 * The bench root-caused TWO independent driver bugs (both fixed in
 * counter_alif_utimer.c):
 *   1. WRONG SHADOW: the COMPARE-A match interrupt (CHAN_INTERRUPT_COMPARE_A_BUF1,
 *      bit2) compares the counter against the COMPARE_A_BUF1 shadow register
 *      (0xD4), NOT the COMPARE_A register (0xD0) the driver wrote -- so the
 *      shadow stayed 0 and bit2 only latched at the start-of-count CNTR==0 tick.
 *   2. WRONG IRQ LINE: the 8 UTIMER NVIC lines map 1:1 to the 8 CHAN_INTERRUPT
 *      bits; the alarm's bit2 asserts on "comp_a_buf1" (IRQ index 2), but the
 *      driver wired the ISR to "comp_capt_a" (bit0/CAPTURE_A) -- so even once
 *      bit2 latched, its NVIC line was never enabled and the ISR never ran.
 *
 * What it proves
 * --------------
 *   1. arm a relative COMPARE-A alarm via the PORTABLE Zephyr counter API
 *      (counter_set_channel_alarm) and confirm the callback fires (fired=1);
 *   2. the one-shot clears, so a re-arm from a fresh call fires again;
 * with a single 'RESULT PASS:' / 'RESULT FAIL:' line.
 *
 * Tick-rate note: the alarm target is a LARGE relative tick count so the match
 * is well ahead of the live count at arm time regardless of the (TRM-pending)
 * real tick rate -- this avoids the relative-alarm "target already passed"
 * window (a small target on a fast free-running counter is only matched after a
 * 2^32 wrap).  Do NOT hard-code a "real" rate here (pending-hw-configs policy).
 *
 * Console is the RAM buffer 'ram_console_buf' (see prj.conf); the bench UART is
 * not wired to USB.  BENCH-VALIDATION app -- not a customer teaching example.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>

/* The counter device is the "alif,utimer-counter" CHILD node (utimer0_counter);
 * the driver binds the child, not the alif,utimer parent. */
#define COUNTER_NODE DT_NODELABEL(utimer0_counter)

/*
 * Large relative target: ~40M ticks is ~99 ms at the ~404 MHz observed rate
 * (~400 ms at the 100 MHz dtsi placeholder) -- far ahead of the live count at
 * arm time, and well inside the wait window below, at either rate.
 */
#define ALARM_TICKS 40000000U

#define WAIT_STEPS   300U   /* poll iterations          */
#define WAIT_STEP_US 10000U /* 10 ms each -> ~3 s budget */

static const struct device *const counter0 = DEVICE_DT_GET(COUNTER_NODE);

static volatile uint32_t alarm_fired;
static volatile uint32_t alarm_ticks_at_cb;

static void alarm_cb(const struct device *dev, uint8_t chan_id, uint32_t ticks, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan_id);
	ARG_UNUSED(user_data);

	alarm_ticks_at_cb = ticks;
	alarm_fired += 1U;
}

/* Arm one relative COMPARE-A alarm and spin until the running fired-count
 * reaches want_count or the window ends.  Returns 1 on reaching it, else 0. */
static int arm_and_wait(uint32_t want_count)
{
	struct counter_alarm_cfg acfg = {
		.callback  = alarm_cb,
		.ticks     = ALARM_TICKS, /* relative: target = CNTR + ticks */
		.user_data = NULL,
		.flags     = 0U,
	};
	int rc = counter_set_channel_alarm(counter0, 0, &acfg);

	printk("counter_set_channel_alarm(chan 0, ticks=%u) rc=%d\n", ALARM_TICKS, rc);
	if (rc != 0) {
		printk("RESULT FAIL: set_channel_alarm rc=%d\n", rc);
		return 0;
	}

	for (uint32_t i = 0; i < WAIT_STEPS; i++) {
		if (alarm_fired >= want_count) {
			return 1;
		}
		k_busy_wait(WAIT_STEP_US);
	}
	return (alarm_fired >= want_count) ? 1 : 0;
}

int main(void)
{
	int      rc;
	uint32_t v0;

	printk("\n=== AEN801 UTIMER COMPARE-A alarm bench "
	       "(counter_alif_utimer / utimer0_counter) ===\n");

	if (!device_is_ready(counter0)) {
		printk("RESULT FAIL: utimer0_counter device not ready\n");
		return 0;
	}

	rc = counter_start(counter0);
	printk("counter_start() rc=%d\n", rc);
	if (rc != 0) {
		printk("RESULT FAIL: counter_start rc=%d\n", rc);
		return 0;
	}

	rc = counter_get_value(counter0, &v0);
	printk("counter_get_value() rc=%d  CNTR=0x%08x\n", rc, v0);

	/* First alarm. */
	if (!arm_and_wait(1U)) {
		printk("RESULT FAIL: COMPARE-A alarm #1 did not fire within ~%u ms "
		       "(fired=%u)\n",
		       (WAIT_STEPS * WAIT_STEP_US) / 1000U,
		       alarm_fired);
		return 0;
	}
	printk("alarm #1 fired (ticks_at_cb=0x%08x)\n", alarm_ticks_at_cb);

	/* Re-arm: the one-shot must have cleared so a second call fires again. */
	if (!arm_and_wait(2U)) {
		printk("RESULT FAIL: COMPARE-A alarm #2 (re-arm) did not fire within "
		       "~%u ms (fired=%u)\n",
		       (WAIT_STEPS * WAIT_STEP_US) / 1000U,
		       alarm_fired);
		return 0;
	}
	printk("alarm #2 fired (ticks_at_cb=0x%08x)\n", alarm_ticks_at_cb);

	printk("RESULT PASS: COMPARE-A alarm fires + re-arms "
	       "(fired=%u over two arms)\n",
	       alarm_fired);
	return 0;
}
