/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include <zephyr/ztest.h>

#include <alp/peripheral.h>
#include <alp/update_log.h>

#include "../../../../src/update_log/engine.h"

ZTEST_SUITE(alp_update_log, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_update_log, test_entry_roundtrip)
{
    alp_update_log_entry_t e = {0};
    e.seq = 7; e.status = ALP_UPDATE_STATUS_CONFIRMED; e.timestamp = 1234;
    strcpy(e.fw_version, "1.2.3");
    uint8_t prev[32]; memset(prev, 0xAB, sizeof(prev));

    uint8_t wire[ULOG_ENTRY_WIRE_LEN];
    zassert_equal(ulog_entry_encode(&e, prev, wire), ALP_OK);

    alp_update_log_entry_t got = {0};
    uint8_t got_prev[32];
    zassert_equal(ulog_entry_decode(wire, sizeof(wire), &got, got_prev), ALP_OK);
    zassert_equal(got.seq, 7);
    zassert_equal(got.status, ALP_UPDATE_STATUS_CONFIRMED);
    zassert_equal(got.timestamp, 1234);
    zassert_equal(strcmp(got.fw_version, "1.2.3"), 0);
    zassert_mem_equal(got_prev, prev, 32);
}

ZTEST(alp_update_log, test_decode_short_buffer)
{
    uint8_t wire[ULOG_ENTRY_WIRE_LEN - 1] = {0};
    alp_update_log_entry_t got; uint8_t p[32];
    zassert_equal(ulog_entry_decode(wire, sizeof(wire), &got, p), ALP_ERR_INVAL);
}

ZTEST(alp_update_log, test_decode_bad_version)
{
    alp_update_log_entry_t e = {0}; e.seq = 1; strcpy(e.fw_version, "x");
    uint8_t prev[32] = {0}; uint8_t wire[ULOG_ENTRY_WIRE_LEN];
    (void)ulog_entry_encode(&e, prev, wire);
    wire[0] = 0xFF; wire[1] = 0xFF;  /* corrupt version */
    alp_update_log_entry_t got; uint8_t p[32];
    zassert_equal(ulog_entry_decode(wire, sizeof(wire), &got, p), ALP_ERR_VERSION);
}
