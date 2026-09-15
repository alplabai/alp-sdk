/* SPDX-License-Identifier: Apache-2.0
 *
 * Regression tests for issue #2144: the CC3501E GPIO proxy must refuse a
 * REVISION-DEPENDENT E1M pin (IO8/IO10/IO21) PER PIN, failing CLOSED,
 * instead of the old all-or-nothing hw_rev guard (#1859) that dropped
 * every proxied pin -- including revision-INDEPENDENT ones -- on a
 * mismatch, and stayed silent (fail-open) when the identity manifest
 * could not be read at all.
 *
 * Two surfaces are exercised:
 *
 *   1. cc3501e_proxy_hw_rev_confirmed_match() -- the pure decision
 *      src/backends/gpio/cc3501e_proxy.c splits out of
 *      alp_gpio_cc3501e_attach() specifically so it is testable without a
 *      real EEPROM (native_sim has no I2C EEPROM to back
 *      alp_hw_info_read()).  Driven directly with crafted
 *      alp_status_t/alp_hw_info_t inputs covering all four manifest
 *      states the issue calls out: matching, mismatched hw_rev, corrupt
 *      (alp_hw_info_read() surfaces a bad schema_version/CRC32 as
 *      ALP_ERR_IO), and missing (ALP_ERR_NOT_PROVISIONED / NOSUPPORT).
 *
 *   2. px_open()'s per-pin gate itself, exercised through the portable
 *      alp_gpio_open() API -- proving the guard is actually WIRED into
 *      the backend, not just correct in isolation.  Since
 *      alp_gpio_cc3501e_attach() would call the real (I2C-backed)
 *      alp_hw_info_read(), which always returns NOSUPPORT on native_sim
 *      (no EEPROM bus configured), this half uses the CONFIG_ZTEST-only
 *      cc3501e_proxy_test_force_hw_rev_confirmed_match() hook to set the
 *      cached decision directly instead of calling attach().
 *
 * Strong override of the WEAK cc3501e_gpio_rev_dependent[] /
 * cc3501e_gpio_rev_dependent_count in cc3501e_proxy_routes_weak.c,
 * mirroring gpio_cc3501e_unrouted's override of cc3501e_gpio_unrouted[].
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
#include <alp/hw_info.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

/* Declared (non-static) in src/backends/gpio/cc3501e_proxy.c -- internal
 * test-visibility split for issue #2144, not part of the public
 * <alp/...> surface. */
extern bool cc3501e_proxy_hw_rev_confirmed_match(alp_status_t         read_status,
                                                 const alp_hw_info_t *info);
extern void cc3501e_proxy_test_force_hw_rev_confirmed_match(bool confirmed);

ZTEST_SUITE(alp_gpio_cc3501e_rev_guard, NULL, NULL, NULL, NULL, NULL);

/* Board-provided revision-dependent list (this test build's SoM pad map):
 * IO8 moves between the Alif SoC (r1) and the CC3501E (r2).
 * cc3501e_gpio_routes[] stays the WEAK empty default, so a "confirmed" IO8
 * open below falls through to the platform delegate -- only the REFUSAL
 * decision is under test here, not the bridge routing path (already
 * covered by the generator/route-table tests). */
const uint32_t cc3501e_gpio_rev_dependent[]     = { ALP_E1M_GPIO_IO8 };
const size_t   cc3501e_gpio_rev_dependent_count = 1u;

static alp_hw_info_t make_info(const char *hw_rev)
{
	alp_hw_info_t info;

	memset(&info, 0, sizeof(info));
	if (hw_rev != NULL) {
		strncpy(info.som_hw_rev, hw_rev, sizeof(info.som_hw_rev) - 1u);
	}
	return info;
}

/* ------------------------------------------------------------------ */
/* cc3501e_proxy_hw_rev_confirmed_match(): the pure decision.           */
/* ------------------------------------------------------------------ */

ZTEST(alp_gpio_cc3501e_rev_guard, test_confirmed_on_matching_manifest)
{
	alp_hw_info_t info = make_info(CONFIG_ALP_SDK_SOM_HW_REV);

	zassert_true(cc3501e_proxy_hw_rev_confirmed_match(ALP_OK, &info));
}

ZTEST(alp_gpio_cc3501e_rev_guard, test_not_confirmed_on_mismatched_hw_rev)
{
	/* A CRC-valid manifest (ALP_OK), but for the OTHER AEN revision. */
	alp_hw_info_t info = make_info("2626-r1");

	zassert_false(cc3501e_proxy_hw_rev_confirmed_match(ALP_OK, &info));
}

ZTEST(alp_gpio_cc3501e_rev_guard, test_not_confirmed_on_corrupt_manifest)
{
	/* alp_hw_info_read() reports ALP_ERR_IO for a magic-ok,
	 * schema_version/CRC32-bad manifest (alp_hw_info_classify_manifest(),
	 * src/zephyr/hw_info_zephyr.c) -- even if info->som_hw_rev happens to
	 * read back looking correct, the CRC failure means it cannot be
	 * trusted, so the status code alone must refuse the match. */
	alp_hw_info_t info = make_info(CONFIG_ALP_SDK_SOM_HW_REV);

	zassert_false(cc3501e_proxy_hw_rev_confirmed_match(ALP_ERR_IO, &info));
}

ZTEST(alp_gpio_cc3501e_rev_guard, test_not_confirmed_on_missing_manifest)
{
	/* ALP_ERR_NOT_PROVISIONED (blank/erased EEPROM) and ALP_ERR_NOSUPPORT
	 * (no EEPROM bus configured) both leave alp_hw_info_read()'s out-param
	 * zero-filled per its documented contract. */
	alp_hw_info_t info = make_info(NULL);

	zassert_false(cc3501e_proxy_hw_rev_confirmed_match(ALP_ERR_NOT_PROVISIONED, &info));
	zassert_false(cc3501e_proxy_hw_rev_confirmed_match(ALP_ERR_NOSUPPORT, &info));
}

/* ------------------------------------------------------------------ */
/* px_open()'s per-pin gate: proves the WIRING, not the decision.       */
/* ------------------------------------------------------------------ */

ZTEST(alp_gpio_cc3501e_rev_guard, test_selector_picks_cc3501e_proxy)
{
	/* The proxy (priority 200) must be the single backend every
	 * alp_gpio_open() call on this target funnels through -- otherwise
	 * the checks below wouldn't be exercising the shared chokepoint. */
	const alp_backend_t *be = alp_backend_select("gpio", ALP_SOC_REF_STR);
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "ti-cc3501e"), 0);
	zassert_equal(be->priority, 200);
}

ZTEST(alp_gpio_cc3501e_rev_guard, test_rev_dependent_pin_refused_when_not_confirmed)
{
	cc3501e_proxy_test_force_hw_rev_confirmed_match(false);

	alp_gpio_t *h = alp_gpio_open(ALP_E1M_GPIO_IO8);
	zassert_is_null(h);
	zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}

ZTEST(alp_gpio_cc3501e_rev_guard, test_rev_dependent_pin_opens_when_confirmed)
{
	/* cc3501e_gpio_routes[] is empty (weak default) in this build, so a
	 * "confirmed" IO8 falls through the guard and DELEGATES to the
	 * platform driver -- proving only that the guard stopped blocking
	 * it, not that the pin reaches the bridge (a separate, already-
	 * covered path). */
	cc3501e_proxy_test_force_hw_rev_confirmed_match(true);

	alp_gpio_t *h = alp_gpio_open(ALP_E1M_GPIO_IO8);
	zassert_not_null(h);
	alp_gpio_close(h);
}

ZTEST(alp_gpio_cc3501e_rev_guard, test_revision_independent_pin_opens_regardless)
{
	/* IO20 is NOT in cc3501e_gpio_rev_dependent[] above -- it must open
	 * whether or not the manifest is confirmed, proving the guard never
	 * shadows a revision-INDEPENDENT pin.  This is issue #2144's headline
	 * regression against the old #1859 guard, which dropped IO20 (the SD
	 * mux enable) right along with the genuinely revision-dependent
	 * pins on any hw_rev disagreement. */
	cc3501e_proxy_test_force_hw_rev_confirmed_match(false);

	alp_gpio_t *h = alp_gpio_open(ALP_E1M_GPIO_IO20);
	zassert_not_null(h);
	alp_gpio_close(h);
}
