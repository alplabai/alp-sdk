/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * wdt-feed — install a 5 second watchdog and feed it three times.
 * Demonstrates the open / feed / close contract.  The watchdog
 * is intentionally NOT exercised to its timeout in CI — the
 * console harness asserts the feed loop completes.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/wdt.h"

#define WDT_TIMEOUT_MS  5000
#define FEED_PERIOD_MS  500

int main(void) {
    printf("[wdt] open id=0 timeout=%u ms\n", WDT_TIMEOUT_MS);

    alp_wdt_t *wdt = alp_wdt_open(0, &(alp_wdt_config_t){
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

    for (int i = 0; i < 3; i++) {
        alp_status_t s = alp_wdt_feed(wdt);
        printf("[wdt] feed %d -> %d\n", i, (int)s);
        k_msleep(FEED_PERIOD_MS);
    }

    /* Best-effort disable; many M-class WDTs are write-once-armed. */
    alp_status_t s = alp_wdt_disable(wdt);
    printf("[wdt] disable -> %d (NOSUPPORT is OK on one-shot WDTs)\n",
           (int)s);

    alp_wdt_close(wdt);
    printf("[wdt] done\n");
    return 0;
}
