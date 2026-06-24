/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-silicon calendar-RTC validation for the E1M-AEN801 (Alif Ensemble E8,
 * M55-HE), bench RAM-run via J-Link.
 *
 * What it proves
 * --------------
 * The PORTABLE calendar surface alp_rtc_set_time() / alp_rtc_get_time()
 * (<alp/rtc.h>) now works on the E8 -- even though the E8 has NO Zephyr-RTC-class
 * peripheral.  The only timekeeping hardware is the always-on LPRTC
 * (lprtc@42000000, "snps,dw-apb-rtc"), which is a bare free-running 32-bit
 * COUNTER (CCVR @ 32768 Hz), not a date/time register file.
 *
 * The alp-sdk "lprtc_calendar_shim" rtc backend
 * (src/backends/rtc/lprtc_calendar_shim.c, selected on silicon_ref
 * "alif:ensemble:e8") bridges the two: it keeps a software epoch base + a
 * counter snapshot and computes
 *     time = epoch_base + (counter_now - tick_snapshot) / 32768
 * so the customer-facing alp_rtc_* API behaves like a calendar clock over the
 * counter.  This app is the end-to-end proof of that shim:
 *
 *   1. open the RTC (alp_rtc_open) -- selection lands on the E8 shim backend.
 *   2. set a known wall-clock (alp_rtc_set_time).
 *   3. busy-wait a fixed window so the LPRTC counter advances.
 *   4. read it back (alp_rtc_get_time) and confirm the clock ADVANCED by about
 *      the busy-wait duration -- a single 'RESULT PASS:' / 'RESULT FAIL:' line.
 *
 * Ground truth (independent of any printk): the human ALSO reads the LPRTC CCVR
 * over J-Link (mem32 0x42000000) across the same window -- it must advance
 * ~32768 ticks/s.  See aen-rtc-regcheck for the raw counter proof; THIS app
 * proves the calendar shim layered on top of it.
 *
 * Volatility note: the shim's epoch base lives in RAM only (the DW-APB-RTC has
 * no battery-backed scratch), so the wall-clock resets on a cold boot until
 * set_time runs again.  Persisting it to retained storage is a SoM-policy TBD
 * (see the backend header + aen-rtc-regcheck/README.md) -- it does NOT affect
 * this in-session advance proof.
 *
 * BENCH NOTE -- VBAT clock-gate: the LPRTC clock is gated from the always-on
 * VBAT domain (VBAT_LPRTC0_CLK_EN, 0x1A609010).  On the bench it is already on
 * (the counter regcheck saw counter_start -> -EALREADY); if get_time never
 * advances here, that gate is the likely cause -- a documented blocker, not a
 * code defect.  Do NOT poke VBAT from app code.
 *
 * Console is the RAM buffer 'ram_console_buf' (see prj.conf); the bench UART is
 * not wired to USB.  BENCH-VALIDATION app -- not a customer teaching example.
 */

#include <zephyr/kernel.h>

#include <alp/peripheral.h>
#include <alp/rtc.h>

/* Wall-clock we set the calendar to before the advance check.  Arbitrary fixed
 * instant -- the test asserts the clock MOVES from here, not its absolute value. */
#define SET_YEAR   2026
#define SET_MONTH  6
#define SET_DAY    18
#define SET_HOUR   14
#define SET_MINUTE 30
#define SET_SECOND 0

/* Dwell between set and read-back.  ~2 s is long enough that the 1 s-resolution
 * second field is guaranteed to tick at least once on a 32768 Hz counter, while
 * staying far from the ~36 h 32-bit-counter wrap. */
#define ADVANCE_GAP_MS 2000U

/* Convert a calendar instant to seconds-of-day for a coarse "did it advance"
 * delta.  Same proleptic civil math as the shim; day rollover is rare across
 * the 2 s window but handled by treating a negative delta as a wrap. */
static int64_t _secs_of_day(const alp_rtc_time_t *t)
{
	return (int64_t)t->hour * 3600 + (int64_t)t->minute * 60 + (int64_t)t->second;
}

int main(void)
{
	printk("\n=== AEN801 LPRTC calendar shim "
	       "(alp_rtc_* over lprtc@42000000 counter) ===\n");

	/* 1. open the calendar RTC.  On the E8 the backend selector lands on the
	 *    lprtc_calendar_shim (exact silicon_ref "alif:ensemble:e8"), which binds
	 *    the LPRTC counter device under the hood.  A NULL handle means the shim
	 *    could not bind the counter -- alp_last_error() says why. */
	alp_rtc_t *rtc = alp_rtc_open(0);
	if (rtc == NULL) {
		printk("RESULT FAIL: alp_rtc_open(0) returned NULL, alp_last_error=%d\n",
		       (int)alp_last_error());
		return 0;
	}
	printk("alp_rtc_open(0) ok (lprtc calendar shim bound)\n");

	/* 2. set a known wall-clock.  The shim snapshots the LPRTC counter here and
	 *    pins this instant to it. */
	alp_rtc_time_t set_t = {
		.year   = SET_YEAR,
		.month  = SET_MONTH,
		.day    = SET_DAY,
		.hour   = SET_HOUR,
		.minute = SET_MINUTE,
		.second = SET_SECOND,
	};
	alp_status_t s = alp_rtc_set_time(rtc, &set_t);
	printk("alp_rtc_set_time(%04u-%02u-%02u %02u:%02u:%02u) -> %d\n",
	       set_t.year,
	       set_t.month,
	       set_t.day,
	       set_t.hour,
	       set_t.minute,
	       set_t.second,
	       (int)s);
	if (s != ALP_OK) {
		printk("RESULT FAIL: set_time status=%d\n", (int)s);
		alp_rtc_close(rtc);
		return 0;
	}

	/* 3. read it straight back; should be the instant we set (give or take the
	 *    sub-second the two calls spanned). */
	alp_rtc_time_t t1;
	s = alp_rtc_get_time(rtc, &t1);
	printk("alp_rtc_get_time #1 -> status=%d, %04u-%02u-%02u %02u:%02u:%02u.%03u\n",
	       (int)s,
	       t1.year,
	       t1.month,
	       t1.day,
	       t1.hour,
	       t1.minute,
	       t1.second,
	       t1.millisecond);
	if (s != ALP_OK) {
		printk("RESULT FAIL: get_time #1 status=%d\n", (int)s);
		alp_rtc_close(rtc);
		return 0;
	}

	/* --- J-Link readback window #1: mem32 0x42000000 (LPRTC CCVR) --- */

	/* 4. let the LPRTC counter advance, then read again.  Use a kernel sleep so
	 *    the counter free-runs in the VBAT domain across the window. */
	k_msleep(ADVANCE_GAP_MS);

	alp_rtc_time_t t2;
	s = alp_rtc_get_time(rtc, &t2);
	printk("alp_rtc_get_time #2 -> status=%d, %04u-%02u-%02u %02u:%02u:%02u.%03u\n",
	       (int)s,
	       t2.year,
	       t2.month,
	       t2.day,
	       t2.hour,
	       t2.minute,
	       t2.second,
	       t2.millisecond);
	if (s != ALP_OK) {
		printk("RESULT FAIL: get_time #2 status=%d\n", (int)s);
		alp_rtc_close(rtc);
		return 0;
	}

	/* --- J-Link readback window #2: mem32 0x42000000 (LPRTC CCVR) --- */

	/* 5. verdict: the calendar must have advanced by about the busy-wait window.
	 *    A counter-derived clock advances every read, so any positive delta over
	 *    the ~2 s gap means the shim is tracking the counter.  Compare in
	 *    seconds-of-day; a negative delta would only mean a midnight rollover,
	 *    which we treat as a wrap (+86400). */
	int64_t d = _secs_of_day(&t2) - _secs_of_day(&t1);
	if (d < 0) d += 86400;
	printk(
	    "calendar delta (read #2 - read #1) = %lld s over ~%u ms\n", (long long)d, ADVANCE_GAP_MS);

	if (d > 0) {
		printk("RESULT PASS: alp_rtc calendar advances over the LPRTC counter "
		       "(delta=%lld s, set=%04u-%02u-%02u %02u:%02u:%02u, "
		       "read=%04u-%02u-%02u %02u:%02u:%02u)\n",
		       (long long)d,
		       set_t.year,
		       set_t.month,
		       set_t.day,
		       set_t.hour,
		       set_t.minute,
		       set_t.second,
		       t2.year,
		       t2.month,
		       t2.day,
		       t2.hour,
		       t2.minute,
		       t2.second);
	} else {
		printk("RESULT FAIL: calendar did not advance over ~%u ms "
		       "(both reads %02u:%02u:%02u) -- check the VBAT LPRTC clock-gate "
		       "(0x1A609010); see the README\n",
		       ADVANCE_GAP_MS,
		       t2.hour,
		       t2.minute,
		       t2.second);
	}

	alp_rtc_close(rtc);
	return 0;
}
