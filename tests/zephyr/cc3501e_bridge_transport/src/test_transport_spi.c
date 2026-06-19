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
#include "worker.h" /* worker_init -- the worker `job` is a static; reset it per test */

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
	zassert_equal(version,
	              (uint16_t)ALP_CC3501E_PROTOCOL_VERSION,
	              "GET_VERSION returns the wire-protocol version (the host's compat gate)");
}

ZTEST(cc3501e_bridge_transport, test_get_mac_not_ready_on_stub)
{
	const uint8_t gm[] = { ALP_CC3501E_CMD_GET_MAC, 0x00u, 0x00u, 0x00u };
	uint8_t       reply[32];

	transport_spi_init();

	/* GET_MAC is now POLL-BY-REPEAT through the async worker (P0-4/P0-6):
	 * the first request submits the job and replies BUSY (the host maps
	 * RESP_ERR_BUSY -> ALP_ERR_BUSY and re-issues).  On the silicon-free
	 * stub the worker runs the job SYNCHRONOUSLY at submit, so a single
	 * re-issue then returns the cached result.  The stub HAL has no radio
	 * -> the job's HAL body reports NOTIMPL, which the handler maps to
	 * NOT_READY rather than a fabricated MAC. */
	transaction(gm, sizeof gm);
	size_t n = drain(reply, sizeof reply);
	zassert_equal(n, 5u, "first GET_MAC reply is header + status");
	assert_reply_header(reply, ALP_CC3501E_CMD_GET_MAC, 1u);
	zassert_equal(reply[4],
	              ALP_CC3501E_RESP_ERR_BUSY,
	              "first GET_MAC submits the job -> BUSY (host retries)");

	/* Host re-issues GET_MAC: the worker has the (synchronous-on-stub)
	 * result cached -> NOT_READY. */
	transaction(gm, sizeof gm);
	n = drain(reply, sizeof reply);
	zassert_equal(n, 5u, "GET_MAC error reply is header + status");
	assert_reply_header(reply, ALP_CC3501E_CMD_GET_MAC, 1u);
	zassert_equal(
	    reply[4], ALP_CC3501E_RESP_ERR_NOT_READY, "re-issued GET_MAC on stub -> NOT_READY");
}

ZTEST(cc3501e_bridge_transport, test_unknown_opcode_rejected)
{
	/* An opcode in the reserved vendor-extension range (>= 0x80) is never a
	 * v1 command -> RESP_ERR_INVALID (the Wi-Fi/GPIO groups are now
	 * implemented, so a real opcode no longer serves as the "unknown" probe). */
	const uint8_t op[] = { ALP_CC3501E_CMD_RESERVED_VENDOR_BASE, 0x00u, 0x00u, 0x00u };
	uint8_t       reply[32];

	transport_spi_init();
	transaction(op, sizeof op);
	size_t n = drain(reply, sizeof reply);

	zassert_equal(n, 5u, "unknown-opcode reply is header + status");
	assert_reply_header(reply, ALP_CC3501E_CMD_RESERVED_VENDOR_BASE, 1u);
	zassert_equal(reply[4],
	              ALP_CC3501E_RESP_ERR_INVALID,
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

/* GPIO proxy (v0.4): configure -> write -> read round-trip through the
 * production dispatch + the stub HAL's in-memory pin model. */
ZTEST(cc3501e_bridge_transport, test_gpio_write_then_read)
{
	uint8_t reply[32];
	transport_spi_init();

	const uint8_t cfg[] = {
		ALP_CC3501E_CMD_GPIO_CONFIGURE, 0x00u, 0x04u, 0x00u, 14u, ALP_CC3501E_GPIO_DIR_OUTPUT,
		ALP_CC3501E_GPIO_PULL_NONE,     0x00u
	};
	transaction(cfg, sizeof cfg);
	size_t n = drain(reply, sizeof reply);
	zassert_equal(n, 5u, "configure reply = header + status");
	zassert_equal(reply[4], ALP_CC3501E_RESP_OK, "GPIO_CONFIGURE -> OK");

	const uint8_t wr[] = { ALP_CC3501E_CMD_GPIO_WRITE, 0x00u, 0x04u, 0x00u, 14u, 1u, 0x00u, 0x00u };
	transaction(wr, sizeof wr);
	(void)drain(reply, sizeof reply);
	zassert_equal(reply[4], ALP_CC3501E_RESP_OK, "GPIO_WRITE -> OK");

	const uint8_t rd[] = { ALP_CC3501E_CMD_GPIO_READ, 0x00u, 0x01u, 0x00u, 14u };
	transaction(rd, sizeof rd);
	n = drain(reply, sizeof reply);
	zassert_equal(n, 6u, "read reply = header + status + level");
	assert_reply_header(reply, ALP_CC3501E_CMD_GPIO_READ, 2u);
	zassert_equal(reply[4], ALP_CC3501E_RESP_OK, "GPIO_READ -> OK");
	zassert_equal(reply[5], 1u, "GPIO_READ reflects the written level");
}

ZTEST(cc3501e_bridge_transport, test_cam_enable_ok)
{
	uint8_t reply[32];
	transport_spi_init();
	const uint8_t cam[] = { ALP_CC3501E_CMD_CAM_ENABLE, 0x00u, 0x01u, 0x00u, 0u };
	transaction(cam, sizeof cam);
	size_t n = drain(reply, sizeof reply);
	zassert_equal(n, 5u, "cam reply = header + status");
	zassert_equal(reply[4], ALP_CC3501E_RESP_OK, "CAM_ENABLE -> OK on stub");
}

ZTEST(cc3501e_bridge_transport, test_gpio_write_bad_len_invalid)
{
	uint8_t reply[32];
	transport_spi_init();
	/* GPIO_WRITE declares a 1-byte payload but the struct is 4 -> INVALID. */
	const uint8_t wr[] = { ALP_CC3501E_CMD_GPIO_WRITE, 0x00u, 0x01u, 0x00u, 14u };
	transaction(wr, sizeof wr);
	(void)drain(reply, sizeof reply);
	zassert_equal(reply[4], ALP_CC3501E_RESP_ERR_INVALID, "wrong-length GPIO_WRITE -> INVALID");
}

/* Wi-Fi (v0.2): the stub HAL has no radio, so a well-formed request parses
 * cleanly and maps to NOT_READY, while a malformed one is rejected at the
 * protocol layer (INVALID) before reaching the HAL. */

/* WIFI_SCAN_START is now WORKER-ROUTED (mirrors GET_MAC): the seconds-long
 * Wlan_Scan MUST NOT run in the SPI ISR, so the handler is POLL-BY-REPEAT --
 * the first request submits the job and replies BUSY, the host re-issues and
 * collects the cached result.  On the silicon-free stub the worker runs the
 * job synchronously at submit, and the stub HAL's scan reports NOTIMPL ->
 * the re-issue maps to NOT_READY. */
ZTEST(cc3501e_bridge_transport, test_wifi_scan_start_not_ready)
{
	uint8_t reply[32];
	transport_spi_init();
	const uint8_t s[] = { ALP_CC3501E_CMD_WIFI_SCAN_START, 0x00u, 0x00u, 0x00u };

	transaction(s, sizeof s);
	size_t n = drain(reply, sizeof reply);
	zassert_equal(n, 5u, "first scan reply = header + status");
	assert_reply_header(reply, ALP_CC3501E_CMD_WIFI_SCAN_START, 1u);
	zassert_equal(reply[4],
	              ALP_CC3501E_RESP_ERR_BUSY,
	              "first SCAN_START submits the job -> BUSY (host retries)");

	transaction(s, sizeof s);
	n = drain(reply, sizeof reply);
	zassert_equal(n, 5u, "re-issued scan reply = header + status");
	assert_reply_header(reply, ALP_CC3501E_CMD_WIFI_SCAN_START, 1u);
	zassert_equal(
	    reply[4], ALP_CC3501E_RESP_ERR_NOT_READY, "re-issued SCAN_START on stub -> NOT_READY");
}

/* WIFI_GET_RSSI is worker-routed the same way (poll-by-repeat): submit -> BUSY,
 * re-issue -> the cached NOT_READY on the radio-less stub. */
ZTEST(cc3501e_bridge_transport, test_wifi_get_rssi_not_ready)
{
	uint8_t reply[32];
	transport_spi_init();
	const uint8_t r[] = { ALP_CC3501E_CMD_WIFI_GET_RSSI, 0x00u, 0x00u, 0x00u };

	transaction(r, sizeof r);
	size_t n = drain(reply, sizeof reply);
	zassert_equal(n, 5u, "first rssi reply = header + status");
	assert_reply_header(reply, ALP_CC3501E_CMD_WIFI_GET_RSSI, 1u);
	zassert_equal(reply[4],
	              ALP_CC3501E_RESP_ERR_BUSY,
	              "first GET_RSSI submits the job -> BUSY (host retries)");

	transaction(r, sizeof r);
	n = drain(reply, sizeof reply);
	zassert_equal(n, 5u, "re-issued rssi reply = header + status");
	assert_reply_header(reply, ALP_CC3501E_CMD_WIFI_GET_RSSI, 1u);
	zassert_equal(
	    reply[4], ALP_CC3501E_RESP_ERR_NOT_READY, "re-issued GET_RSSI on stub -> NOT_READY");
}

ZTEST(cc3501e_bridge_transport, test_wifi_connect_sta_parses_then_not_ready)
{
	uint8_t reply[32];
	transport_spi_init();
	/* connect_t {ssid_len=4, psk_len=8, security=WPA2(1), rsvd} + "wifi" + "password";
	 * payload_len = 4 (header) + 4 (ssid) + 8 (psk) = 16. */
	const uint8_t req[] = { ALP_CC3501E_CMD_WIFI_CONNECT_STA,
		                    0x00u,
		                    16u,
		                    0x00u,
		                    4u,
		                    8u,
		                    1u,
		                    0u,
		                    'w',
		                    'i',
		                    'f',
		                    'i',
		                    'p',
		                    'a',
		                    's',
		                    's',
		                    'w',
		                    'o',
		                    'r',
		                    'd' };
	transaction(req, sizeof req);
	size_t n = drain(reply, sizeof reply);
	zassert_equal(n, 5u, "connect reply = header + status");
	assert_reply_header(reply, ALP_CC3501E_CMD_WIFI_CONNECT_STA, 1u);
	zassert_equal(reply[4],
	              ALP_CC3501E_RESP_ERR_NOT_READY,
	              "well-formed connect parses, then NOT_READY (no radio)");
}

ZTEST(cc3501e_bridge_transport, test_wifi_connect_bad_len_invalid)
{
	uint8_t reply[32];
	transport_spi_init();
	/* connect_t says ssid_len=4 psk_len=8 (needs 16 payload) but only 8 sent. */
	const uint8_t req[] = {
		ALP_CC3501E_CMD_WIFI_CONNECT_STA, 0x00u, 8u, 0x00u, 4u, 8u, 1u, 0u, 'w', 'i', 'f', 'i'
	};
	transaction(req, sizeof req);
	(void)drain(reply, sizeof reply);
	zassert_equal(reply[4], ALP_CC3501E_RESP_ERR_INVALID, "connect length mismatch -> INVALID");
}

/* BLE (v0.3): no BLE host on the stub -> well-formed requests parse and map
 * to NOT_READY; malformed ones are rejected (INVALID) at the protocol layer. */

/* BLE_ENABLE is WORKER-ROUTED (the real body starts Wi-Fi + NimBLE, blocks for
 * seconds -- must run off the SPI ISR), so it is poll-by-repeat exactly like
 * GET_MAC: the first request submits the job and replies BUSY; the re-issue
 * collects the cached result, which on the radio-less stub is NOT_READY. */
ZTEST(cc3501e_bridge_transport, test_ble_enable_not_ready)
{
	uint8_t reply[32];
	transport_spi_init();
	const uint8_t e[] = { ALP_CC3501E_CMD_BLE_ENABLE, 0x00u, 0x00u, 0x00u };

	transaction(e, sizeof e);
	size_t n = drain(reply, sizeof reply);
	zassert_equal(n, 5u, "first BLE_ENABLE reply = header + status");
	assert_reply_header(reply, ALP_CC3501E_CMD_BLE_ENABLE, 1u);
	zassert_equal(reply[4],
	              ALP_CC3501E_RESP_ERR_BUSY,
	              "first BLE_ENABLE submits the job -> BUSY (host retries)");

	transaction(e, sizeof e);
	n = drain(reply, sizeof reply);
	zassert_equal(n, 5u, "re-issued BLE_ENABLE reply = header + status");
	assert_reply_header(reply, ALP_CC3501E_CMD_BLE_ENABLE, 1u);
	zassert_equal(reply[4], ALP_CC3501E_RESP_ERR_NOT_READY, "no BLE host on stub -> NOT_READY");
}

ZTEST(cc3501e_bridge_transport, test_ble_adv_start_parses_then_not_ready)
{
	uint8_t reply[32];
	transport_spi_init();
	/* connectable=1, rsvd=0, imin=100, imax=200, adv_data_len=3, adv={02 01 06};
	 * packed header is 7 bytes -> payload_len = 7 + 3 = 10. */
	const uint8_t req[] = { ALP_CC3501E_CMD_BLE_ADV_START,
		                    0x00u,
		                    10u,
		                    0x00u,
		                    1u,
		                    0u,
		                    100u,
		                    0u,
		                    200u,
		                    0u,
		                    3u,
		                    0x02u,
		                    0x01u,
		                    0x06u };
	transaction(req, sizeof req);
	size_t n = drain(reply, sizeof reply);
	zassert_equal(n, 5u, "adv reply = header + status");
	assert_reply_header(reply, ALP_CC3501E_CMD_BLE_ADV_START, 1u);
	zassert_equal(reply[4], ALP_CC3501E_RESP_ERR_NOT_READY, "well-formed adv parses -> NOT_READY");
}

ZTEST(cc3501e_bridge_transport, test_ble_adv_start_bad_len_invalid)
{
	uint8_t reply[32];
	transport_spi_init();
	/* adv_data_len=3 needs payload 10, but only 7 sent. */
	const uint8_t req[] = {
		ALP_CC3501E_CMD_BLE_ADV_START, 0x00u, 7u, 0x00u, 1u, 0u, 100u, 0u, 200u, 0u, 3u
	};
	transaction(req, sizeof req);
	(void)drain(reply, sizeof reply);
	zassert_equal(reply[4], ALP_CC3501E_RESP_ERR_INVALID, "adv length mismatch -> INVALID");
}

ZTEST(cc3501e_bridge_transport, test_ble_connect_bad_len_invalid)
{
	uint8_t reply[32];
	transport_spi_init();
	/* BLE_CONNECT needs 7 bytes (addr_type + addr[6]); send 4. */
	const uint8_t req[] = { ALP_CC3501E_CMD_BLE_CONNECT, 0x00u, 4u, 0x00u, 0u, 1u, 2u, 3u };
	transaction(req, sizeof req);
	(void)drain(reply, sizeof reply);
	zassert_equal(reply[4], ALP_CC3501E_RESP_ERR_INVALID, "short BLE_CONNECT -> INVALID");
}

/* Configurability + diagnostics (firmware-side, no radio): these are fully
 * functional on the stub -- config is accepted (OK) and diag/stats return
 * real firmware-tracked data. */
ZTEST(cc3501e_bridge_transport, test_get_diag_info)
{
	uint8_t reply[40];
	transport_spi_init();
	const uint8_t d[] = { ALP_CC3501E_CMD_GET_DIAG_INFO, 0x00u, 0x00u, 0x00u };
	transaction(d, sizeof d);
	size_t n = drain(reply, sizeof reply);
	zassert_equal(n, 4u + 1u + 16u, "diag reply = header + status + 16B struct");
	assert_reply_header(reply, ALP_CC3501E_CMD_GET_DIAG_INFO, 17u);
	zassert_equal(reply[4], ALP_CC3501E_RESP_OK, "GET_DIAG_INFO -> OK");
	const uint16_t fw = (uint16_t)reply[5] | ((uint16_t)reply[6] << 8);
	zassert_equal(fw, 0x0001u, "fw_version = 0x0001 (v0.1.0)");
	zassert_equal(reply[7], (uint8_t)ALP_CC3501E_RESET_POWER_ON, "stub reset cause = POWER_ON");
	zassert_equal(reply[8], (uint8_t)ALP_CC3501E_ROLE_OFF, "role = OFF in v0.1 (no radio)");
}

ZTEST(cc3501e_bridge_transport, test_power_policy_ok)
{
	uint8_t reply[16];
	transport_spi_init();
	/* policy=BALANCED(1) | wake=HOST_SPI(0x01) | rsvd(2) | idle_ms=1000 (LE32) */
	const uint8_t pp[] = {
		ALP_CC3501E_CMD_POWER_POLICY, 0x00u, 8u, 0x00u, 1u, 0x01u, 0u, 0u, 0xE8u, 0x03u, 0u, 0u
	};
	transaction(pp, sizeof pp);
	(void)drain(reply, sizeof reply);
	zassert_equal(reply[4], ALP_CC3501E_RESP_OK, "POWER_POLICY accepted -> OK");
}

ZTEST(cc3501e_bridge_transport, test_power_policy_bad_len_invalid)
{
	uint8_t reply[16];
	transport_spi_init();
	const uint8_t pp[] = { ALP_CC3501E_CMD_POWER_POLICY, 0x00u, 4u, 0x00u, 1u, 0u, 0u, 0u };
	transaction(pp, sizeof pp);
	(void)drain(reply, sizeof reply);
	zassert_equal(reply[4], ALP_CC3501E_RESP_ERR_INVALID, "short POWER_POLICY -> INVALID");
}

ZTEST(cc3501e_bridge_transport, test_diag_get_stats_counts_frames)
{
	uint8_t reply[24];
	transport_spi_init();
	/* A PING first guarantees >= 1 OK frame before we read the stats. */
	const uint8_t ping[] = { ALP_CC3501E_CMD_PING, 0x00u, 0x00u, 0x00u };
	transaction(ping, sizeof ping);
	(void)drain(reply, sizeof reply);

	const uint8_t s[] = { ALP_CC3501E_CMD_DIAG_GET_STATS, 0x00u, 0x00u, 0x00u };
	transaction(s, sizeof s);
	size_t n = drain(reply, sizeof reply);
	zassert_equal(n, 4u + 1u + 8u, "stats reply = header + status + 8B");
	zassert_equal(reply[4], ALP_CC3501E_RESP_OK, "DIAG_GET_STATS -> OK");
	const uint32_t frames_ok = (uint32_t)reply[5] | ((uint32_t)reply[6] << 8) |
	                           ((uint32_t)reply[7] << 16) | ((uint32_t)reply[8] << 24);
	zassert_true(frames_ok >= 1u, "OK frames counted (>= the prior PING)");
}

ZTEST(cc3501e_bridge_transport, test_diag_log_level_ok)
{
	uint8_t reply[16];
	transport_spi_init();
	const uint8_t l[] = { ALP_CC3501E_CMD_DIAG_LOG_LEVEL, 0x00u, 1u, 0x00u, 2u };
	transaction(l, sizeof l);
	(void)drain(reply, sizeof reply);
	zassert_equal(reply[4], ALP_CC3501E_RESP_OK, "DIAG_LOG_LEVEL accepted -> OK");
}

/* ---- GPIO proxy (0x50..0x53) + camera enables (0x60/0x61) ---- *
 * The stub HAL simulates the pads in RAM, so these exercise the full
 * wire contract: payload struct layout, dispatch, and the read-back. */

ZTEST(cc3501e_bridge_transport, test_gpio_configure_write_read_roundtrip)
{
	uint8_t reply[16];
	transport_spi_init();

	/* CONFIGURE pad 13 as OUTPUT, no pull (payload = gpio_configure_t, 4 B). */
	const uint8_t cfg[] = {
		ALP_CC3501E_CMD_GPIO_CONFIGURE, 0x00u, 0x04u, 0x00u, 13u, ALP_CC3501E_GPIO_DIR_OUTPUT,
		ALP_CC3501E_GPIO_PULL_NONE,     0u
	};
	transaction(cfg, sizeof cfg);
	size_t n = drain(reply, sizeof reply);
	zassert_equal(n, 5u, "CONFIGURE reply = header + status");
	assert_reply_header(reply, ALP_CC3501E_CMD_GPIO_CONFIGURE, 1u);
	zassert_equal(reply[4], ALP_CC3501E_RESP_OK, "CONFIGURE -> OK");

	/* WRITE pad 13 high (payload = gpio_write_t, 4 B). */
	const uint8_t wr[] = { ALP_CC3501E_CMD_GPIO_WRITE, 0x00u, 0x04u, 0x00u, 13u, 1u, 0u, 0u };
	transaction(wr, sizeof wr);
	n = drain(reply, sizeof reply);
	zassert_equal(reply[4], ALP_CC3501E_RESP_OK, "WRITE -> OK");

	/* READ pad 13 (request payload = 1 B); reply = header + status + level. */
	const uint8_t rd[] = { ALP_CC3501E_CMD_GPIO_READ, 0x00u, 0x01u, 0x00u, 13u };
	transaction(rd, sizeof rd);
	n = drain(reply, sizeof reply);
	zassert_equal(n, 6u, "READ reply = header + status + level");
	assert_reply_header(reply, ALP_CC3501E_CMD_GPIO_READ, 2u);
	zassert_equal(reply[4], ALP_CC3501E_RESP_OK, "READ -> OK");
	zassert_equal(reply[5], 1u, "READ reflects the WRITE-high");

	/* WRITE low, READ back 0. */
	const uint8_t wr0[] = { ALP_CC3501E_CMD_GPIO_WRITE, 0x00u, 0x04u, 0x00u, 13u, 0u, 0u, 0u };
	transaction(wr0, sizeof wr0);
	(void)drain(reply, sizeof reply);
	transaction(rd, sizeof rd);
	(void)drain(reply, sizeof reply);
	zassert_equal(reply[5], 0u, "READ reflects the WRITE-low");
}

ZTEST(cc3501e_bridge_transport, test_gpio_configure_bad_len_invalid)
{
	uint8_t reply[16];
	transport_spi_init();
	/* CONFIGURE with a 2-byte payload (gpio_configure_t is 4) -> INVALID. */
	const uint8_t bad[] = { ALP_CC3501E_CMD_GPIO_CONFIGURE, 0x00u, 0x02u, 0x00u, 13u, 1u };
	transaction(bad, sizeof bad);
	(void)drain(reply, sizeof reply);
	zassert_equal(reply[4], ALP_CC3501E_RESP_ERR_INVALID, "short CONFIGURE -> INVALID");
}

ZTEST(cc3501e_bridge_transport, test_cam_enable_disable_ok)
{
	uint8_t reply[16];
	transport_spi_init();
	const uint8_t en[] = { ALP_CC3501E_CMD_CAM_ENABLE, 0x00u, 0x01u, 0x00u, 0u };
	transaction(en, sizeof en);
	(void)drain(reply, sizeof reply);
	zassert_equal(reply[4], ALP_CC3501E_RESP_OK, "CAM_ENABLE(0) -> OK");

	const uint8_t dis[] = { ALP_CC3501E_CMD_CAM_DISABLE, 0x00u, 0x01u, 0x00u, 0u };
	transaction(dis, sizeof dis);
	(void)drain(reply, sizeof reply);
	zassert_equal(reply[4], ALP_CC3501E_RESP_OK, "CAM_DISABLE(0) -> OK");
}

/* The worker's `job` is a file-static singleton shared across the whole TU, so a
 * worker-routed test that submits but never collects its result (the body runs
 * synchronously on the stub and caches ERR) would leave the worker non-IDLE and
 * make EVERY later worker-routed poll report "other cmd busy".  Reset it before
 * each test so the cases are independent of order. */
static void reset_worker(void *fixture)
{
	(void)fixture;
	worker_init();
}

ZTEST_SUITE(cc3501e_bridge_transport, NULL, NULL, reset_worker, NULL, NULL);
