/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ztest unit-test for alp_status_name() / alp_status_description().  Runs
 * on native_sim under twister.  A second testcase.yaml variant builds this
 * same suite with CONFIG_ALP_STATUS_DESCRIPTIONS=n to exercise the
 * compiled-out fallback path.
 */

#include <stddef.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/peripheral.h>

ZTEST_SUITE(alp_status_strings, NULL, NULL, NULL, NULL, NULL);

/* Every declared status (ALP_OK down to ALP_STATUS_ENUM_FLOOR) must return
 * a non-NULL, non-empty name -- alp_status_name() is documented to never
 * return NULL. */
ZTEST(alp_status_strings, test_every_declared_status_has_a_nonempty_name)
{
	for (int v = (int)ALP_OK; v >= (int)ALP_STATUS_ENUM_FLOOR; v--) {
		const char *name = alp_status_name((alp_status_t)v);

		zassert_not_null(name, "status %d returned a NULL name", v);
		zassert_true(strlen(name) > 0, "status %d returned an empty name", v);
	}
}

ZTEST(alp_status_strings, test_name_specific_mappings)
{
	zassert_str_equal(alp_status_name(ALP_OK), "ALP_OK");
	zassert_str_equal(alp_status_name(ALP_ERR_INVAL), "ALP_ERR_INVAL");
	zassert_str_equal(alp_status_name(ALP_ERR_NOT_READY), "ALP_ERR_NOT_READY");
	zassert_str_equal(alp_status_name(ALP_ERR_NOT_FOUND), "ALP_ERR_NOT_FOUND");
}

ZTEST(alp_status_strings, test_out_of_range_value_returns_documented_fallback)
{
	/* One past ALP_OK (positive; the enum is entirely <= 0) and one past
     * the sentinel floor (more negative than any declared member) are
     * both outside ALP_STATUS_ENUM_FLOOR..ALP_OK. */
	const char *above = alp_status_name((alp_status_t)1);
	const char *below = alp_status_name((alp_status_t)((int)ALP_STATUS_ENUM_FLOOR - 1));

	zassert_not_null(above);
	zassert_not_null(below);
	zassert_str_equal(above, "ALP_STATUS_UNKNOWN");
	zassert_str_equal(below, "ALP_STATUS_UNKNOWN");
}

/* ALP_STATUS_ENUM_FLOOR is NOT a status (its own doc comment says so) and
 * numerically aliases ALP_ERR_NOT_PROVISIONED -- the sentinel must still
 * resolve sanely (never NULL/empty), not crash or read out of bounds. */
ZTEST(alp_status_strings, test_sentinel_value_handled_sanely)
{
	const char *name = alp_status_name(ALP_STATUS_ENUM_FLOOR);

	zassert_not_null(name);
	zassert_true(strlen(name) > 0);
	zassert_str_equal(name,
	                  alp_status_name(ALP_ERR_NOT_PROVISIONED),
	                  "the sentinel aliases ALP_ERR_NOT_PROVISIONED's value; "
	                  "the two must resolve to the same string");
}

ZTEST(alp_status_strings, test_description_never_null_or_empty)
{
	for (int v = (int)ALP_OK; v >= (int)ALP_STATUS_ENUM_FLOOR; v--) {
		const char *desc = alp_status_description((alp_status_t)v);

		zassert_not_null(desc, "status %d returned a NULL description", v);
		zassert_true(strlen(desc) > 0, "status %d returned an empty description", v);
	}

	/* Out-of-range also gets a documented fallback, never NULL. */
	const char *oor = alp_status_description((alp_status_t)1);

	zassert_not_null(oor);
	zassert_true(strlen(oor) > 0);
}

#if defined(CONFIG_ALP_STATUS_DESCRIPTIONS)

ZTEST(alp_status_strings, test_description_text_when_enabled)
{
	zassert_str_equal(alp_status_description(ALP_OK), "success");
	zassert_str_equal(alp_status_description(ALP_ERR_INVAL), "Invalid argument.");
}

#else /* !CONFIG_ALP_STATUS_DESCRIPTIONS */

ZTEST(alp_status_strings, test_description_text_when_disabled)
{
	/* Compiled-out build: still a real string, just not the per-status
     * text -- the same fallback for every status. */
	const char *ok  = alp_status_description(ALP_OK);
	const char *inv = alp_status_description(ALP_ERR_INVAL);

	zassert_str_equal(ok,
	                  inv,
	                  "with descriptions compiled out every status shares "
	                  "the one fallback string");
}

#endif /* CONFIG_ALP_STATUS_DESCRIPTIONS */
