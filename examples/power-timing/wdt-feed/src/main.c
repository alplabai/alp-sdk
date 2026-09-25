/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * wdt-feed — install a 5 second watchdog timeout and feed it from
 * a periodic loop.
 *
 * The watchdog is the safety belt of an embedded system: if main
 * loop hangs (deadlock, infinite loop, lost interrupt), the WDT
 * fires after `timeout_ms` and the SoC resets back to a known
 * good state.  Apps that pass this point without feeding lose
 * their data; production firmware combines WDT with a clean
 * boot-time fault log so the post-mortem is actionable.
 *
 * In CI we exercise the install + feed loop only -- never the
 * timeout -- because letting the WDT actually reset the runner
 * would terminate the test harness uncleanly.  Real product
 * firmware would feed from inside the main loop's iteration
 * boundary, NOT from a kernel timer (a kernel timer keeps
 * feeding even when the main loop is wedged, defeating the
 * watchdog).
 *
 * After the RESET_SOC handle closes, this also opens a second,
 * ALP_WDT_INTERRUPT_ONLY handle on the SAME wdt_id -- demonstrating
 * both that on_expire is mandatory for that mode (v0.17.0, #1637)
 * and that closing a handle cleanly frees its wdt_id for reopening.
 */

#include <stdio.h>

#include "alp/peripheral.h"

#include "alp/e1m_pinout.h"
#include "alp/wdt.h"

/* WDT_TIMEOUT_MS sets the max interval between two feed() calls.
 * Pick conservatively long enough that legitimate worst-case
 * latency (say, a slow flash erase) doesn't trip a false reset,
 * but short enough that a genuine hang doesn't leave the
 * device unresponsive for too long.  5 seconds is typical. */
#define WDT_TIMEOUT_MS 5000

/* Feed every 500 ms -- 10x safety margin against the 5 s timeout.
 * The 10x margin lets feeds line up with a slow main loop without
 * needing exact periodicity. */
#define FEED_PERIOD_MS 500

/* Runs in ISR context on the Zephyr backend -- keep it short; do not
 * block, allocate, or take a mutex here.  Real firmware would set a
 * flag and act on it from the main loop; this printf is example-only
 * and would be the wrong thing to do in production. */
static void on_wdt_expire(alp_wdt_t *wdt, void *user)
{
	(void)wdt;
	printf("[wdt] on_expire fired, user=%p\n", user);
}

int main(void)
{
	printf("[wdt] open id=0 timeout=%u ms\n", WDT_TIMEOUT_MS);

	/* on_timeout = ALP_WDT_RESET_SOC asks for a full SoC reset on
     * miss-feed.  Alternatives are RESET_CPU (core reset only,
     * peripherals retain state -- useful for soft-fault recovery)
     * and INTERRUPT_ONLY (fires an IRQ; you'd capture state in the
     * handler before manually triggering a reset).  wdt_id selects
     * the watchdog instance (ALP_E1M_WDT0 = 0 on every E1M SoM). */
	alp_wdt_t *wdt = alp_wdt_open(&(alp_wdt_config_t){
	    .wdt_id     = ALP_E1M_WDT0,
	    .timeout_ms = WDT_TIMEOUT_MS,
	    .on_timeout = ALP_WDT_RESET_SOC,
	});
	if (wdt == NULL) {
		/* native_sim has no `alp-wdt0` DT alias, so this is the
		 * expected outcome in CI (board.yaml's `peripherals:
		 * watchdog` selects the real Zephyr backend, priority 100,
		 * over sw_fallback).  Keep going rather than exit here: the
		 * INTERRUPT_ONLY demo below is independent of this handle
		 * and must still run so CI actually exercises it instead of
		 * leaving it dead code behind an early return. */
		printf("[wdt] open failed: alp_last_error=%d "
		       "(expected NOT_READY = -2 on native_sim)\n",
		       (int)alp_last_error());
	} else {
		/* The feed loop.  In production this lives inside main()'s
	     * top-level loop (or, for cooperative-scheduled apps, inside
	     * the dispatcher's per-task budget).  Critically, it should
	     * NOT run from a high-priority kernel timer -- that would feed
	     * the watchdog regardless of whether the application's actual
	     * work is making progress. */
		for (int i = 0; i < 3; i++) {
			alp_status_t s = alp_wdt_feed(wdt);
			printf("[wdt] feed %d -> %d\n", i, (int)s);
			alp_delay_ms(FEED_PERIOD_MS);
		}

		/* Best-effort disable.  Many M-class watchdogs are
	     * write-once-armed in hardware -- once you call wdt_setup,
	     * you cannot turn it off without a reset.  ALP_ERR_NOSUPPORT
	     * is the expected return on those SoCs; treat as
	     * informational, not as failure. */
		alp_status_t s = alp_wdt_disable(wdt);
		printf("[wdt] disable -> %d (NOSUPPORT is OK on one-shot WDTs)\n", (int)s);

		/* close() releases the handle.  On hardware that can't disable
	     * the WDT, you MUST keep feeding from another thread or accept
	     * the reset.  Plan for this when designing the firmware --
	     * close() doesn't grant you a free pass. */
		alp_wdt_close(wdt);
	}

	/* ALP_WDT_INTERRUPT_ONLY fires an IRQ instead of resetting, and
     * (v0.17.0, #1637) can now notify the app via on_expire instead
     * of firing into nothing.  on_expire is REQUIRED for this mode --
     * alp_wdt_open() rejects INTERRUPT_ONLY with ALP_ERR_INVAL if
     * it's NULL, because an interrupt nobody can observe is worse
     * than not offering the mode at all.  On real silicon, reopening
     * ALP_E1M_WDT0 here (same wdt_id, right after the close() above)
     * is safe: the dispatcher's per-wdt_id exclusivity (#1650) already
     * guarantees no other handle owns it, and the Zephyr backend
     * reclaims a still-armed device from the prior handle
     * automatically.  On native_sim the first open above always fails
     * (no `alp-wdt0` DT alias), so this open independently hits the
     * exact same NOT_READY path -- proving it, not the reclaim. */
	int        cookie  = 42;
	alp_wdt_t *wdt_irq = alp_wdt_open(&(alp_wdt_config_t){
	    .wdt_id     = ALP_E1M_WDT0,
	    .timeout_ms = WDT_TIMEOUT_MS,
	    .on_timeout = ALP_WDT_INTERRUPT_ONLY,
	    .on_expire  = on_wdt_expire,
	    .user       = &cookie,
	});
	if (wdt_irq == NULL) {
		printf("[wdt] interrupt-only open failed: alp_last_error=%d "
		       "(expected NOT_READY = -2 on native_sim)\n",
		       (int)alp_last_error());
	} else {
		/* Feed once so the CI run never lets this deadline miss;
         * on_expire above is exercised on real silicon only. */
		alp_status_t feed_s = alp_wdt_feed(wdt_irq);
		printf("[wdt] interrupt-only feed -> %d\n", (int)feed_s);
		alp_wdt_close(wdt_irq);
		printf("[wdt] interrupt-only open+feed+close ok\n");
	}

	printf("[wdt] done\n");
	return 0;
}
