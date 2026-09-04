/* SPDX-License-Identifier: Apache-2.0
 *
 * One-handle-per-watchdog-instance tests for the WDT dispatcher
 * (issue #1637).
 *
 * Why the watchdog is exclusive where other classes are not: the
 * backend close path disables the whole DEVICE, not the handle's
 * channel (src/backends/wdt/zephyr_drv.c z_close calls
 * wdt_disable(dev), with the error (void)-cast away).  So two
 * subsystems each holding ALP_E1M_WDT0 means the first one to close
 * silently removes the second one's protection, with no error on any
 * path.  A device that believes it is watchdog-protected and is not is
 * worse than one with no watchdog, because nobody is looking.
 *
 * This build deliberately leaves CONFIG_WATCHDOG off so sw_fallback is
 * the selected backend and alp_wdt_open() really succeeds under
 * native_sim -- see the comment in prj.conf.  The rule under test lives
 * in src/wdt_dispatch.c, above the backend, so sw_fallback exercises it
 * faithfully.
 */

#include <stddef.h>

#include <zephyr/ztest.h>

#include <alp/peripheral.h>
#include <alp/wdt.h>

ZTEST_SUITE(alp_wdt_exclusivity, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_wdt_exclusivity, test_second_open_of_same_id_is_refused)
{
	alp_wdt_config_t cfg = ALP_WDT_CONFIG_DEFAULT(0u);

	alp_wdt_t *first = alp_wdt_open(&cfg);
	zassert_not_null(first, "sw_fallback must open on a CONFIG_WATCHDOG=n build");

	alp_wdt_t *second = alp_wdt_open(&cfg);
	zassert_is_null(second, "a second open of a live wdt_id must be refused");
	zassert_equal(alp_last_error(),
	              ALP_ERR_BUSY,
	              "the refusal must report ALP_ERR_BUSY, not a generic failure");

	alp_wdt_close(first);

	/* After the owner closes, the instance must be claimable again --
	 * exclusivity must not leak the instance for the run's lifetime. */
	alp_wdt_t *third = alp_wdt_open(&cfg);
	zassert_not_null(third, "the instance must be reusable after close");
	alp_wdt_close(third);
}

ZTEST(alp_wdt_exclusivity, test_distinct_ids_open_concurrently)
{
	/* Exclusivity is per INSTANCE, not a global one-watchdog rule:
	 * an SoM with two watchdogs must still hand out two handles. */
	alp_wdt_config_t cfg0 = ALP_WDT_CONFIG_DEFAULT(0u);
	alp_wdt_config_t cfg1 = ALP_WDT_CONFIG_DEFAULT(1u);

	alp_wdt_t *w0 = alp_wdt_open(&cfg0);
	zassert_not_null(w0);
	alp_wdt_t *w1 = alp_wdt_open(&cfg1);
	zassert_not_null(w1, "a different wdt_id must not be refused");

	alp_wdt_close(w0);
	alp_wdt_close(w1);
}
