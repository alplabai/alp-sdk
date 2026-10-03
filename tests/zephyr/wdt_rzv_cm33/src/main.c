/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Build-only: arms the CM33 watchdog through the portable <alp/wdt.h> API.
 * Never run in CI -- a started WDT0 cannot be stopped and resets the whole SoM.
 * Bench steps: docs/hil/rzv-wdt0-cm33.md.
 */

#include <zephyr/kernel.h>

#include <alp/e1m_x_pinout.h>
#include <alp/wdt.h>

int main(void)
{
	alp_wdt_t *wdt = alp_wdt_open(&(alp_wdt_config_t){
	    .wdt_id     = ALP_E1M_X_WDT0,
	    .timeout_ms = 3000,
	    .on_timeout = ALP_WDT_RESET_SOC,
	});

	if (wdt == NULL) {
		printk("wdt: open failed\n");
		return 0;
	}

	while (1) {
		(void)alp_wdt_feed(wdt);
		k_sleep(K_MSEC(500));
	}

	return 0;
}
