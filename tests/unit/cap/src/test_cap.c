/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ztest unit-test for alp_has() / alp_cap_name().  Runs on native_sim
 * under twister; selects ALIF_ENSEMBLE_E7 so we have a SoC whose
 * capability set is non-trivial.
 */

#include <stddef.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/cap.h>
#include <alp/soc_caps.h>

ZTEST_SUITE(alp_cap, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_cap, test_runtime_matches_compile_time_for_each_id) {
    /* Every ALP_CAP_ID_* must agree with the ALP_HAS() macro of the
     * same name. */
    zassert_equal((bool)ALP_HAS(HW_I2C), alp_has(ALP_CAP_ID_HW_I2C));
    zassert_equal((bool)ALP_HAS(HW_SPI), alp_has(ALP_CAP_ID_HW_SPI));
    zassert_equal((bool)ALP_HAS(NPU_DRPAI), alp_has(ALP_CAP_ID_NPU_DRPAI));
    zassert_equal((bool)ALP_HAS(HELIUM_MVE), alp_has(ALP_CAP_ID_HELIUM_MVE));
}

ZTEST(alp_cap, test_name_returns_expected_string) {
    zassert_str_equal(alp_cap_name(ALP_CAP_ID_HW_I2C), "HW_I2C");
    zassert_str_equal(alp_cap_name(ALP_CAP_ID_NPU_DRPAI), "NPU_DRPAI");
}

ZTEST(alp_cap, test_out_of_bounds_id_returns_safe_defaults) {
    zassert_false(alp_has(ALP_CAP_ID_COUNT));
    zassert_is_null(alp_cap_name(ALP_CAP_ID_COUNT));
}
