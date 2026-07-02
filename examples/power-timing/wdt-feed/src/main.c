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
 */

#include <stdio.h>

#include "alp/peripheral.h"

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

int main(void)
{
	printf("[wdt] open id=0 timeout=%u ms\n", WDT_TIMEOUT_MS);

	/* on_timeout = ALP_WDT_RESET_SOC asks for a full SoC reset on
     * miss-feed.  Alternatives are RESET_CPU (core reset only,
     * peripherals retain state -- useful for soft-fault recovery)
     * and INTERRUPT_ONLY (fires an IRQ; you'd capture state in the
     * handler before manually triggering a reset). */
	alp_wdt_t *wdt = alp_wdt_open(0,
	                              &(alp_wdt_config_t){
	                                  .timeout_ms = WDT_TIMEOUT_MS,
	                                  .on_timeout = ALP_WDT_RESET_SOC,
	                              });
	if (wdt == NULL) {
		printf("[wdt] open failed: alp_last_error=%d "
		       "(expected NOT_READY = -2 on native_sim)\n",
		       (int)alp_last_error());
		printf("[wdt] done\n");
		return 0;
	}

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
	printf("[wdt] done\n");
	return 0;
}
