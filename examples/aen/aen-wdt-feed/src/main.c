/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-wdt-feed -- install + feed the Ensemble E8 CMSDK watchdog on the
 * E1M-AEN801 (M55-HE), via the UPSTREAM Zephyr wdt_cmsdk_apb driver (Tier-1, no
 * vendoring -- "arm,cmsdk-watchdog").  Drives the standard Zephyr watchdog API
 * (wdt_install_timeout / wdt_setup / wdt_feed / wdt_disable) on
 * DT_ALIAS(alp_wdt0) = &wdog0.
 *
 * The watchdog is the safety belt: if the main loop hangs, the WDT resets the
 * SoC back to a known-good state.  This example installs a timeout, feeds it a
 * few times, then DISABLES it -- it never lets the timeout actually fire (a
 * reset would re-boot the SES and tear down the bench RAM-console read).
 *
 * PASS gate: device ready, wdt_install_timeout + wdt_setup + every wdt_feed
 * return 0, and wdt_disable returns 0 (the WDT was running and got stopped
 * cleanly).  NOTE: the exact timeout duration depends on the watchdog clock
 * rate, which is a TRM placeholder here -- the API path is what this proves.
 */

#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>

#define WDT_NODE       DT_ALIAS(alp_wdt0)
#define WDT_TIMEOUT_MS 8000
#define FEEDS          5
#define FEED_PERIOD_MS 300

int main(void)
{
	const struct device *wdt = DEVICE_DT_GET(WDT_NODE);

	printf("[wdt] open %s (CMSDK watchdog, timeout=%u ms)\n", wdt->name, WDT_TIMEOUT_MS);
	if (!device_is_ready(wdt)) {
		printf("[wdt] RESULT FAIL: device not ready\n[wdt] done\n");
		return 0;
	}

	struct wdt_timeout_cfg cfg = {
		.window   = { .min = 0, .max = WDT_TIMEOUT_MS },
		.callback = NULL,
		.flags    = WDT_FLAG_RESET_SOC,
	};
	int ch = wdt_install_timeout(wdt, &cfg);
	printf("[wdt] wdt_install_timeout -> %d\n", ch);
	if (ch < 0) {
		printf("[wdt] RESULT FAIL: install rc=%d\n[wdt] done\n", ch);
		return 0;
	}

	/* PAUSE_HALTED_BY_DBG so the SWD halt used to read the console doesn't
	 * trip the watchdog mid-debug. */
	int rc = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	printf("[wdt] wdt_setup -> %d\n", rc);
	if (rc != 0) {
		printf("[wdt] RESULT FAIL: setup rc=%d\n[wdt] done\n", rc);
		return 0;
	}

	int fed = 0;
	for (int i = 0; i < FEEDS; i++) {
		k_msleep(FEED_PERIOD_MS);
		rc = wdt_feed(wdt, ch);
		if (rc != 0) {
			printf("[wdt] wdt_feed[%d] -> %d\n", i, rc);
			break;
		}
		fed++;
		printf("[wdt] fed %d/%d\n", fed, FEEDS);
	}

	/* Stop the watchdog before main() returns so the SoC doesn't reset out
	 * from under the bench console read. */
	rc = wdt_disable(wdt);
	printf("[wdt] wdt_disable -> %d\n", rc);

	bool ok = (fed == FEEDS) && (rc == 0);
	printf("[wdt] RESULT %s: %s\n",
	       ok ? "PASS" : "PARTIAL",
	       ok ? "install + setup + feed x5 + disable all clean (exact timeout pends WDT clock rate)"
	          : "WDT path incomplete (see rc above)");
	printf("[wdt] done\n");
	return 0;
}
