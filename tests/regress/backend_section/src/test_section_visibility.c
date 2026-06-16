/* SPDX-License-Identifier: Apache-2.0
 *
 * Guards against linker scripts (or LTO settings) that silently
 * drop the .alp_backends_* sections.  Registers a sentinel backend
 * for a fictional class "regress_sentinel" and asserts the
 * selector finds it back.
 *
 * If this test ever fails on a new toolchain, the registry pattern
 * is broken on that toolchain -- triage the linker script before
 * the next subsystem migration lands.
 */

#include <string.h>
#include <zephyr/ztest.h>
#include <alp/backend.h>

ALP_BACKEND_REGISTER(regress_sentinel, alif,
                     {
                         .silicon_ref = "alif:ensemble:e7",
                         .vendor      = "alif",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = NULL,
                         .probe       = NULL,
                     });

ALP_BACKEND_DEFINE_CLASS(regress_sentinel);

ZTEST_SUITE(alp_backend_section, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_backend_section, test_sentinel_is_visible_to_selector)
{
	const alp_backend_t *be = alp_backend_select("regress_sentinel", "alif:ensemble:e7");
	zassert_not_null(be, "sentinel backend not found -- linker stripped the section");
	zassert_equal(strcmp(be->vendor, "alif"), 0);
	zassert_equal(be->priority, 100);
}

ZTEST(alp_backend_section, test_count_reflects_section)
{
	zassert_equal(alp_backend_count("regress_sentinel"), 1u);
}

ZTEST(alp_backend_section, test_select_returns_null_for_unknown_class)
{
	zassert_is_null(alp_backend_select("does_not_exist", "alif:ensemble:e7"));
}

ZTEST(alp_backend_section, test_select_returns_null_for_unknown_silicon)
{
	zassert_is_null(alp_backend_select("regress_sentinel", "fictional:soc:zz"));
}
