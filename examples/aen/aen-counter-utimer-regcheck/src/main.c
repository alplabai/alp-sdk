/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-silicon UTIMER counter validation for the E1M-AEN801 (Alif Ensemble E8)
 * over the alp-sdk Tier-1.5 counter_alif_utimer driver (ADR 0017 Tier-1.5,
 * "alif,utimer-counter" -- a thin Zephyr counter-class shell over the Apache-2.0
 * hal_alif alif_utimer_* register library).
 *
 * What it proves
 * --------------
 * Start the free-running 32-bit utimer0 counter through the PORTABLE Zephyr
 * counter_* class API and confirm the count actually ADVANCES on silicon, two
 * independent ways:
 *
 *   1. in-firmware:  this app reads counter_get_value() twice with a busy-wait
 *      between, prints both raw tick values + their delta + every API return
 *      code, and a single 'RESULT PASS:' / 'RESULT FAIL:' line.
 *   2. over J-Link:  the human reads the UTIMER CNTR register with mem32
 *      (0x48001044, see the overlay) across the same window -- the ground truth
 *      on silicon, independent of any printk.
 *
 * Counter node shape (see the overlay): the counter_alif_utimer driver binds the
 * "alif,utimer-counter" CHILD (utimer0_counter) and reaches the "alif,utimer"
 * PARENT (utimer0 @ 0x48001000) for the timer/global reg windows + timer-id +
 * clock-frequency + the comp_capt_a IRQ.
 *
 * BENCH NOTE -- tick rate: the parent's clock-frequency is the dtsi PLACEHOLDER
 * 100 MHz; the counter is expected to advance at a HIGHER real rate (early bench
 * observation ~400 MHz tick).  A wrong clock-frequency ONLY mis-scales
 * counter_us_to_ticks(); it does NOT change whether the count advances, so this
 * test (advance == PASS) is valid regardless.  Do NOT hard-code a "real" rate
 * here -- the authoritative value is a silicon HW fact from the Alif Ensemble E8
 * TRM (pending bench), per the alp-sdk pending-hw-configs policy.
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

/* Dwell between the two counter reads.  ~2 ms is plenty for the count to move at
 * any plausible UTIMER tick rate (100 MHz placeholder -> 200000 ticks; a faster
 * real rate only widens the delta), and short enough not to wrap a 32-bit
 * free-running counter. */
#define READ_GAP_US 2000U

static const struct device *const counter0 = DEVICE_DT_GET(COUNTER_NODE);

int main(void)
{
	int      rc;
	uint32_t freq, top, v1, v2, delta;

	printk("\n=== AEN801 UTIMER counter bench "
	       "(counter_alif_utimer / utimer0_counter) ===\n");

	/* 1. device readiness.  If the counter node did not instantiate a device
	 *    the build would have failed at link (undefined __device_dts_ord_*),
	 *    so reaching here means the device object exists -- check it is ready. */
	if (!device_is_ready(counter0)) {
		printk("RESULT FAIL: utimer0_counter device not ready\n");
		return 0;
	}
	printk("utimer0_counter device ready\n");

	/* 2. static parameters.  freq is the parent clock-frequency (placeholder
	 *    100 MHz this build); top is the free-running 32-bit reload value
	 *    (expect 0xFFFFFFFF). */
	freq = counter_get_frequency(counter0);
	top  = counter_get_top_value(counter0);
	printk("counter_get_frequency() = %u Hz (dtsi placeholder; real rate per TRM)\n", freq);
	printk("counter_get_top_value() = 0x%08x\n", top);

	/* 3. start the free-running counter. */
	rc = counter_start(counter0);
	printk("counter_start() rc=%d\n", rc);
	if (rc != 0) {
		printk("RESULT FAIL: counter_start rc=%d\n", rc);
		return 0;
	}

	/* 4. first reading. */
	rc = counter_get_value(counter0, &v1);
	printk("counter_get_value() #1 rc=%d  ticks=0x%08x (%u)\n", rc, v1, v1);
	if (rc != 0) {
		printk("RESULT FAIL: counter_get_value #1 rc=%d\n", rc);
		return 0;
	}

	/* --- J-Link readback window #1: mem32 0x48001044 (UTIMER0 CNTR) --- */

	/* 5. let the counter advance, then read again. */
	k_busy_wait(READ_GAP_US);

	rc = counter_get_value(counter0, &v2);
	printk("counter_get_value() #2 rc=%d  ticks=0x%08x (%u)\n", rc, v2, v2);
	if (rc != 0) {
		printk("RESULT FAIL: counter_get_value #2 rc=%d\n", rc);
		return 0;
	}

	/* --- J-Link readback window #2: mem32 0x48001044 (UTIMER0 CNTR) --- */

	/* 6. verdict: a free-running up-counter must have advanced between the two
	 *    reads.  Use unsigned wrap so a (very unlikely) 32-bit wrap across the
	 *    ~2 ms gap still yields the true positive delta. */
	delta = v2 - v1;
	printk("delta (v2 - v1) = %u ticks over ~%u us\n", delta, READ_GAP_US);

	if (delta != 0U) {
		printk("RESULT PASS: utimer0 counter advances "
		       "(v1=0x%08x v2=0x%08x delta=%u ticks over ~%u us)\n",
		       v1,
		       v2,
		       delta,
		       READ_GAP_US);
	} else {
		printk("RESULT FAIL: counter did not advance "
		       "(v1=0x%08x v2=0x%08x delta=0)\n",
		       v1,
		       v2);
	}

	/* Leave the counter running + park; CNTR stays live for the J-Link read. */
	return 0;
}
