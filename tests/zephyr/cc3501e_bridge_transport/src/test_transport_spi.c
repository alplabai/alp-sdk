/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the cc3501e-bridge SPI-slave transport seams
 * (firmware/cc3501e/src/transport_spi.c) + the shared framing/dispatch
 * (firmware/cc3501e/src/protocol.c), exercised against the silicon-free
 * stub HAL backend.  These are the PRODUCTION code paths, not a mock --
 * the same protocol_build_reply() the SDIO transport uses.
 *
 * Wire framing (see <alp/protocol/cc3501e.h>): 4-byte LE header
 * [cmd | flags | payload_len(LE16)] + payload; the reply payload's
 * first byte is the response status (ALP_CC3501E_RESP_*).  No SOF, no
 * CRC -- the link is a short hardwired point-to-point bus.
 */

#include <string.h>
#include <zephyr/ztest.h>

#include "alp/protocol/cc3501e.h"
#include "transport.h"

/* Replays one request CS transaction through the seams, the way the TI
 * HAL backend's ISR path does: reset staging, feed the bytes, decode. */
static void transaction(const uint8_t *bytes, size_t len)
{
	spi_slave_cs_low();
	for (size_t i = 0; i < len; i++) {
		spi_slave_rx_byte(bytes[i]);
	}
	spi_slave_cs_high();
}

/* Drains the staged reply the way the HAL clocks the host's read FIFO. */
static size_t drain(uint8_t *out, size_t cap)
{
	size_t n = 0;
	while (spi_slave_tx_pending() && n < cap) {
		out[n++] = spi_slave_tx_next_byte();
	}
	return n;
}

/* Assert the 4-byte reply header echoes @cmd, is solicited (flags 0),
 * and declares @payload_len. */
static void assert_reply_header(const uint8_t *r, uint8_t cmd, uint16_t payload_len)
{
	zassert_equal(r[0], cmd, "reply echoes the request cmd");
	zassert_equal(r[1], 0x00u, "solicited reply: flags == 0");
	zassert_equal((uint16_t)(r[2] | ((uint16_t)r[3] << 8)), payload_len, "reply payload_len");
}

ZTEST(cc3501e_bridge_transport, test_ping_ok)
{
	const uint8_t ping[] = { ALP_CC3501E_CMD_PING, 0x00u, 0x00u, 0x00u };
	uint8_t       reply[32];

	transport_spi_init();
	transaction(ping, sizeof ping);
	size_t n = drain(reply, sizeof reply);

	/* Reply = header(4) + status(1), no data. */
	zassert_equal(n, 5u, "PING reply is header + status");
	assert_reply_header(reply, ALP_CC3501E_CMD_PING, 1u);
	zassert_equal(reply[4], ALP_CC3501E_RESP_OK, "PING -> RESP_OK");
}

ZTEST(cc3501e_bridge_transport, test_get_version_returns_protocol_version)
{
	const uint8_t gv[] = { ALP_CC3501E_CMD_GET_VERSION, 0x00u, 0x00u, 0x00u };
	uint8_t       reply[32];

	transport_spi_init();
	transaction(gv, sizeof gv);
	size_t n = drain(reply, sizeof reply);

	/* Reply = header(4) + status(1) + version(2, LE). */
	zassert_equal(n, 7u, "GET_VERSION reply is header + status + u16");
	assert_reply_header(reply, ALP_CC3501E_CMD_GET_VERSION, 3u);
	zassert_equal(reply[4], ALP_CC3501E_RESP_OK, "GET_VERSION -> RESP_OK");
	const uint16_t version = (uint16_t)reply[5] | ((uint16_t)reply[6] << 8);
	zassert_equal(version, (uint16_t)ALP_CC3501E_PROTOCOL_VERSION,
	              "GET_VERSION returns the wire-protocol version (the host's compat gate)");
}

ZTEST(cc3501e_bridge_transport, test_get_mac_not_ready_on_stub)
{
	const uint8_t gm[] = { ALP_CC3501E_CMD_GET_MAC, 0x00u, 0x00u, 0x00u };
	uint8_t       reply[32];

	transport_spi_init();
	transaction(gm, sizeof gm);
	size_t n = drain(reply, sizeof reply);

	/* The stub HAL has no radio -> the handler maps NOTIMPL to NOT_READY
     * rather than returning a fabricated MAC. */
	zassert_equal(n, 5u, "GET_MAC error reply is header + status");
	assert_reply_header(reply, ALP_CC3501E_CMD_GET_MAC, 1u);
	zassert_equal(reply[4], ALP_CC3501E_RESP_ERR_NOT_READY, "GET_MAC on stub -> NOT_READY");
}

ZTEST(cc3501e_bridge_transport, test_unknown_opcode_rejected)
{
	/* A v1 opcode whose firmware body has not landed in v0.1 (Wi-Fi). */
	const uint8_t scan[] = { ALP_CC3501E_CMD_WIFI_SCAN_START, 0x00u, 0x00u, 0x00u };
	uint8_t       reply[32];

	transport_spi_init();
	transaction(scan, sizeof scan);
	size_t n = drain(reply, sizeof reply);

	zassert_equal(n, 5u, "unknown-opcode reply is header + status");
	assert_reply_header(reply, ALP_CC3501E_CMD_WIFI_SCAN_START, 1u);
	zassert_equal(reply[4], ALP_CC3501E_RESP_ERR_INVALID,
	              "unimplemented opcode -> RESP_ERR_INVALID (header contract)");
}

ZTEST(cc3501e_bridge_transport, test_ping_with_payload_is_invalid)
{
	/* PING takes no payload; a non-empty one is a caller bug.  Also
     * exercises the payload-framing path (declared len == captured). */
	const uint8_t ping_pl[] = { ALP_CC3501E_CMD_PING, 0x00u, 0x01u, 0x00u, 0xABu };
	uint8_t       reply[32];

	transport_spi_init();
	transaction(ping_pl, sizeof ping_pl);
	size_t n = drain(reply, sizeof reply);

	zassert_equal(n, 5u, "reply is header + status");
	zassert_equal(reply[4], ALP_CC3501E_RESP_ERR_INVALID, "PING with payload -> INVALID");
}

ZTEST(cc3501e_bridge_transport, test_bad_payload_len_is_protocol_error)
{
	/* Header declares a 5-byte payload but the transaction carried none
     * -> framing mismatch. */
	const uint8_t bad[] = { ALP_CC3501E_CMD_PING, 0x00u, 0x05u, 0x00u };
	uint8_t       reply[32];

	transport_spi_init();
	transaction(bad, sizeof bad);
	size_t n = drain(reply, sizeof reply);

	zassert_equal(n, 5u, "reply is header + status");
	zassert_equal(reply[4], ALP_CC3501E_RESP_ERR_PROTOCOL, "length mismatch -> PROTOCOL error");
}

/* An empty transaction (CS toggled, no bytes) must rewind the drain
 * cursor so the host's reply-read re-serves the staged reply. */
ZTEST(cc3501e_bridge_transport, test_empty_transaction_rewinds_reply)
{
	const uint8_t ping[] = { ALP_CC3501E_CMD_PING, 0x00u, 0x00u, 0x00u };
	uint8_t       first[32], again[32];

	transport_spi_init();
	transaction(ping, sizeof ping);
	size_t n_first = drain(first, sizeof first);
	zassert_false(spi_slave_tx_pending(), "reply drained");

	transaction(NULL, 0u); /* CS pulse, nothing captured */

	zassert_true(spi_slave_tx_pending(), "empty transaction rewinds the staged reply");
	size_t n_again = drain(again, sizeof again);
	zassert_equal(n_again, n_first, "full reply re-armed");
	zassert_mem_equal(again, first, n_first, "identical bytes re-armed");
}

/* A fresh request replaces the staged reply; the rewind must never
 * resurrect a previous command's reply once a new one decodes. */
ZTEST(cc3501e_bridge_transport, test_new_request_replaces_staged_reply)
{
	const uint8_t ping[] = { ALP_CC3501E_CMD_PING, 0x00u, 0x00u, 0x00u };
	const uint8_t gv[]   = { ALP_CC3501E_CMD_GET_VERSION, 0x00u, 0x00u, 0x00u };
	uint8_t       buf[32];

	transport_spi_init();
	transaction(ping, sizeof ping);
	(void)drain(buf, sizeof buf);

	transaction(gv, sizeof gv);
	size_t n = drain(buf, sizeof buf);
	zassert_equal(n, 7u, "GET_VERSION reply staged");
	zassert_equal(buf[0], ALP_CC3501E_CMD_GET_VERSION, "current reply, not the old PING");

	transaction(NULL, 0u);
	size_t n2 = drain(buf, sizeof buf);
	zassert_equal(n2, 7u, "rewind re-arms the CURRENT reply");
	zassert_equal(buf[0], ALP_CC3501E_CMD_GET_VERSION, "still GET_VERSION after rewind");
}

ZTEST_SUITE(cc3501e_bridge_transport, NULL, NULL, NULL, NULL, NULL);
