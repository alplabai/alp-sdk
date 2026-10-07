/* SPDX-License-Identifier: Apache-2.0 */

#include <stddef.h>
#include <zephyr/ztest.h>
#include <alp/cap_instance.h>

ZTEST_SUITE(alp_caps, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_caps, test_has_returns_true_for_set_flag)
{
	alp_capabilities_t c = { .flags = ALP_INSTANCE_CAP_DMA };
	zassert_true(alp_capabilities_has(&c, ALP_INSTANCE_CAP_DMA));
}

ZTEST(alp_caps, test_has_returns_false_for_clear_flag)
{
	alp_capabilities_t c = { .flags = ALP_INSTANCE_CAP_DMA };
	zassert_false(alp_capabilities_has(&c, ALP_INSTANCE_CAP_REPORTED));
}

ZTEST(alp_caps, test_has_returns_false_for_null_pointer)
{
	zassert_false(alp_capabilities_has(NULL, ALP_INSTANCE_CAP_DMA));
}

ZTEST(alp_caps, test_or_of_multiple_flags)
{
	alp_capabilities_t c = {
		.flags = ALP_INSTANCE_CAP_DMA | ALP_INSTANCE_CAP_REPORTED,
	};
	zassert_true(alp_capabilities_has(&c, ALP_INSTANCE_CAP_DMA));
	zassert_true(alp_capabilities_has(&c, ALP_INSTANCE_CAP_REPORTED));
}

/* ---------- Reported/not-reported contract (Wave 1, #1640) --------------- */

ZTEST(alp_caps, test_zeroed_descriptor_reads_as_not_reported)
{
	/* An untouched (zero-initialized) descriptor -- exactly what a
     * backend "with nothing to say" must leave caps_out as -- reads as
     * "not reported", never as "affirmatively has nothing". */
	alp_capabilities_t c = { 0 };
	zassert_false(alp_capabilities_has(&c, ALP_INSTANCE_CAP_REPORTED));
	zassert_false(alp_capabilities_has(&c, ALP_INSTANCE_CAP_DMA));
	zassert_equal(c.class_flags, 0u);
}

ZTEST(alp_caps, test_reported_with_clear_class_bit_is_affirmative_absence)
{
	/* REPORTED set + a clear class bit is a definite "does not have
     * it", distinguishable from "not reported" only via REPORTED. */
	alp_capabilities_t c = {
		.flags       = ALP_INSTANCE_CAP_REPORTED,
		.class_flags = 0u, /* e.g. no ALP_ADC_CAP_* bits set */
	};
	zassert_true(alp_capabilities_has(&c, ALP_INSTANCE_CAP_REPORTED));
	zassert_equal(c.class_flags, 0u, "REPORTED + 0 class_flags == affirmatively absent");
}

ZTEST(alp_caps, test_reported_with_zero_channel_count_is_serves_none)
{
	/* channel_count == 0 alongside REPORTED means "serves none", as
     * distinct from "not reported" (channel_count == 0, REPORTED clear). */
	alp_capabilities_t reported_none = {
		.flags         = ALP_INSTANCE_CAP_REPORTED,
		.channel_count = 0u,
	};
	alp_capabilities_t not_reported = { 0 };

	zassert_true(alp_capabilities_has(&reported_none, ALP_INSTANCE_CAP_REPORTED));
	zassert_equal(reported_none.channel_count, 0u);

	zassert_false(alp_capabilities_has(&not_reported, ALP_INSTANCE_CAP_REPORTED));
	zassert_equal(not_reported.channel_count, 0u);
}
