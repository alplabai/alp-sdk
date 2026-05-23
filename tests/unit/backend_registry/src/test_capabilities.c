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
    zassert_false(alp_capabilities_has(&c, ALP_INSTANCE_CAP_HW_TRIGGER));
}

ZTEST(alp_caps, test_has_returns_false_for_null_pointer)
{
    zassert_false(alp_capabilities_has(NULL, ALP_INSTANCE_CAP_DMA));
}

ZTEST(alp_caps, test_or_of_multiple_flags)
{
    alp_capabilities_t c = {
        .flags = ALP_INSTANCE_CAP_DMA | ALP_INSTANCE_CAP_HW_OVERSAMPLE,
    };
    zassert_true(alp_capabilities_has(&c, ALP_INSTANCE_CAP_DMA));
    zassert_true(alp_capabilities_has(&c, ALP_INSTANCE_CAP_HW_OVERSAMPLE));
    zassert_false(alp_capabilities_has(&c, ALP_INSTANCE_CAP_HW_TRIGGER));
}
