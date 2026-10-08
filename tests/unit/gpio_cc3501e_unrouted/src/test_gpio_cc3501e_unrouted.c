/* SPDX-License-Identifier: Apache-2.0
 *
 * Regression test for issue #1854: an E1M pad the SoM preset marks
 * `dispatch: unrouted` (physically open on this hardware revision --
 * reaches neither the CC3501E mediator nor the Alif SoC) must refuse
 * alp_gpio_open() with ALP_ERR_NOSUPPORT instead of silently delegating
 * to the platform GPIO driver and opening a pin that goes nowhere.
 *
 * Strong override of the WEAK cc3501e_gpio_unrouted[] /
 * cc3501e_gpio_unrouted_count in src/backends/gpio/cc3501e_proxy.c --
 * mirrors AEN r2's real IO21 fact (metadata/e1m_modules/E1M-AEN801.yaml
 * pad_routes: `{ e1m: E1M_GPIO_IO21, dispatch: unrouted }`).
 *
 * boards/native_sim*.overlay provides a real alp,pin-array node so the
 * platform-delegate path can genuinely succeed -- without it, native_sim's
 * missing DT node would make delegation fail with ALP_ERR_INVAL regardless
 * of this fix, masking whether the new check is what actually refused the
 * open (known-bad-validation trap for issue #1854).
 *
 * Backends visible on this test build:
 *   cc3501e_proxy (priority 200, "*" wildcard -- wins the selector)
 *   zephyr_drv     (priority 100, "*" wildcard, reached via delegation)
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/chips/cc3501e.h>
#include <alp/e1m_pinout.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

ZTEST_SUITE(alp_gpio_cc3501e_unrouted, NULL, NULL, NULL, NULL, NULL);

/* Board-provided unrouted list (this test build's SoM pad map): IO21 is
 * open on AEN r2.  cc3501e_gpio_routes[] stays the WEAK empty default, so
 * every OTHER pin_id falls through to the platform delegate unchanged. */
const uint32_t cc3501e_gpio_unrouted[]     = { ALP_E1M_GPIO_IO21 };
const size_t   cc3501e_gpio_unrouted_count = 1u;

ZTEST(alp_gpio_cc3501e_unrouted, test_selector_picks_cc3501e_proxy)
{
	/* The proxy (priority 200) must be the single backend every
     * alp_gpio_open() call on this target funnels through -- otherwise
     * the check below wouldn't be exercising the shared chokepoint. */
	const alp_backend_t *be = alp_backend_select("gpio", ALP_SOC_REF_STR);
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "ti-cc3501e"), 0);
	zassert_equal(be->priority, 200);
}

ZTEST(alp_gpio_cc3501e_unrouted, test_unrouted_pin_refused_nosupport)
{
	alp_gpio_t *h = alp_gpio_open(ALP_E1M_GPIO_IO21);
	zassert_is_null(h);
	zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}

ZTEST(alp_gpio_cc3501e_unrouted, test_pin_not_in_unrouted_list_still_opens)
{
	/* IO20 is not in cc3501e_gpio_unrouted[] above, so it must fall
     * through to the platform delegate exactly as before this fix --
     * proving the new check doesn't shadow an ordinary pin. */
	alp_gpio_t *h = alp_gpio_open(ALP_E1M_GPIO_IO20);
	zassert_not_null(h);
	alp_gpio_close(h);
}
