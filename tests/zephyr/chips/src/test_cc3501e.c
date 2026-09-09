/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e -- TI Wi-Fi 6 + BLE 5.4 coprocessor (E1M-AEN companion MCU).
 * NULL-arg validation + post-init NOT_READY contract, plus (#2035) the
 * all-zero dead-phase guard's opcode classification and the GET_MAC
 * validity check.
 *
 * #2035 note on WHAT this file can exercise: this test app (tests/zephyr/
 * chips) wires no SPI bus for cc3501e (see test_cc3501e_init_null_args'
 * comment below -- "the test rig's SPI emul may or may not be available"),
 * unlike tests/zephyr/cc3501e_host_driver, which runs the real driver
 * against a software model of the firmware SPI slave.  That means the
 * FULL request/reply path (cc3501e_request_locked() in cc3501e_core.c)
 * cannot be driven end-to-end from here.  What CAN be exercised directly,
 * with no bus at all, is the exact decision logic that path defers to --
 * cc3501e_reply_may_be_all_zero() and cc3501e_mac_is_valid() -- which is
 * what determines ALP_OK vs ALP_ERR_IO for an all-zero reply.  Both are
 * declared `bool` (not `static`) in chips/cc3501e/cc3501e_internal.h
 * precisely for this. */

#include <zephyr/ztest.h>

#include "alp/chips/cc3501e.h"
#include "alp/e1m_pinout.h"
#include "alp/peripheral.h"

/* Relative include, not a new -I: chips/cc3501e/'s own internal header,
 * the same "expose one internal helper for testing" pattern already used
 * by chips/da9292/da9292_internal.h + chips/bme280/bme280_internal.h (see
 * this app's CMakeLists.txt). */
#include "../../../../chips/cc3501e/cc3501e_internal.h"

ZTEST(alp_chips, test_cc3501e_init_null_args)
{
	cc3501e_t  ctx;
	alp_spi_t *bus = alp_spi_open(&(alp_spi_config_t){
	    .bus_id        = 0u,
	    .freq_hz       = 10000000u,
	    .mode          = ALP_SPI_MODE_0,
	    .bits_per_word = 8u,
	    .cs_pin_id     = 0u,
	});
	/* The test rig's SPI emul may or may not be available; either
     * way the NULL-arg paths are testable. */
	zassert_equal(cc3501e_init(NULL, bus), ALP_ERR_INVAL);
	zassert_equal(cc3501e_init(&ctx, NULL), ALP_ERR_INVAL);
	if (bus != NULL) alp_spi_close(bus);
}

ZTEST(alp_chips, test_cc3501e_calls_reject_uninitialised)
{
	cc3501e_t ctx = { 0 };
	uint16_t  version;
	uint8_t   tx[4]  = { 0 }, rx[4];
	size_t    rx_len = sizeof rx;

	zassert_equal(cc3501e_reset(&ctx), ALP_ERR_NOT_READY);
	zassert_equal(cc3501e_get_version(&ctx, &version), ALP_ERR_NOT_READY);
	zassert_equal(
	    cc3501e_request(&ctx, (alp_cc3501e_cmd_t)0, tx, sizeof tx, rx, sizeof rx, &rx_len, 100u),
	    ALP_ERR_NOT_READY);
	zassert_equal(cc3501e_add_event_callback(&ctx, NULL, NULL), ALP_ERR_NOT_READY);
}

/* #2035: opcodes whose reply payload can NEVER legitimately be all-zero must
 * stay PROTECTED -- i.e. cc3501e_reply_may_be_all_zero() must say false for
 * them, so cc3501e_request_locked() treats an all-zero header+payload as the
 * #1378 dead-phase alias and returns ALP_ERR_IO instead of ALP_OK.
 *
 * GET_MAC is the opcode #2035 is actually about: cc3501e_wifi_get_mac()
 * previously had NOTHING here to catch a dead phase that reads back
 * status=0x00 (RESP_OK) + mac[0..5]=0x00 -- the exact silicon-observed bug
 * (00:00:00:00:00:00 reported as a successful read).  GET_VERSION /
 * GET_CAPABILITIES / GET_DIAG_INFO are the "other identity-style getters"
 * with the same shape the task calls out; none of the three is listed as an
 * exemption, so a header that read intact followed by an all-zero payload
 * phase is rejected for them too. */
ZTEST(alp_chips, test_cc3501e_dead_phase_protects_identity_getters)
{
	zassert_false(cc3501e_reply_may_be_all_zero(ALP_CC3501E_CMD_GET_MAC),
	              "GET_MAC: all-zero reply must be treated as a dead phase");
	zassert_false(cc3501e_reply_may_be_all_zero(ALP_CC3501E_CMD_GET_VERSION),
	              "GET_VERSION: all-zero reply must be treated as a dead phase");
	zassert_false(cc3501e_reply_may_be_all_zero(ALP_CC3501E_CMD_GET_CAPABILITIES),
	              "GET_CAPABILITIES: all-zero reply must be treated as a dead phase");
	zassert_false(cc3501e_reply_may_be_all_zero(ALP_CC3501E_CMD_GET_DIAG_INFO),
	              "GET_DIAG_INFO: all-zero reply must be treated as a dead phase");
}

/* #2035: the flip side -- opcodes named in cc3501e_core.c's own comments (or
 * the protocol header's) as having a genuinely, legitimately all-zero reply
 * must stay exempt, or this change would trade the #1378 false ALP_OK for a
 * routine false ALP_ERR_IO on paths that are correct today (the exact
 * regression the task warns against). */
ZTEST(alp_chips, test_cc3501e_dead_phase_exempts_documented_legitimate_zero_replies)
{
	zassert_true(cc3501e_reply_may_be_all_zero(ALP_CC3501E_CMD_WIFI_STATUS),
	             "WIFI_STATUS: disconnected-and-never-attempted is a real all-zero state");
	zassert_true(cc3501e_reply_may_be_all_zero(ALP_CC3501E_CMD_WIFI_GET_RSSI),
	             "WIFI_GET_RSSI: 0 dBm is a legal reading");
	zassert_true(cc3501e_reply_may_be_all_zero(ALP_CC3501E_CMD_WIFI_GET_IP),
	             "WIFI_GET_IP: 0.0.0.0 legitimately means no IP assigned");
	zassert_true(cc3501e_reply_may_be_all_zero(ALP_CC3501E_CMD_DIAG_GET_STATS),
	             "DIAG_GET_STATS: zero counters right after boot are real");
	zassert_true(cc3501e_reply_may_be_all_zero(ALP_CC3501E_CMD_SOCK_RECV),
	             "SOCK_RECV: zero-bytes-pending is documented normal behaviour");
	zassert_true(cc3501e_reply_may_be_all_zero(ALP_CC3501E_CMD_OTA_STATUS),
	             "OTA_STATUS: no OTA session ever run is a real all-zero state");
	zassert_true(cc3501e_reply_may_be_all_zero(ALP_CC3501E_CMD_OTA_UPDATE_MODE),
	             "OTA_UPDATE_MODE (0x47): mode==0 is documented as not structurally "
	             "defeatable -- must stay exempt, see <alp/protocol/cc3501e.h>");
	zassert_true(cc3501e_reply_may_be_all_zero(ALP_CC3501E_CMD_GPIO_READ),
	             "GPIO_READ: a pad sampled LOW is an ordinary reading");
	zassert_true(cc3501e_reply_may_be_all_zero(ALP_CC3501E_CMD_SPI1_TRANSFER),
	             "SPI1_TRANSFER: a standalone CS-deassert (len=flags=seq=0) is documented "
	             "normal usage");
}

/* #2035: cc3501e_wifi_get_mac()'s own validity check.  Simulates exactly the
 * "valid header followed by an all-zero payload" wire scenario the task
 * asks for, at the boundary this test app can actually reach without an
 * emulated SPI bus: the MAC bytes cc3501e_request_locked() would have
 * handed cc3501e_wifi_get_mac() had it not already been rejected upstream. */
ZTEST(alp_chips, test_cc3501e_mac_is_valid_rejects_all_zero)
{
	const uint8_t dead_phase_mac[CC3501E_MAC_LEN] = { 0, 0, 0, 0, 0, 0 };

	zassert_false(cc3501e_mac_is_valid(dead_phase_mac),
	              "an all-zero MAC must never be accepted as a real station address");
}

ZTEST(alp_chips, test_cc3501e_mac_is_valid_rejects_group_bit)
{
	/* mac[0] bit 0 set = IEEE group/multicast bit -- never legal on a real
	 * station's individual (unicast) address, even though this is NOT
	 * all-zero and so would sail past a pure zero check. */
	const uint8_t group_bit_mac[CC3501E_MAC_LEN] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };

	zassert_false(cc3501e_mac_is_valid(group_bit_mac),
	              "a MAC with the IEEE group bit set must never be accepted");
}

ZTEST(alp_chips, test_cc3501e_mac_is_valid_accepts_real_unicast_mac)
{
	/* A real TI OUI-prefixed, individual-address MAC -- bit 0 of the first
     * octet clear, not all-zero.  Must NOT be rejected: the whole point of
     * this change is to reject the dead-phase alias without breaking a
     * genuine successful read. */
	const uint8_t real_mac[CC3501E_MAC_LEN] = { 0x00, 0x18, 0x02, 0xAA, 0xBB, 0xCC };

	zassert_true(cc3501e_mac_is_valid(real_mac), "a real unicast MAC must be accepted");
}
