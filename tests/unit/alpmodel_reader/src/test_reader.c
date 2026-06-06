/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/ztest.h>
#include <string.h>
#include "alp/model.h"
#include "fixture.h"

ZTEST_SUITE(alpmodel_reader, NULL, NULL, NULL, NULL, NULL);

ZTEST(alpmodel_reader, test_parse_minimal_fixture)
{
    alp_model_t m;
    zassert_equal(ALP_OK, alp_model_parse(alp_model_fixture, alp_model_fixture_len, &m));
    zassert_str_equal(m.name, "minimal");
    zassert_equal(m.n_targets, 2u, "expected 2 targets, got %u", m.n_targets);
    zassert_str_equal(m.targets[0].backend, "ethos_u");
    zassert_str_equal(m.targets[0].accel_config, "ethos-u85-256");
    zassert_equal(m.targets[0].arena_bytes, 65536u);
    zassert_equal(m.targets[0].req_sram_kib, 256u);
    zassert_str_equal(m.targets[1].backend, "cpu");
    zassert_equal(m.targets[0].blob_len, 8u);
    zassert_mem_equal(m.targets[0].blob, "VELA\x00\x01\x02\x03", 8);
}

ZTEST(alpmodel_reader, test_bad_magic_rejected)
{
    uint8_t buf[64];
    memcpy(buf, alp_model_fixture, sizeof(buf));
    buf[0] = 'Z';
    alp_model_t m;
    zassert_equal(ALP_ERR_INVAL, alp_model_parse(buf, sizeof(buf), &m));
}

ZTEST(alpmodel_reader, test_bad_version_rejected)
{
    uint8_t buf[64];
    memcpy(buf, alp_model_fixture, sizeof(buf));
    buf[4] = 99;
    alp_model_t m;
    zassert_equal(ALP_ERR_VERSION, alp_model_parse(buf, sizeof(buf), &m));
}

ZTEST(alpmodel_reader, test_oversized_blob_count_rejected)
{
    uint8_t buf[700];
    zassert_true(alp_model_fixture_len <= sizeof(buf), "buffer too small for fixture");
    memcpy(buf, alp_model_fixture, alp_model_fixture_len);
    /* blob_count is the u32 at header offset 20; make the blob table run past EOF */
    buf[20] = 0xFF;
    buf[21] = 0xFF;
    buf[22] = 0x00;
    buf[23] = 0x00; /* 65535 blobs */
    alp_model_t m;
    zassert_equal(ALP_ERR_INVAL, alp_model_parse(buf, alp_model_fixture_len, &m));
}
