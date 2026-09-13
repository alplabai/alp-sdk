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
#include "alp/protocol/crc16.h"

/* Relative include, not a new -I: chips/cc3501e/'s own internal header,
 * the same "expose one internal helper for testing" pattern already used
 * by chips/da9292/da9292_internal.h + chips/bme280/bme280_internal.h (see
 * this app's CMakeLists.txt). */
#include "../../../../chips/cc3501e/cc3501e_internal.h"

/* #2035: examples/aen/aen-evk-demo's GET_VERSION link verdict, pulled into
 * its own header precisely so it can be exercised here without a board --
 * see that header for the bilingual-host reasoning. Reachable via this
 * app's existing examples/aen/aen-evk-demo/src include dir (see
 * bmp581_verdict.h's comment in this app's CMakeLists.txt). */
#include "cc3501e_link_verdict.h"

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

/* ------------------------------------------------------------------ */
/* v4.0 (#2035): the wire-MAJOR-bump gate + reply decode.  Both
 * cc3501e_fw_major_is_acceptable() and cc3501e_reply_verdict() are pure
 * (no ctx, no I/O), so -- like cc3501e_reply_may_be_all_zero() above --
 * they are directly testable with fabricated frames and no SPI bus.
 * ------------------------------------------------------------------ */

ZTEST(alp_chips, test_cc3501e_fw_major_gate_accepts_3_and_4_refuses_rest)
{
	zassert_true(cc3501e_fw_major_is_acceptable(3u),
	             "MAJOR 3 (legacy, migration window) must pass");
	zassert_true(cc3501e_fw_major_is_acceptable(4u), "MAJOR 4 (this driver's own wire) must pass");
	zassert_false(cc3501e_fw_major_is_acceptable(0u),
	              "MAJOR 0 (pre-ADR-0033 raw-integer legacy) must be refused");
	zassert_false(cc3501e_fw_major_is_acceptable(1u), "MAJOR 1 was never a real wire MAJOR");
	zassert_false(cc3501e_fw_major_is_acceptable(2u), "MAJOR 2 was never a real wire MAJOR");
	zassert_false(cc3501e_fw_major_is_acceptable(5u), "MAJOR 5 does not exist yet");
	zassert_false(cc3501e_fw_major_is_acceptable(255u),
	              "an arbitrary garbage MAJOR must be refused");
}

/* Fill @p out[0..3] with a reply header for @p cmd; the reply payload_len
 * field (bytes 2..3, LE) is not consulted by cc3501e_reply_verdict() -- only
 * the header's role as CRC-covered bytes matters here -- so it is left 0. */
static void fill_reply_hdr(uint8_t out[ALP_CC3501E_HEADER_BYTES], alp_cc3501e_cmd_t cmd)
{
	out[0] = (uint8_t)cmd;
	out[1] = ALP_CC3501E_FLAG_RESP_REQUIRED;
	out[2] = 0u;
	out[3] = 0u;
}

/* Append a valid CRC-16/CCITT-FALSE trailer to @p payload (which already
 * holds @p unpadded_len real bytes) and return the total length.  Uses the
 * SAME shared alp_crc16_ccitt_false() the production code calls -- this
 * proves cc3501e_reply_verdict() accepts a CORRECTLY computed trailer, not
 * that it accepts some independently-invented value. */
static uint16_t append_valid_crc(const uint8_t reply_hdr[ALP_CC3501E_HEADER_BYTES],
                                 uint8_t      *payload,
                                 uint16_t      unpadded_len)
{
	uint16_t crc               = alp_crc16_ccitt_false(reply_hdr, ALP_CC3501E_HEADER_BYTES);
	crc                        = alp_crc16_ccitt_false_update(crc, payload, unpadded_len);
	payload[unpadded_len]      = (uint8_t)(crc & 0xFFu);
	payload[unpadded_len + 1u] = (uint8_t)((crc >> 8) & 0xFFu);
	return (uint16_t)(unpadded_len + ALP_CC3501E_CRC_BYTES);
}

/* Shape-tolerant GET_VERSION: the LEGACY wire shape (status
 * ALP_CC3501E_RESP_OK_LEGACY, 2 data bytes, no CRC) must decode to ALP_OK at
 * fw_proto_major == 0 -- the state cc3501e_get_version() runs in from inside
 * cc3501e_reset(), before the gate has run. */
ZTEST(alp_chips, test_cc3501e_reply_verdict_get_version_legacy_shape)
{
	uint8_t hdr[ALP_CC3501E_HEADER_BYTES];
	fill_reply_hdr(hdr, ALP_CC3501E_CMD_GET_VERSION);
	uint8_t payload[3] = { ALP_CC3501E_RESP_OK_LEGACY, 3u, 1u }; /* legacy MAJOR.MINOR = 3.1 */

	zassert_equal(cc3501e_reply_verdict(ALP_CC3501E_CMD_GET_VERSION, 0u, hdr, payload, 3u),
	              ALP_OK,
	              "legacy (0x00, no CRC) GET_VERSION reply must decode OK pre-gate");
}

/* Shape-tolerant GET_VERSION: the 4.0 wire shape (status ALP_CC3501E_RESP_OK,
 * 2 data bytes, valid CRC) must ALSO decode to ALP_OK at fw_proto_major == 0
 * -- same pre-gate call, opposite dialect. */
ZTEST(alp_chips, test_cc3501e_reply_verdict_get_version_v4_shape)
{
	uint8_t hdr[ALP_CC3501E_HEADER_BYTES];
	fill_reply_hdr(hdr, ALP_CC3501E_CMD_GET_VERSION);
	uint8_t  payload[5] = { ALP_CC3501E_RESP_OK, 4u, 0u, 0u, 0u }; /* MAJOR.MINOR = 4.0 */
	uint16_t len        = append_valid_crc(hdr, payload, 3u);

	zassert_equal(len, 5u, "3 real bytes + 2 CRC bytes");
	zassert_equal(cc3501e_reply_verdict(ALP_CC3501E_CMD_GET_VERSION, 0u, hdr, payload, len),
	              ALP_OK,
	              "4.0 (0x5A + valid CRC) GET_VERSION reply must decode OK pre-gate");
}

/* THE LANDMINE (see <alp/protocol/cc3501e.h> and cc3501e_core.c): an
 * all-zero dead SPI phase on GET_VERSION, seen with fw_proto_major still 0
 * (i.e. from inside cc3501e_reset(), before the gate runs), must NOT decode
 * as a legitimate legacy MAJOR-0 reply.  If it did, cc3501e_reset() would
 * read fw_major == 0, permanently clear ctx->initialised and report
 * ALP_ERR_VERSION for a transient transport hiccup -- stranding the fleet
 * silently, exactly the failure the task calls out. */
ZTEST(alp_chips, test_cc3501e_reply_verdict_get_version_dead_phase_is_not_major_zero)
{
	uint8_t hdr[ALP_CC3501E_HEADER_BYTES];
	fill_reply_hdr(hdr, ALP_CC3501E_CMD_GET_VERSION);
	uint8_t payload[3] = { 0x00u, 0x00u, 0x00u }; /* dead phase: every byte reads back 0x00 */

	zassert_equal(cc3501e_reply_verdict(ALP_CC3501E_CMD_GET_VERSION, 0u, hdr, payload, 3u),
	              ALP_ERR_IO,
	              "an all-zero GET_VERSION reply must be rejected as a dead phase, "
	              "never accepted as legacy MAJOR 0");
}

/* A major-3 (legacy) firmware round trip: bare RESP_OK_LEGACY, no CRC, on an
 * ALREADY-GATED (fw_proto_major == 3) context. */
ZTEST(alp_chips, test_cc3501e_reply_verdict_major3_round_trip)
{
	uint8_t hdr[ALP_CC3501E_HEADER_BYTES];
	fill_reply_hdr(hdr, ALP_CC3501E_CMD_PING);
	uint8_t payload[1] = { ALP_CC3501E_RESP_OK_LEGACY };

	zassert_equal(cc3501e_reply_verdict(ALP_CC3501E_CMD_PING, 3u, hdr, payload, 1u),
	              ALP_OK,
	              "a major-3 peer's bare legacy OK must decode OK");
}

/* Blocker-1 regression (#2035 follow-up): a REAL major-3 bridge reply is
 * NEVER declared payload_len == 1 on the wire -- the firmware zero-pads
 * every reply up to an ALP_CC3501E_REPLY_PAD (8 B) multiple with the pad
 * folded INTO the declared length (see cc3501e_events.c's poll comment and
 * <alp/protocol/cc3501e.h>'s ALP_CC3501E_REPLY_PAD doc comment), so a bare
 * RESP_OK_LEGACY reply for a real bare-status opcode like PING arrives here
 * as payload_len == 8, all eight bytes 0x00 (status 0x00 + 7 pad bytes).
 * test_cc3501e_reply_verdict_major3_round_trip above fabricates payload_len
 * == 1, a shape a real bridge never produces -- it let the pre-fix
 * `payload_len == 1u` dispatch look correct while being dead code against
 * the wire; every real bare-status reply fell through to the "has data"
 * branch instead and was wrongly rejected as a dead phase.  This is the
 * real shape.  Before the blocker-1 fix this asserted ALP_ERR_IO, not
 * ALP_OK -- see the task's mutation-evidence run. */
ZTEST(alp_chips, test_cc3501e_reply_verdict_major3_bare_status_is_padded)
{
	uint8_t hdr[ALP_CC3501E_HEADER_BYTES];
	fill_reply_hdr(hdr, ALP_CC3501E_CMD_PING);
	uint8_t payload[8] = { ALP_CC3501E_RESP_OK_LEGACY, 0u, 0u, 0u, 0u, 0u, 0u, 0u };

	zassert_equal(cc3501e_reply_verdict(ALP_CC3501E_CMD_PING, 3u, hdr, payload, 8u),
	              ALP_OK,
	              "a real, pad-shaped major-3 bare-status reply must decode OK, not "
	              "be mistaken for a dead phase");
}

/* A major-4 firmware round trip: RESP_OK plus a valid CRC trailer, on an
 * already-gated (fw_proto_major == 4) context. */
ZTEST(alp_chips, test_cc3501e_reply_verdict_major4_round_trip_valid_crc)
{
	uint8_t hdr[ALP_CC3501E_HEADER_BYTES];
	fill_reply_hdr(hdr, ALP_CC3501E_CMD_PING);
	uint8_t  payload[3] = { ALP_CC3501E_RESP_OK, 0u, 0u };
	uint16_t len        = append_valid_crc(hdr, payload, 1u);

	zassert_equal(cc3501e_reply_verdict(ALP_CC3501E_CMD_PING, 4u, hdr, payload, len),
	              ALP_OK,
	              "a major-4 peer's OK + valid CRC must decode OK");
}

/* A corrupted CRC trailer must be rejected, even though the status byte
 * itself claims success. */
ZTEST(alp_chips, test_cc3501e_reply_verdict_major4_corrupted_crc_rejected)
{
	uint8_t hdr[ALP_CC3501E_HEADER_BYTES];
	fill_reply_hdr(hdr, ALP_CC3501E_CMD_PING);
	uint8_t  payload[3] = { ALP_CC3501E_RESP_OK, 0u, 0u };
	uint16_t len        = append_valid_crc(hdr, payload, 1u);
	payload[len - 1u] ^= 0xFFu; /* flip the CRC's high byte */

	zassert_equal(cc3501e_reply_verdict(ALP_CC3501E_CMD_PING, 4u, hdr, payload, len),
	              ALP_ERR_IO,
	              "a corrupted CRC must be rejected even with resp == RESP_OK");
}

/* An all-zero dead phase on a PROTECTED multi-byte opcode (GET_MAC is not on
 * cc3501e_reply_may_be_all_zero()'s exemption list) must be rejected on the
 * LEGACY (major-3) branch via the pre-existing all-zero heuristic -- no CRC
 * exists on that wire to catch it any other way. */
ZTEST(alp_chips, test_cc3501e_reply_verdict_dead_phase_rejected_major3)
{
	uint8_t hdr[ALP_CC3501E_HEADER_BYTES];
	fill_reply_hdr(hdr, ALP_CC3501E_CMD_GET_MAC);
	uint8_t payload[7] = { 0 }; /* status=0x00 + 6 all-zero MAC bytes */

	zassert_equal(cc3501e_reply_verdict(ALP_CC3501E_CMD_GET_MAC, 3u, hdr, payload, 7u),
	              ALP_ERR_IO,
	              "an all-zero legacy GET_MAC reply must be rejected as a dead phase");
}

/* The same all-zero dead phase on the 4.0 (major-4) branch: the CRC check
 * catches it independently of the all-zero heuristic (the CRC of a real,
 * non-zero header plus an all-zero payload span is never the wire's
 * all-zero "CRC"), so this must reject too -- via a different mechanism than
 * the major-3 case above, which is exactly what "rejected under both
 * branches" means. */
ZTEST(alp_chips, test_cc3501e_reply_verdict_dead_phase_rejected_major4)
{
	uint8_t hdr[ALP_CC3501E_HEADER_BYTES];
	fill_reply_hdr(hdr, ALP_CC3501E_CMD_GET_MAC);
	uint8_t payload[9] = { 0 }; /* status + 6 MAC bytes + 2 (also-zero) CRC bytes */

	zassert_equal(cc3501e_reply_verdict(ALP_CC3501E_CMD_GET_MAC, 4u, hdr, payload, 9u),
	              ALP_ERR_IO,
	              "an all-zero 4.0 GET_MAC reply must be rejected -- the CRC of the real "
	              "(non-zero) header can never equal the wire's zero trailer");
}

/* A NON-all-zero major-4 reply whose status byte is the LEGACY OK (0x00),
 * not ALP_CC3501E_RESP_OK (0x5A), must still be rejected even with a valid
 * CRC over exactly those bytes -- a real fw_proto_major-4 peer's status byte
 * is always 0x5A on success, so 0x00 reaching here is never legitimate.
 * Guards against a narrower reintroduction of #1378: making resp_to_status()
 * accept legacy 0x00 unconditionally (instead of gating it on fw_major) would
 * NOT be caught by the all-zero dead-phase tests above, since this payload is
 * deliberately not all-zero. */
ZTEST(alp_chips, test_cc3501e_reply_verdict_major4_legacy_status_byte_rejected)
{
	uint8_t hdr[ALP_CC3501E_HEADER_BYTES];
	fill_reply_hdr(hdr, ALP_CC3501E_CMD_PING);
	uint8_t  payload[4] = { ALP_CC3501E_RESP_OK_LEGACY, 0xABu, 0u, 0u }; /* not all-zero */
	uint16_t len        = append_valid_crc(hdr, payload, 2u);

	zassert_equal(cc3501e_reply_verdict(ALP_CC3501E_CMD_PING, 4u, hdr, payload, len),
	              ALP_ERR_IO,
	              "a major-4 peer's status byte must never be read as the legacy 0x00 OK");
}

/*
 * #2035: a bench session on a board running 3.1 firmware saw this demo print
 * "MAJOR MISMATCH" -> phase FAIL while every functional sub-check (ping,
 * MAC, caps, scan, BLE) passed. The host driver is deliberately bilingual
 * (cc3501e_fw_major_is_acceptable() above), so a legacy-major reply is the
 * v3->v4 migration working, not a mismatch. These four pin the fix's four
 * outcomes; each one exercises cc3501e_classify_link() and
 * cc3501e_link_verdict_ok() exactly as main.c's phase_cc3501e() does.
 *
 * Mutation coverage: widening the old two-way ver_ok boolean back to
 * `fw_major == MAJOR` alone (dropping the LEGACY branch) reddens
 * test_cc3501e_classify_link_legacy_major_is_ok; dropping the
 * ver_rc != ALP_OK guard reddens test_cc3501e_classify_link_bad_rc_is_mismatch;
 * collapsing MATCH and MINOR_AHEAD into one outcome reddens
 * test_cc3501e_classify_link_minor_ahead_is_not_match; and accepting a
 * fw_major that is neither current nor legacy reddens
 * test_cc3501e_classify_link_wrong_major_is_mismatch.
 */
ZTEST(alp_chips, test_cc3501e_classify_link_exact_match)
{
	cc3501e_link_verdict_t v = cc3501e_classify_link(
	    ALP_OK, (unsigned)ALP_CC3501E_PROTOCOL_MAJOR, (unsigned)ALP_CC3501E_PROTOCOL_MINOR);
	zassert_equal(v, CC3501E_LINK_VERDICT_MATCH, "major+minor both equal must be MATCH");
	zassert_true(cc3501e_link_verdict_ok(v), "an exact match must be an OK link");
}

ZTEST(alp_chips, test_cc3501e_classify_link_minor_ahead_is_not_match)
{
	cc3501e_link_verdict_t v = cc3501e_classify_link(
	    ALP_OK, (unsigned)ALP_CC3501E_PROTOCOL_MAJOR, (unsigned)ALP_CC3501E_PROTOCOL_MINOR + 1u);
	zassert_equal(
	    v, CC3501E_LINK_VERDICT_MINOR_AHEAD, "major match + minor delta must be MINOR_AHEAD");
	zassert_true(cc3501e_link_verdict_ok(v), "a minor delta is additive (ADR 0033) -- must be ok");
}

ZTEST(alp_chips, test_cc3501e_classify_link_legacy_major_is_ok)
{
	/* This is the exact scenario from the bench report: v3.1 firmware,
	 * a v4.0 host. It must be LEGACY, and it must be an OK link -- not
	 * FAIL, which is the bug this fix closes. */
	cc3501e_link_verdict_t v =
	    cc3501e_classify_link(ALP_OK, (unsigned)ALP_CC3501E_PROTOCOL_MAJOR_LEGACY, 1u);
	zassert_equal(
	    v, CC3501E_LINK_VERDICT_LEGACY, "the migration-window predecessor must be LEGACY");
	zassert_true(cc3501e_link_verdict_ok(v),
	             "a bilingual host must treat its accepted legacy MAJOR as an OK link, not a FAIL");
}

ZTEST(alp_chips, test_cc3501e_classify_link_wrong_major_is_mismatch)
{
	/* Neither the current nor the legacy MAJOR -- a genuine
	 * incompatibility, must still fail. */
	cc3501e_link_verdict_t v =
	    cc3501e_classify_link(ALP_OK, (unsigned)ALP_CC3501E_PROTOCOL_MAJOR_LEGACY - 1u, 0u);
	zassert_equal(v, CC3501E_LINK_VERDICT_MISMATCH, "an unaccepted major must be MISMATCH");
	zassert_false(cc3501e_link_verdict_ok(v), "a genuine major mismatch must not be an OK link");
}

ZTEST(alp_chips, test_cc3501e_classify_link_bad_rc_is_mismatch)
{
	/* A transport/parse failure must never be read as a link outcome --
	 * ALP_ERR_TIMEOUT here, even against an otherwise-accepted major,
	 * must still be MISMATCH. */
	cc3501e_link_verdict_t v = cc3501e_classify_link(ALP_ERR_TIMEOUT,
	                                                 (unsigned)ALP_CC3501E_PROTOCOL_MAJOR,
	                                                 (unsigned)ALP_CC3501E_PROTOCOL_MINOR);
	zassert_equal(v, CC3501E_LINK_VERDICT_MISMATCH, "a failed GET_VERSION call must be MISMATCH");
	zassert_false(cc3501e_link_verdict_ok(v), "a failed GET_VERSION call must not be an OK link");
}
