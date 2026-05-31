/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smoke tests for <alp/hw_info.h> under native_sim.  v0.3 ships
 * the API contract; both entry points must return NOSUPPORT, the
 * out-struct must be zeroed, and NULL-arg validation must catch
 * invalid callers before they reach the stub.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "alp/hw_info.h"
#include "alp/peripheral.h"

ZTEST_SUITE(alp_hw_info, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_hw_info, test_read_null_out_returns_inval)
{
    zassert_equal(alp_hw_info_read(NULL), ALP_ERR_INVAL);
}

ZTEST(alp_hw_info, test_read_returns_nosupport_v03)
{
    /* Pre-fill the struct with a sentinel pattern so we can prove
     * the stub zeroes it before returning NOSUPPORT. */
    alp_hw_info_t info;
    memset(&info, 0xAA, sizeof(info));
    zassert_equal(alp_hw_info_read(&info), ALP_ERR_NOSUPPORT);

    /* All fields should now read as zero/empty.  Spot-check the
     * key strings + the ADC mV fields. */
    zassert_equal(info.som_family[0], 0);
    zassert_equal(info.som_sku[0], 0);
    zassert_equal(info.som_hw_rev[0], 0);
    zassert_equal(info.som_board_id_mv, 0);
    zassert_equal(info.board_hw_rev[0], 0);
    zassert_equal(info.board_id_mv, 0);
}

ZTEST(alp_hw_info, test_assert_null_info_returns_inval)
{
    zassert_equal(alp_hw_info_assert_matches_build(NULL, "E1M-AEN701", "r1"), ALP_ERR_INVAL);
}

ZTEST(alp_hw_info, test_assert_returns_nosupport_v03)
{
    alp_hw_info_t info = {0};
    zassert_equal(alp_hw_info_assert_matches_build(&info, "E1M-AEN701", "r1"), ALP_ERR_NOSUPPORT);
    /* NULL expected_* arguments are valid (skip-the-check semantics)
     * and must still report NOSUPPORT, not INVAL. */
    zassert_equal(alp_hw_info_assert_matches_build(&info, NULL, NULL), ALP_ERR_NOSUPPORT);
}

ZTEST(alp_hw_info, test_eeprom_manifest_is_exactly_128_bytes)
{
    /* The Python programmer tool and the C reader BOTH depend on
     * this exact size; production-test binaries written from one
     * end must line up byte-for-byte with the other.  The header's
     * _Static_assert covers this at compile time; this ztest
     * surfaces a clearer failure if anyone ever drops the assert. */
    zassert_equal(sizeof(alp_hw_info_eeprom_t), 128u);
    zassert_equal(ALP_HW_INFO_MAGIC, 0x414C5048u);
    zassert_equal(ALP_HW_INFO_SCHEMA_VERSION, 1u);
}
