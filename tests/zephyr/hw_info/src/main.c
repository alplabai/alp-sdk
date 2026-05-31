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
#include "hw_info_manifest.h"

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
    alp_hw_info_t info = { 0 };
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

/* ---- alp_hw_info_classify_manifest: the EEPROM-authoritative path ---- */

static void make_valid_manifest(alp_hw_info_eeprom_t *m)
{
    memset(m, 0, sizeof(*m));
    m->magic          = ALP_HW_INFO_MAGIC;
    m->schema_version = ALP_HW_INFO_SCHEMA_VERSION;
    strcpy(m->family, "v2n");
    strcpy(m->sku, "E1M-V2N101");
    strcpy(m->hw_rev, "r1");
    strcpy(m->serial, "ALP-V2N101-26W19-00042");
    m->mfg_year  = 2026;
    m->mfg_month = 5;
    m->mfg_day   = 9;
    m->crc32     = alp_hw_info_crc32((const uint8_t *)m, sizeof(*m) - sizeof(m->crc32));
}

ZTEST(alp_hw_info, test_classify_valid_returns_ok)
{
    alp_hw_info_eeprom_t m;
    make_valid_manifest(&m);
    alp_hw_info_t info;
    memset(&info, 0, sizeof(info));
    zassert_equal(alp_hw_info_classify_manifest(&m, &info), ALP_OK);
    zassert_str_equal(info.som_family, "v2n");
    zassert_str_equal(info.som_sku, "E1M-V2N101");
    zassert_str_equal(info.som_hw_rev, "r1");
    zassert_str_equal(info.som_serial, "ALP-V2N101-26W19-00042");
    zassert_equal(info.som_mfg_year, 2026);
    zassert_equal(info.som_mfg_month, 5);
    zassert_equal(info.som_mfg_day, 9);
}

ZTEST(alp_hw_info, test_classify_blank_eeprom_returns_not_provisioned)
{
    alp_hw_info_eeprom_t m;
    memset(&m, 0xFF, sizeof(m)); /* erased flash/EEPROM pattern */
    alp_hw_info_t info;
    memset(&info, 0, sizeof(info));
    zassert_equal(alp_hw_info_classify_manifest(&m, &info), ALP_ERR_NOT_PROVISIONED);
    zassert_equal(info.som_hw_rev[0], 0); /* out untouched on failure */
}

ZTEST(alp_hw_info, test_classify_zeroed_eeprom_returns_not_provisioned)
{
    alp_hw_info_eeprom_t m;
    memset(&m, 0, sizeof(m)); /* freshly zeroed, no magic */
    alp_hw_info_t info;
    memset(&info, 0, sizeof(info));
    zassert_equal(alp_hw_info_classify_manifest(&m, &info), ALP_ERR_NOT_PROVISIONED);
}

ZTEST(alp_hw_info, test_classify_bad_schema_returns_io)
{
    alp_hw_info_eeprom_t m;
    make_valid_manifest(&m);
    m.schema_version = 99u; /* magic OK, body wrong */
    /* Recompute a *valid* CRC so this proves the schema check trips
     * first (before CRC) -- a good checksum must not rescue a bad schema. */
    m.crc32 = alp_hw_info_crc32((const uint8_t *)&m, sizeof(m) - sizeof(m.crc32));
    alp_hw_info_t info;
    memset(&info, 0, sizeof(info));
    zassert_equal(alp_hw_info_classify_manifest(&m, &info), ALP_ERR_IO);
}

ZTEST(alp_hw_info, test_classify_bad_crc_returns_io)
{
    alp_hw_info_eeprom_t m;
    make_valid_manifest(&m);
    m.crc32 ^= 0xFFFFFFFFu; /* corrupt the checksum */
    alp_hw_info_t info;
    memset(&info, 0, sizeof(info));
    zassert_equal(alp_hw_info_classify_manifest(&m, &info), ALP_ERR_IO);
}

ZTEST(alp_hw_info, test_classify_null_args_return_inval)
{
    alp_hw_info_eeprom_t m;
    make_valid_manifest(&m);
    alp_hw_info_t info;
    zassert_equal(alp_hw_info_classify_manifest(NULL, &info), ALP_ERR_INVAL);
    zassert_equal(alp_hw_info_classify_manifest(&m, NULL), ALP_ERR_INVAL);
}
