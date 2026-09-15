/* SPDX-License-Identifier: Apache-2.0
 *
 * Regression tests for issue #2144: the CC3501E GPIO proxy must refuse a
 * REVISION-DEPENDENT E1M pin (IO8/IO10/IO21) PER PIN, failing CLOSED,
 * instead of the old all-or-nothing hw_rev guard (#1859) that dropped
 * every proxied pin -- including revision-INDEPENDENT ones -- on a
 * mismatch, and stayed silent (fail-open) when the identity manifest
 * could not be read at all.
 *
 * Three surfaces are exercised:
 *
 *   1. cc3501e_proxy_hw_rev_confirmed_match() -- the pure decision
 *      src/backends/gpio/cc3501e_proxy.c splits out of
 *      alp_gpio_cc3501e_attach() specifically so it is testable without a
 *      real EEPROM (native_sim has no I2C EEPROM to back
 *      alp_hw_info_read()).  Driven both with hand-crafted
 *      alp_status_t/alp_hw_info_t inputs AND with CRC-valid
 *      alp_hw_info_eeprom_t buffers routed through the real
 *      alp_hw_info_classify_manifest() (built with the real
 *      alp_hw_info_crc32(), the same helper
 *      src/zephyr/hw_info_zephyr.c itself uses) -- covering every manifest
 *      state the issue calls out: matching, mismatched hw_rev, corrupt
 *      (bad schema_version or CRC32 -> ALP_ERR_IO), and missing/blank
 *      (ALP_ERR_NOT_PROVISIONED / NOSUPPORT).
 *
 *   2. px_open()'s per-pin gate itself, exercised through the portable
 *      alp_gpio_open() API -- proving the guard is actually WIRED into
 *      the backend, not just correct in isolation.  Uses the
 *      CONFIG_ZTEST-only cc3501e_proxy_test_force_hw_rev_confirmed_match()
 *      hook to set the cached decision directly instead of calling
 *      attach() (surface 3 below covers attach() itself).
 *
 *   3. alp_gpio_cc3501e_attach() -- does it actually SET the cached
 *      decision, not just correctly consult it once set?  A #2144 review
 *      mutation (hardcoding `g_hw_rev_confirmed_match = true;` in attach(),
 *      skipping the real read) passed all of surfaces 1+2 untouched, since
 *      neither ever calls attach() itself.  attach()'s ctx pointer is
 *      opaque (struct cc3501e is private to chips/cc3501e/) and is never
 *      dereferenced before the hw_info read runs, so a non-NULL sentinel
 *      that is deliberately never touched is the smallest honest seam here
 *      -- alp_gpio_cc3501e_attach(NULL) is a documented ALP_ERR_INVAL
 *      short-circuit that returns before touching the cached decision at
 *      all, so it cannot exercise this path. This test build's prj.conf
 *      enables CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID (needed for
 *      surface 1's real-classify cases above), but native_sim has no
 *      devicetree I2C0 device backing it, so the real alp_hw_info_read()
 *      genuinely fails at runtime -- attach() must reset the cached
 *      decision to false, not leave a stale forced-true value standing.
 *      The POSITIVE direction -- attach() turning a CONFIRMED manifest
 *      into g_hw_rev_confirmed_match=true -- is a second, distinct
 *      mutation gap: neither the negative test above nor surfaces 1+2 ever
 *      observe attach() itself PRODUCE a match. cc3501e_proxy_internal.h's
 *      cc3501e_proxy_test_inject_hw_info_read() (also CONFIG_ZTEST-only)
 *      arms a one-shot canned alp_hw_info_read() result for the next
 *      attach() call, so its real body -- the real
 *      cc3501e_proxy_hw_rev_confirmed_match() call and cache assignment,
 *      only the read's SOURCE swapped -- runs and IO8 opens.
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

#include "cc3501e_proxy_internal.h"
#include "hw_info_manifest.h" /* alp_hw_info_crc32(), alp_hw_info_classify_manifest() */

/* cc3501e_gpio_routes[]/cc3501e_gpio_unrouted[] stay the WEAK empty
 * defaults (no override in this TU) -- a "confirmed" IO8 open below falls
 * through to the platform delegate, so only the REFUSAL decision is under
 * test here, not the bridge routing path (already covered by the
 * generator/route-table tests). cc3501e_gpio_rev_dependent[] is NOT
 * overridable any more (issue #2144 design review): it is the SDK-owned
 * src/backends/gpio/cc3501e_rev_dependent_pins.c, always linked alongside
 * cc3501e_proxy.c, so this suite drives ALP_E1M_GPIO_IO8 -- one of its 3
 * real entries -- directly instead of declaring a local one-pin list. */

ZTEST_SUITE(alp_gpio_cc3501e_rev_guard, NULL, NULL, NULL, NULL, NULL);

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
/* Same pure decision, but through the REAL manifest classifier + the  */
/* REAL CRC routine -- not a hand-set alp_status_t/alp_hw_info_t.       */
/* Mirrors tests/zephyr/hw_info's make_valid_manifest() helper.         */
/* ------------------------------------------------------------------ */

static void make_valid_eeprom(alp_hw_info_eeprom_t *m, const char *hw_rev)
{
	memset(m, 0, sizeof(*m));
	m->magic          = ALP_HW_INFO_MAGIC;
	m->schema_version = ALP_HW_INFO_SCHEMA_VERSION;
	strcpy(m->family, "aen");
	strcpy(m->sku, "E1M-AEN801");
	strncpy(m->hw_rev, hw_rev, sizeof(m->hw_rev) - 1u);
	strcpy(m->serial, "ALP-AEN801-26W36-00003");
	m->mfg_year  = 2026;
	m->mfg_month = 6;
	m->mfg_day   = 23;
	m->crc32     = alp_hw_info_crc32((const uint8_t *)m, sizeof(*m) - sizeof(m->crc32));
}

ZTEST(alp_gpio_cc3501e_rev_guard, test_classify_valid_2626_r2_confirms)
{
	alp_hw_info_eeprom_t m;
	make_valid_eeprom(&m, "2626-r2");
	alp_hw_info_t info;
	memset(&info, 0, sizeof(info));

	alp_status_t rc = alp_hw_info_classify_manifest(&m, &info);
	zassert_equal(rc, ALP_OK);
	zassert_true(cc3501e_proxy_hw_rev_confirmed_match(rc, &info));
}

ZTEST(alp_gpio_cc3501e_rev_guard, test_classify_valid_2626_r1_does_not_confirm)
{
	/* CRC-valid, correctly classified -- just the OTHER AEN board rev. */
	alp_hw_info_eeprom_t m;
	make_valid_eeprom(&m, "2626-r1");
	alp_hw_info_t info;
	memset(&info, 0, sizeof(info));

	alp_status_t rc = alp_hw_info_classify_manifest(&m, &info);
	zassert_equal(rc, ALP_OK);
	zassert_false(cc3501e_proxy_hw_rev_confirmed_match(rc, &info));
}

ZTEST(alp_gpio_cc3501e_rev_guard, test_classify_bad_crc_does_not_confirm)
{
	/* Otherwise-valid "2626-r2" manifest, one flipped CRC32 word -- the
	 * bit-for-bit corruption case, not a hand-typed ALP_ERR_IO. */
	alp_hw_info_eeprom_t m;
	make_valid_eeprom(&m, "2626-r2");
	m.crc32 ^= 0xFFFFFFFFu;
	alp_hw_info_t info;
	memset(&info, 0, sizeof(info));

	alp_status_t rc = alp_hw_info_classify_manifest(&m, &info);
	zassert_equal(rc, ALP_ERR_IO);
	zassert_false(cc3501e_proxy_hw_rev_confirmed_match(rc, &info));
}

ZTEST(alp_gpio_cc3501e_rev_guard, test_classify_blank_eeprom_does_not_confirm)
{
	/* Factory-erased EEPROM: every byte 0xFF, no ALPH magic. */
	alp_hw_info_eeprom_t m;
	memset(&m, 0xFF, sizeof(m));
	alp_hw_info_t info;
	memset(&info, 0, sizeof(info));

	alp_status_t rc = alp_hw_info_classify_manifest(&m, &info);
	zassert_equal(rc, ALP_ERR_NOT_PROVISIONED);
	zassert_false(cc3501e_proxy_hw_rev_confirmed_match(rc, &info));
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

/* ------------------------------------------------------------------ */
/* alp_gpio_cc3501e_attach(): does it actually SET the cached decision, */
/* not just correctly consult it once set (surfaces 1+2 above)?         */
/* ------------------------------------------------------------------ */

ZTEST(alp_gpio_cc3501e_rev_guard, test_attach_resets_stale_forced_true_on_read_failure)
{
	/* Force a stale "confirmed" left over from an earlier test/hook use,
	 * then call the REAL attach() (not the hook) -- proving attach() itself
	 * re-derives the decision from a real alp_hw_info_read(), rather than
	 * leaving whatever the cache already held.  A mutated attach() that
	 * hardcodes `g_hw_rev_confirmed_match = true;` instead of assigning
	 * cc3501e_proxy_hw_rev_confirmed_match()'s result would leave this
	 * forced-true value standing and fail the zassert_is_null() below. */
	cc3501e_proxy_test_force_hw_rev_confirmed_match(true);

	/* struct cc3501e is opaque outside chips/cc3501e/ (no public
	 * definition), so a test cannot construct a real one on the stack;
	 * alp_gpio_cc3501e_attach(NULL) is a documented ALP_ERR_INVAL
	 * short-circuit that returns before touching the cached decision at
	 * all (see cc3501e_proxy.c), so it can't exercise this path either.
	 * attach()'s body only ever ASSIGNS g_bridge_ctx = ctx and never
	 * dereferences it before running the hw_info read below -- so a
	 * deliberately-never-dereferenced non-NULL sentinel is the smallest
	 * honest seam available.  This test never calls
	 * alp_gpio_configure()/write()/read() (the paths that DO dereference
	 * the bridge ctx), so the sentinel is safe here. */
	cc3501e_t *bogus_ctx_never_dereferenced = (cc3501e_t *)(uintptr_t)1;
	zassert_equal(alp_gpio_cc3501e_attach(bogus_ctx_never_dereferenced), ALP_OK);

	/* This test build's prj.conf enables
	 * CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID=0 (needed for the real-
	 * classify tests above), but native_sim has no devicetree I2C0 device
	 * backing it -- so attach()'s real alp_hw_info_read() genuinely fails,
	 * and IO8 must be refused, proving the forced-true value above did NOT
	 * survive attach(). */
	alp_gpio_t *h = alp_gpio_open(ALP_E1M_GPIO_IO8);
	zassert_is_null(h);
	zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);

	/* g_bridge_ctx now holds the bogus sentinel above -- reset it to NULL
	 * so a later test in this suite can never dereference it (it would
	 * only happen if a route table turned is_bridge true, which this
	 * suite's weak empty cc3501e_gpio_routes[] never does today, but the
	 * sentinel should not outlive the one test that needs it). */
	cc3501e_proxy_test_reset_bridge_ctx();
}

ZTEST(alp_gpio_cc3501e_rev_guard, test_attach_confirms_match_from_injected_manifest)
{
	/* The positive counterpart to the test above: does the REAL attach()
	 * body -- not the force hook -- turn a CONFIRMED manifest into
	 * g_hw_rev_confirmed_match=true?  native_sim still has no I2C EEPROM
	 * to back a real alp_hw_info_read(), so this arms the one-shot
	 * cc3501e_proxy_test_inject_hw_info_read() seam (declared in
	 * cc3501e_proxy_internal.h alongside the force hook): attach() still
	 * calls cc3501e_proxy_hw_rev_confirmed_match() and assigns its result
	 * for real, only alp_hw_info_read()'s SOURCE is swapped.
	 *
	 * Mutation-proof for the #2144 review finding: a mutant hardcoding
	 * `g_hw_rev_confirmed_match = false;` in attach() (instead of
	 * assigning the real decision) passed all 13 prior tests in this
	 * suite untouched, because none of them observed attach() PRODUCE a
	 * confirmed match -- test_rev_dependent_pin_opens_when_confirmed uses
	 * the force hook (bypasses attach() entirely), and
	 * test_attach_resets_stale_forced_true_on_read_failure above only
	 * proves attach() can turn confirmed INTO unconfirmed, not the
	 * reverse. This test closes that gap; hand-verified red against that
	 * exact mutant, then restored. */
	cc3501e_proxy_test_force_hw_rev_confirmed_match(false);

	alp_hw_info_t info = make_info(CONFIG_ALP_SDK_SOM_HW_REV);
	cc3501e_proxy_test_inject_hw_info_read(ALP_OK, &info);

	cc3501e_t *bogus_ctx_never_dereferenced = (cc3501e_t *)(uintptr_t)1;
	zassert_equal(alp_gpio_cc3501e_attach(bogus_ctx_never_dereferenced), ALP_OK);

	/* cc3501e_gpio_routes[] is empty (weak default) in this build, so a
	 * confirmed IO8 falls through the guard and DELEGATES to the platform
	 * driver -- proving the #2144 revision check passed, not that the pin
	 * reaches the bridge (already covered by
	 * test_rev_dependent_pin_opens_when_confirmed above). */
	alp_gpio_t *h = alp_gpio_open(ALP_E1M_GPIO_IO8);
	zassert_not_null(h);
	alp_gpio_close(h);

	cc3501e_proxy_test_reset_bridge_ctx();
}
