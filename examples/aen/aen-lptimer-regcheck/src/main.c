/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-silicon LPTIMER validation for the E1M-AEN801 (Alif Ensemble E8) over the
 * alp-sdk Tier-1.5 counter_alif_lptimer driver (ADR 0017 Tier-1.5,
 * "alif,lptimer" -- a Zephyr counter-class driver written directly over the
 * memory-mapped LPTIMER registers for the always-on lptimer@42001000 block).
 *
 * What it proves
 * --------------
 * Start a free-running LPTIMER channel through the PORTABLE Zephyr counter_*
 * class API and confirm the count actually ADVANCES on silicon, two independent
 * ways:
 *
 *   1. in-firmware:  this app reads counter_get_value() twice with a busy-wait
 *      between, prints both raw tick values + their delta + every API return
 *      code, and a single 'RESULT PASS:' / 'RESULT FAIL:' line.
 *   2. over J-Link:  the human reads the LPTIMER channel-0 CURRENTVAL register
 *      with mem32 (0x42001004, see the overlay) across the same window -- the
 *      ground truth on silicon, independent of any printk.
 *
 * IMPORTANT -- the LPTIMER is a DOWN-counter.  Unlike the LPRTC (which counts
 * UP), the Alif LPTIMER loads 0xFFFFFFFF in free-running mode and counts DOWN,
 * so counter_get_value() returns a value that DECREASES over time.  "Advances"
 * here means the count CHANGES (the first read is strictly GREATER than the
 * second), so the advance delta is computed as (v1 - v2) with unsigned wrap.
 *
 * DISTINCT block.  This is NOT the LPRTC (lprtc@42000000, snps,dw-apb-rtc,
 * aen-rtc-regcheck) and NOT the UTIMER counter (alif,utimer-counter): a separate
 * always-on IP at lptimer@42001000 with its own register map and its own four
 * NVIC lines (60..63 on E8).  This regcheck exercises the LPTIMER counter path.
 *
 * BENCH NOTE -- tick rate: the node's clock-frequency is 32768 (the VBAT-domain
 * 32 kHz LF source the driver selects via TIMER_CLKSEL clock-source 0),
 * transcribed from the Alif DFP.  A wrong clock-frequency would only mis-scale
 * counter_us_to_ticks(); it does NOT change whether the count advances, so this
 * test (advance == PASS) is valid regardless.
 *
 * BENCH NOTE -- clock gate: the driver selects the channel-0 clock in the
 * always-on VBAT TIMER_CLKSEL (0x1A609004).  Whether the selected VBAT LF source
 * is itself running on the alp-sdk upstream-Zephyr build path is bench-TBD.  If
 * CURRENTVAL never advances here, the source clock is the likely cause -- see
 * the overlay / dtsi notes.  This is a documented blocker, not a code defect.
 *
 * Console is the RAM buffer 'ram_console_buf' (see prj.conf); the bench UART is
 * not wired to USB.  BENCH-VALIDATION app -- not a customer teaching example.
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>

/* The counter device is the "alif,lptimer" node (lptimer@42001000, channel 0);
 * the counter_alif_lptimer driver binds it directly. */
#define COUNTER_NODE DT_NODELABEL(lptimer0)

/*
 * Alarm-latency probe (#1829).
 *
 * HWRM 13.1.4.7 makes the LPTIMER toggle period "(LPTIMERn_LOADCOUNT + 1) x
 * LPTIMERn_CLK clock period", and 13.1.4.3 puts the interrupt on the same
 * reload -- so an alarm of N ticks needs LOADCOUNT = N - 1.  The driver used to
 * program LOADCOUNT = N, firing one tick late: at 32768 Hz a 1-tick alarm took
 * 61.0 us instead of 30.5 us, a 100% error that shrinks as 1/N and so hid
 * entirely at long delays.
 *
 * Each row below prints the measured interval next to the ideal N/32768 s.  On
 * the broken driver every row reads one tick (30.5 us) long.
 */
static const uint32_t alarm_ticks[] = { 1U, 2U, 4U, 64U, 1024U };

static volatile uint32_t alarm_fired_cyc;
static volatile bool     alarm_fired;

static void alarm_cb(const struct device *dev, uint8_t chan, uint32_t ticks, void *user)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan);
	ARG_UNUSED(ticks);
	ARG_UNUSED(user);

	alarm_fired_cyc = k_cycle_get_32();
	alarm_fired     = true;
}

/* Dwell between the two counter reads.  The LPTIMER ticks at 32768 Hz, so
 * ~100 ms moves it by ~3277 ticks -- comfortably nonzero for an advance check,
 * and far from a 32-bit wrap (~36 hours at 32768 Hz). */
#define READ_GAP_US 100000U

static const struct device *const lptimer = DEVICE_DT_GET(COUNTER_NODE);

int main(void)
{
	int      rc;
	uint32_t freq, top, v1, v2, delta;

	printk("\n=== AEN801 LPTIMER counter bench "
	       "(counter_alif_lptimer / lptimer@42001000 ch0) ===\n");

	/* 1. device readiness.  If the counter node did not instantiate a device
	 *    the build would have failed at link (undefined __device_dts_ord_*),
	 *    so reaching here means the device object exists -- check it is ready. */
	if (!device_is_ready(lptimer)) {
		printk("RESULT FAIL: lptimer device not ready\n");
		return 0;
	}
	printk("lptimer device ready\n");

	/* 2. static parameters.  freq is the node clock-frequency (32768 Hz, the
	 *    VBAT 32 kHz source); top is the free-running reload value (the LPTIMER
	 *    loads 0xFFFFFFFF in free-run, so expect 0xFFFFFFFF). */
	freq = counter_get_frequency(lptimer);
	top  = counter_get_top_value(lptimer);
	printk("counter_get_frequency() = %u Hz\n", freq);
	printk("counter_get_top_value() = 0x%08x\n", top);

	/* 3. start the free-running down-counter.  counter_start() loads 0xFFFFFFFF
	 *    and enables the channel.  If it was already running the driver returns
	 *    -EALREADY, which is NOT a failure here; treat 0 and -EALREADY as
	 *    success and go on to the advance check. */
	rc = counter_start(lptimer);
	printk("counter_start() rc=%d%s\n",
	       rc,
	       (rc == -EALREADY) ? " (-EALREADY: LPTIMER channel already running)" : "");
	if (rc != 0 && rc != -EALREADY) {
		printk("RESULT FAIL: counter_start rc=%d\n", rc);
		return 0;
	}

	/* 4. first reading. */
	rc = counter_get_value(lptimer, &v1);
	printk("counter_get_value() #1 rc=%d  ticks=0x%08x (%u)\n", rc, v1, v1);
	if (rc != 0) {
		printk("RESULT FAIL: counter_get_value #1 rc=%d\n", rc);
		return 0;
	}

	/* --- J-Link readback window #1: mem32 0x42001004 (LPTIMER ch0 CURRENTVAL) --- */

	/* 5. let the counter advance, then read again. */
	k_busy_wait(READ_GAP_US);

	rc = counter_get_value(lptimer, &v2);
	printk("counter_get_value() #2 rc=%d  ticks=0x%08x (%u)\n", rc, v2, v2);
	if (rc != 0) {
		printk("RESULT FAIL: counter_get_value #2 rc=%d\n", rc);
		return 0;
	}

	/* --- J-Link readback window #2: mem32 0x42001004 (LPTIMER ch0 CURRENTVAL) --- */

	/* 6. verdict: a free-running LPTIMER is a DOWN-counter, so the second read
	 *    must be LESS than the first.  Compute the advance as (v1 - v2) with
	 *    unsigned wrap so a (very unlikely) 32-bit reload across the ~100 ms gap
	 *    still yields the true positive delta. */
	delta = v1 - v2;
	printk("delta (v1 - v2, down-count) = %u ticks over ~%u us\n", delta, READ_GAP_US);

	if (delta != 0U) {
		printk("RESULT PASS: lptimer counter advances "
		       "(v1=0x%08x v2=0x%08x down-delta=%u ticks over ~%u us)\n",
		       v1,
		       v2,
		       delta,
		       READ_GAP_US);
	} else {
		printk("RESULT FAIL: counter did not advance "
		       "(v1=0x%08x v2=0x%08x delta=0)  -- check the VBAT LPTIMER "
		       "clock source (TIMER_CLKSEL 0x1A609004); see the overlay note\n",
		       v1,
		       v2);
	}

	/*
	 * Alarm latency, one row per requested tick count (#1829).  Reported in ns
	 * beside the ideal interval; the driver is correct when measured ~= ideal
	 * and one LPTIMER tick (30518 ns at 32768 Hz) long when it is not.
	 */
	printk("-- alarm latency (#1829) --\n");

	for (size_t i = 0; i < ARRAY_SIZE(alarm_ticks); i++) {
		struct counter_alarm_cfg acfg = {
			.callback  = alarm_cb,
			.ticks     = alarm_ticks[i],
			.user_data = NULL,
			.flags     = 0,
		};
		uint32_t t0;
		int      rc;

		alarm_fired = false;

		t0 = k_cycle_get_32();
		rc = counter_set_channel_alarm(lptimer, 0, &acfg);
		if (rc != 0) {
			printk("  ticks=%-5u set_channel_alarm rc=%d\n", alarm_ticks[i], rc);
			continue;
		}

		/* The register witness, read while the alarm is armed: LOADCOUNT IS
		 * the reload value, so it must read ticks - 1 (HWRM 13.1.4.7 makes the
		 * period LOADCOUNT + 1 clocks).  This is the decisive check -- the
		 * wall-clock number below carries a fixed ~0.7 ms of arm overhead
		 * (the CONTROLREG/LOADCOUNT writes have to cross into the 32768 Hz
		 * clock domain), which swamps a single 30.5 us tick at the short end.
		 */
		printk("  ticks=%-5u LOADCOUNT=%u (expect %u)
		       ",
		       alarm_ticks[i],
		       counter_get_top_value(lptimer),
		       alarm_ticks[i] - 1U);

		/* Longest row is 1024/32768 s = 31.25 ms; 200 ms is slack, not a
		 * measurement window (the timestamp is taken in the callback). */
		for (int spins = 0; spins < 200 && !alarm_fired; spins++) {
			k_msleep(1);
		}

		if (!alarm_fired) {
			printk("  ticks=%-5u NO CALLBACK within 200 ms\n", alarm_ticks[i]);
			(void)counter_cancel_channel_alarm(lptimer, 0);
			continue;
		}

		printk("  ticks=%-5u measured %8u ns   ideal %8u ns\n",
		       alarm_ticks[i],
		       (unsigned)k_cyc_to_ns_floor64(alarm_fired_cyc - t0),
		       (unsigned)(((uint64_t)alarm_ticks[i] * 1000000000ULL) / 32768ULL));
	}

	/* Leave the counter running + park; CURRENTVAL stays live for the J-Link
	 * read. */
	return 0;
}
