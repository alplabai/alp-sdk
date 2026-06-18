/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-silicon LPRTC validation for the E1M-AEN801 (Alif Ensemble E8) over the
 * alp-sdk Tier-2 counter_dw_rtc driver (ADR 0017 Tier-2, "snps,dw-apb-rtc" -- a
 * Zephyr counter-class driver vendored from the Apache-2.0 zephyr_alif fork for
 * the always-on lprtc@42000000 DesignWare APB RTC).
 *
 * What it proves
 * --------------
 * Start the free-running 32-bit LPRTC counter through the PORTABLE Zephyr
 * counter_* class API and confirm the count actually ADVANCES on silicon, two
 * independent ways:
 *
 *   1. in-firmware:  this app reads counter_get_value() twice with a busy-wait
 *      between, prints both raw tick values + their delta + every API return
 *      code, and a single 'RESULT PASS:' / 'RESULT FAIL:' line.
 *   2. over J-Link:  the human reads the LPRTC CCVR register with mem32
 *      (0x42000000, see the overlay) across the same window -- the ground truth
 *      on silicon, independent of any printk.
 *
 * NOT a calendar RTC.  The DW-APB-RTC is a bare counter (CCVR free-running
 * up-count + one CMR compare).  It has NO date/time registers, so it does NOT
 * back the alp_rtc_* calendar surface; this regcheck exercises the COUNTER path
 * only.  Turning the LPRTC into an alp_rtc_* calendar source needs a
 * counter->calendar shim (software epoch-base in retained storage) that is NOT
 * authored yet -- see the README.
 *
 * BENCH NOTE -- tick rate: the node's clock-frequency is 32768 (the LF source
 * the VBAT domain feeds the LPRTC), transcribed from the fork e1.dtsi.  A wrong
 * clock-frequency would only mis-scale counter_us_to_ticks(); it does NOT change
 * whether the count advances, so this test (advance == PASS) is valid
 * regardless.
 *
 * BENCH NOTE -- clock gate: on the alp-sdk upstream-Zephyr build the VBAT
 * LPRTC clock-gate (VBAT_LPRTC0_CLK_EN, 0x1A609010) is enabled by no visible
 * code (the fork SoC layer does it; upstream soc.c does not; the driver does
 * not).  If CCVR never advances here, the gate is the likely cause -- see the
 * overlay / dtsi notes.  This is a documented blocker, not a code defect.
 *
 * Console is the RAM buffer 'ram_console_buf' (see prj.conf); the bench UART is
 * not wired to USB.  BENCH-VALIDATION app -- not a customer teaching example.
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>

/* The counter device is the "snps,dw-apb-rtc" node (lprtc@42000000); the
 * counter_dw_rtc driver binds it directly. */
#define COUNTER_NODE DT_NODELABEL(rtc0)

/* Dwell between the two counter reads.  The LPRTC ticks at 32768 Hz, so ~100 ms
 * advances it by ~3277 ticks -- comfortably nonzero for an advance check, and
 * far from a 32-bit wrap (~36 hours at 32768 Hz). */
#define READ_GAP_US 100000U

static const struct device *const lprtc = DEVICE_DT_GET(COUNTER_NODE);

int main(void)
{
	int      rc;
	uint32_t freq, top, v1, v2, delta;

	printk("\n=== AEN801 LPRTC counter bench "
	       "(counter_dw_rtc / lprtc@42000000) ===\n");

	/* 1. device readiness.  If the counter node did not instantiate a device
	 *    the build would have failed at link (undefined __device_dts_ord_*),
	 *    so reaching here means the device object exists -- check it is ready. */
	if (!device_is_ready(lprtc)) {
		printk("RESULT FAIL: lprtc device not ready\n");
		return 0;
	}
	printk("lprtc device ready\n");

	/* 2. static parameters.  freq is the node clock-frequency (32768 Hz from
	 *    the e1.dtsi); top is the free-running 32-bit reload value
	 *    (expect 0xFFFFFFFF). */
	freq = counter_get_frequency(lprtc);
	top  = counter_get_top_value(lprtc);
	printk("counter_get_frequency() = %u Hz\n", freq);
	printk("counter_get_top_value() = 0x%08x\n", top);

	/* 3. start the free-running counter.  The LPRTC lives in the always-on VBAT
	 *    domain, so the SES typically leaves it already running -- counter_start
	 *    then returns -EALREADY, which is NOT a failure here (it directly answers
	 *    the open VBAT clock-gate question: the counter is already clocked, so no
	 *    VBAT_LPRTC0_CLK_EN write is needed on this build path).  Treat 0 and
	 *    -EALREADY as success and go on to the advance check. */
	rc = counter_start(lprtc);
	printk("counter_start() rc=%d%s\n",
	       rc,
	       (rc == -EALREADY) ? " (-EALREADY: LPRTC already running in VBAT domain)" : "");
	if (rc != 0 && rc != -EALREADY) {
		printk("RESULT FAIL: counter_start rc=%d\n", rc);
		return 0;
	}

	/* 4. first reading. */
	rc = counter_get_value(lprtc, &v1);
	printk("counter_get_value() #1 rc=%d  ticks=0x%08x (%u)\n", rc, v1, v1);
	if (rc != 0) {
		printk("RESULT FAIL: counter_get_value #1 rc=%d\n", rc);
		return 0;
	}

	/* --- J-Link readback window #1: mem32 0x42000000 (LPRTC CCVR) --- */

	/* 5. let the counter advance, then read again. */
	k_busy_wait(READ_GAP_US);

	rc = counter_get_value(lprtc, &v2);
	printk("counter_get_value() #2 rc=%d  ticks=0x%08x (%u)\n", rc, v2, v2);
	if (rc != 0) {
		printk("RESULT FAIL: counter_get_value #2 rc=%d\n", rc);
		return 0;
	}

	/* --- J-Link readback window #2: mem32 0x42000000 (LPRTC CCVR) --- */

	/* 6. verdict: a free-running up-counter must have advanced between the two
	 *    reads.  Use unsigned wrap so a (very unlikely) 32-bit wrap across the
	 *    ~100 ms gap still yields the true positive delta. */
	delta = v2 - v1;
	printk("delta (v2 - v1) = %u ticks over ~%u us\n", delta, READ_GAP_US);

	if (delta != 0U) {
		printk("RESULT PASS: lprtc counter advances "
		       "(v1=0x%08x v2=0x%08x delta=%u ticks over ~%u us)\n",
		       v1,
		       v2,
		       delta,
		       READ_GAP_US);
	} else {
		printk("RESULT FAIL: counter did not advance "
		       "(v1=0x%08x v2=0x%08x delta=0)  -- check the VBAT LPRTC "
		       "clock-gate (0x1A609010); see the overlay note\n",
		       v1,
		       v2);
	}

	/* Leave the counter running + park; CCVR stays live for the J-Link read. */
	return 0;
}
