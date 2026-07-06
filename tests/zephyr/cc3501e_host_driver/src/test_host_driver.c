/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hermetic host-side tests for the CC3501E companion wrappers that the
 * OTA suite (tests/zephyr/cc3501e_host_ota) does not cover: the Wi-Fi,
 * BLE, socket, GPIO-proxy, power, and diagnostics helpers in
 * chips/cc3501e/cc3501e.c.  They drive the REAL host driver -- its request
 * ENCODE (opcode + payload byte layout) and its reply DECODE (struct field
 * extraction) -- against a software model of the firmware SPI slave.
 *
 * The model lives entirely in this test's alp_spi_transceive() stub (the
 * one seam that carries the wire contract): it plays the firmware slave in
 * the CS-less 3-wire lockstep (request header -> request payload -> reply
 * header -> reply payload), records the exact bytes the host EMITS for
 * each opcode, and stages a deterministic reply the host then DECODES back.
 * No TI silicon, no Zephyr SPI backend, no radio -- just the driver and the
 * wire format from <alp/protocol/cc3501e.h>.
 *
 * Wire framing mirrors <alp/protocol/cc3501e.h> and the firmware transport
 * (firmware/cc3501e/hal/ti/transport_hw_ti_spi.c): a 4-byte LE header
 * [cmd | flags | payload_len(LE16)] then payload; the reply header echoes
 * the request cmd and declares the reply payload length; the reply
 * payload's first byte is the response status (ALP_CC3501E_RESP_*).
 */

#include <string.h>
#include <zephyr/ztest.h>

#include "alp/chips/cc3501e.h"
#include "alp/protocol/cc3501e.h"

/* ---- software model of the firmware slave ---------------------------------- */

enum slave_phase {
	PH_REQ_HDR = 0, /* next transfer is a 4-byte request header   */
	PH_REQ_PL,      /* next transfer is the request payload       */
	PH_REPLY_HDR,   /* host reads the 4-byte reply header         */
	PH_REPLY_PL,    /* host reads the reply payload (status+data) */
};

static struct {
	enum slave_phase phase;
	uint8_t          cmd;     /* opcode of the in-flight request (0 = none clocked) */
	uint16_t         req_len; /* declared request payload length                    */
	uint8_t          req_pl[ALP_CC3501E_MAX_PAYLOAD]; /* captured request payload    */

	/* Staged reply (built at request completion, drained over phases 3+4). */
	uint8_t  reply_pl[ALP_CC3501E_MAX_PAYLOAD]; /* status byte + data */
	uint16_t reply_len;                         /* == 1 + data bytes  */

	/* A tiny in-RAM pin model so GPIO configure -> write -> read round-trips
	 * through the real wire encode/decode, like the firmware stub HAL. */
	uint8_t pin_level[64];
} slave;

static void slave_reset(void)
{
	memset(&slave, 0, sizeof(slave));
	slave.phase = PH_REQ_HDR;
}

static void stage_status(uint8_t st)
{
	slave.reply_pl[0] = st;
	slave.reply_len   = 1u;
}

/* status(1) + @n data bytes copied from @data. */
static void stage_reply(uint8_t st, const uint8_t *data, uint16_t n)
{
	slave.reply_pl[0] = st;
	if (n > 0u) {
		memcpy(&slave.reply_pl[1], data, n);
	}
	slave.reply_len = (uint16_t)(1u + n);
}

/* ---- canned decode fixtures (the values the DECODE tests assert on) -------- */

static const uint8_t FIX_MAC[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };

/* Two Wi-Fi scan records, packed exactly as the firmware returns them:
 * bssid[6] | rssi(int8) | channel | security_info(LE16) | ssid_len | ssid[]. */
static uint16_t build_wifi_scan(uint8_t *p)
{
	uint16_t o = 0u;
	/* rec0: "Test", ch6, -40 dBm, WPA2 (sec-type bitmap 0x04 in the high byte). */
	const uint8_t b0[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
	memcpy(&p[o], b0, 6);
	o += 6u;
	p[o++] = (uint8_t)(-40);   /* rssi */
	p[o++] = 6u;               /* channel */
	p[o++] = 0x00u;            /* security_info LE lo */
	p[o++] = 0x04u;            /* security_info LE hi -> 0x0400 = WPA2 */
	p[o++] = 4u;               /* ssid_len */
	memcpy(&p[o], "Test", 4u); /* ssid */
	o += 4u;
	/* rec1: "OpenNet", ch11, -70 dBm, open (sec bits 0). */
	const uint8_t b1[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
	memcpy(&p[o], b1, 6);
	o += 6u;
	p[o++] = (uint8_t)(-70);
	p[o++] = 11u;
	p[o++] = 0x00u;
	p[o++] = 0x00u; /* open */
	p[o++] = 7u;
	memcpy(&p[o], "OpenNet", 7u);
	o += 7u;
	return o;
}

/* Two BLE scan records: addr[6] | addr_type | rssi(int8) | name_len | name[]. */
static uint16_t build_ble_scan(uint8_t *p)
{
	uint16_t o = 0u;
	/* rec0: named "MyBLE", public addr, -55 dBm. */
	const uint8_t a0[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
	memcpy(&p[o], a0, 6);
	o += 6u;
	p[o++] = 0u; /* addr_type public */
	p[o++] = (uint8_t)(-55);
	p[o++] = 5u;
	memcpy(&p[o], "MyBLE", 5u);
	o += 5u;
	/* rec1: nameless, random addr, -88 dBm. */
	const uint8_t a1[6] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 };
	memcpy(&p[o], a1, 6);
	o += 6u;
	p[o++] = 1u; /* addr_type random */
	p[o++] = (uint8_t)(-88);
	p[o++] = 0u; /* no name */
	return o;
}

/* Build the reply for the just-received request. */
static void slave_dispatch(void)
{
	switch (slave.cmd) {
	case ALP_CC3501E_CMD_PING:
	case ALP_CC3501E_CMD_RESET:
	case ALP_CC3501E_CMD_WIFI_CONNECT_STA:
	case ALP_CC3501E_CMD_WIFI_DISCONNECT:
	case ALP_CC3501E_CMD_WIFI_AP_START:
	case ALP_CC3501E_CMD_WIFI_AP_STOP:
	case ALP_CC3501E_CMD_WIFI_SCAN_STOP:
	case ALP_CC3501E_CMD_BLE_ENABLE:
	case ALP_CC3501E_CMD_BLE_DISABLE:
	case ALP_CC3501E_CMD_BLE_ADV_START:
	case ALP_CC3501E_CMD_BLE_ADV_STOP:
	case ALP_CC3501E_CMD_BLE_SCAN_STOP:
	case ALP_CC3501E_CMD_BLE_CONNECT:
	case ALP_CC3501E_CMD_BLE_DISCONNECT:
	case ALP_CC3501E_CMD_BLE_GATT_REGISTER:
	case ALP_CC3501E_CMD_BLE_GATT_NOTIFY:
	case ALP_CC3501E_CMD_BLE_GATT_WRITE:
	case ALP_CC3501E_CMD_GPIO_SET_INTERRUPT:
	case ALP_CC3501E_CMD_CAM_ENABLE:
	case ALP_CC3501E_CMD_CAM_DISABLE:
	case ALP_CC3501E_CMD_POWER_POLICY:
	case ALP_CC3501E_CMD_DIAG_LOG_LEVEL:
	case ALP_CC3501E_CMD_SOCK_CONNECT:
	case ALP_CC3501E_CMD_SOCK_CLOSE:
		/* Argless / write-only ops: success is the bare OK status. */
		stage_status(ALP_CC3501E_RESP_OK);
		break;

	case ALP_CC3501E_CMD_GET_VERSION: {
		const uint8_t v[2] = { (uint8_t)(ALP_CC3501E_PROTOCOL_VERSION & 0xFFu),
			                   (uint8_t)((ALP_CC3501E_PROTOCOL_VERSION >> 8) & 0xFFu) };
		stage_reply(ALP_CC3501E_RESP_OK, v, 2u);
		break;
	}
	case ALP_CC3501E_CMD_GET_MAC:
		stage_reply(ALP_CC3501E_RESP_OK, FIX_MAC, 6u);
		break;

	case ALP_CC3501E_CMD_GET_DIAG_INFO: {
		/* 16-byte alp_cc3501e_diag_info_t: fw_version(LE16) | reset_cause |
		 * role | uptime_ms(LE32) | free_heap(LE32) | last_error | reserved[3]. */
		uint8_t d[16] = { 0 };
		d[0]          = 0x02u; /* fw_version = 0x0102 */
		d[1]          = 0x01u;
		d[2]          = ALP_CC3501E_RESET_POWER_ON;
		d[3]          = ALP_CC3501E_ROLE_WIFI_STA;
		d[4]          = 0xEFu; /* uptime = 0x00ABCDEF */
		d[5]          = 0xCDu;
		d[6]          = 0xABu;
		d[7]          = 0x00u;
		d[8]          = 0x40u; /* free_heap = 0x00012340 */
		d[9]          = 0x23u;
		d[10]         = 0x01u;
		d[11]         = 0x00u;
		d[12]         = ALP_CC3501E_RESP_OK; /* last_error */
		stage_reply(ALP_CC3501E_RESP_OK, d, 16u);
		break;
	}
	case ALP_CC3501E_CMD_DIAG_GET_STATS: {
		uint8_t s[8] = { 0x44, 0x33, 0x22, 0x11,   /* frames_ok  = 0x11223344 */
			             0x05, 0x00, 0x00, 0x00 }; /* frames_err = 0x00000005 */
		stage_reply(ALP_CC3501E_RESP_OK, s, 8u);
		break;
	}
	case ALP_CC3501E_CMD_WIFI_GET_RSSI: {
		const uint8_t r = (uint8_t)(-42);
		stage_reply(ALP_CC3501E_RESP_OK, &r, 1u);
		break;
	}
	case ALP_CC3501E_CMD_WIFI_GET_IP: {
		/* On the wire the octets arrive REVERSED (the firmware extracts the lwIP
		 * network-order u32 MSB-first); the host reverses them back.  Stage the
		 * wire order for 192.168.1.14 (0xC0A8010E) = {0x0E,0x01,0xA8,0xC0}. */
		const uint8_t wire[4] = { 0x0E, 0x01, 0xA8, 0xC0 };
		stage_reply(ALP_CC3501E_RESP_OK, wire, 4u);
		break;
	}
	case ALP_CC3501E_CMD_WIFI_STATUS: {
		uint8_t st[4];
		st[0] = ALP_CC3501E_WIFI_CONNECTED;
		st[1] = ALP_CC3501E_WIFI_FAIL_NONE;
		st[2] = (uint8_t)(-50); /* rssi_dbm */
		st[3] = 0u;
		stage_reply(ALP_CC3501E_RESP_OK, st, 4u);
		break;
	}
	case ALP_CC3501E_CMD_WIFI_SCAN_START: {
		uint8_t  recs[ALP_CC3501E_MAX_PAYLOAD];
		uint16_t n = build_wifi_scan(recs);
		stage_reply(ALP_CC3501E_RESP_OK, recs, n);
		break;
	}
	case ALP_CC3501E_CMD_BLE_SCAN_START: {
		uint8_t  recs[ALP_CC3501E_MAX_PAYLOAD];
		uint16_t n = build_ble_scan(recs);
		stage_reply(ALP_CC3501E_RESP_OK, recs, n);
		break;
	}
	case ALP_CC3501E_CMD_BLE_GATT_READ: {
		const uint8_t val[2] = { 0xAB, 0xCD }; /* attribute value bytes */
		stage_reply(ALP_CC3501E_RESP_OK, val, 2u);
		break;
	}
	case ALP_CC3501E_CMD_SOCK_OPEN: {
		/* reply DATA = alp_cc3501e_sock_handle_t { handle(LE16) | rsvd[2] }. */
		const uint8_t h[4] = { 0x34, 0x12, 0x00, 0x00 }; /* handle 0x1234 */
		stage_reply(ALP_CC3501E_RESP_OK, h, 4u);
		break;
	}
	case ALP_CC3501E_CMD_SOCK_SEND: {
		/* Echo the inline data_len (bytes [4..5] of the send header) as the
		 * accepted count -- the firmware queues everything in this model. */
		uint16_t      dl   = (uint16_t)slave.req_pl[4] | ((uint16_t)slave.req_pl[5] << 8);
		const uint8_t c[2] = { (uint8_t)(dl & 0xFFu), (uint8_t)((dl >> 8) & 0xFFu) };
		stage_reply(ALP_CC3501E_RESP_OK, c, 2u);
		break;
	}
	case ALP_CC3501E_CMD_SOCK_RECV: {
		/* reply DATA = sock_addr(20) | data_len(LE16) | reserved(2) | data[]. */
		static const uint8_t payload[5] = { 'h', 'e', 'l', 'l', 'o' };
		uint8_t              d[24 + 5];
		memset(d, 0, sizeof(d));
		d[20] = (uint8_t)sizeof(payload); /* data_len lo */
		d[21] = 0u;                       /* data_len hi */
		memcpy(&d[24], payload, sizeof(payload));
		stage_reply(ALP_CC3501E_RESP_OK, d, (uint16_t)sizeof(d));
		break;
	}
	case ALP_CC3501E_CMD_GPIO_CONFIGURE:
		/* Accept; the pin model needs no state change on configure. */
		stage_status(ALP_CC3501E_RESP_OK);
		break;
	case ALP_CC3501E_CMD_GPIO_WRITE: {
		uint8_t pad = slave.req_pl[0];
		if (pad < sizeof(slave.pin_level)) {
			slave.pin_level[pad] = slave.req_pl[1] ? 1u : 0u;
		}
		stage_status(ALP_CC3501E_RESP_OK);
		break;
	}
	case ALP_CC3501E_CMD_GPIO_READ: {
		uint8_t pad = slave.req_pl[0];
		uint8_t lvl = (pad < sizeof(slave.pin_level)) ? slave.pin_level[pad] : 0u;
		stage_reply(ALP_CC3501E_RESP_OK, &lvl, 1u);
		break;
	}
	default:
		stage_status(ALP_CC3501E_RESP_ERR_INVALID);
		break;
	}
}

/* ---- test doubles for the alp_* seams the host driver links against -------- */

alp_status_t alp_spi_transceive(alp_spi_t *bus, const uint8_t *tx, uint8_t *rx, size_t len)
{
	(void)bus;
	if (len == 0u) {
		return ALP_OK;
	}
	switch (slave.phase) {
	case PH_REQ_HDR:
		slave.cmd     = tx[0];
		slave.req_len = (uint16_t)tx[2] | ((uint16_t)tx[3] << 8);
		if (rx != NULL) {
			memset(rx, ALP_CC3501E_SYNC_IDLE, len);
		}
		if (slave.req_len > 0u) {
			slave.phase = PH_REQ_PL;
		} else {
			slave_dispatch();
			slave.phase = PH_REPLY_HDR;
		}
		break;
	case PH_REQ_PL:
		memcpy(slave.req_pl, tx, len);
		if (rx != NULL) {
			memset(rx, ALP_CC3501E_SYNC_IDLE, len);
		}
		slave_dispatch();
		slave.phase = PH_REPLY_HDR;
		break;
	case PH_REPLY_HDR:
		rx[0]       = slave.cmd; /* reply header echoes the cmd */
		rx[1]       = 0x00u;     /* solicited */
		rx[2]       = (uint8_t)(slave.reply_len & 0xFFu);
		rx[3]       = (uint8_t)((slave.reply_len >> 8) & 0xFFu);
		slave.phase = PH_REPLY_PL;
		break;
	case PH_REPLY_PL:
		memcpy(rx, slave.reply_pl, len);
		slave.phase = PH_REQ_HDR;
		break;
	}
	return ALP_OK;
}

/* Delays are no-ops under the sim; the GPIO seams are inert (the fixture's ctx
 * leaves reset/enable/ready pins unset, so the wrappers under test never call
 * them -- they exercise cc3501e_request, not the reset-pin pulse). */
void alp_delay_us(uint32_t us)
{
	(void)us;
}
void alp_delay_ms(uint32_t ms)
{
	(void)ms;
}
alp_gpio_t *alp_gpio_open(uint32_t pin_id)
{
	(void)pin_id;
	return NULL;
}
alp_status_t alp_gpio_write(alp_gpio_t *pin, bool level)
{
	(void)pin;
	(void)level;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_gpio_read(alp_gpio_t *pin, bool *level)
{
	(void)pin;
	(void)level;
	return ALP_ERR_NOSUPPORT;
}

/* ---- fixture --------------------------------------------------------------- */

static cc3501e_t  fw;
static alp_spi_t *fake_bus = (alp_spi_t *)&fw; /* opaque, non-NULL; the stub ignores it */

static void reset_before(void *fixture)
{
	(void)fixture;
	slave_reset();
	zassert_equal(cc3501e_init(&fw, fake_bus), ALP_OK, "init binds the (fake) bus");
}

/* ================================ META ===================================== */

ZTEST(cc3501e_host_driver, test_ping_encodes_opcode)
{
	zassert_equal(cc3501e_ping(&fw), ALP_OK, "PING -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_PING, "opcode 0x00 reached the slave");
	zassert_equal(slave.req_len, 0u, "PING carries no payload");
}

ZTEST(cc3501e_host_driver, test_soft_reset_encodes_opcode)
{
	zassert_equal(cc3501e_soft_reset(&fw), ALP_OK, "RESET -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_RESET, "opcode 0x02 reached the slave");
}

ZTEST(cc3501e_host_driver, test_get_version_decodes_le16)
{
	uint16_t v = 0u;
	zassert_equal(cc3501e_get_version(&fw, &v), ALP_OK, "GET_VERSION -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_GET_VERSION, "opcode 0x01");
	zassert_equal(v, (uint16_t)ALP_CC3501E_PROTOCOL_VERSION, "decoded LE16 protocol version");
}

/* ============================ DIAGNOSTICS ================================== */

ZTEST(cc3501e_host_driver, test_diag_info_decodes_all_fields)
{
	alp_cc3501e_diag_info_t d;
	memset(&d, 0xA5, sizeof(d));
	zassert_equal(cc3501e_diag_info(&fw, &d), ALP_OK, "GET_DIAG_INFO -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_GET_DIAG_INFO, "opcode 0x04");
	zassert_equal(d.fw_version, 0x0102u, "fw_version LE16");
	zassert_equal(d.reset_cause, ALP_CC3501E_RESET_POWER_ON, "reset_cause");
	zassert_equal(d.role, ALP_CC3501E_ROLE_WIFI_STA, "role");
	zassert_equal(d.uptime_ms, 0x00ABCDEFu, "uptime_ms LE32");
	zassert_equal(d.free_heap_bytes, 0x00012340u, "free_heap_bytes LE32");
	zassert_equal(d.last_error, ALP_CC3501E_RESP_OK, "last_error");
}

ZTEST(cc3501e_host_driver, test_diag_info_null_out_invalid)
{
	zassert_equal(cc3501e_diag_info(&fw, NULL), ALP_ERR_INVAL, "NULL out -> INVAL");
	zassert_equal(slave.cmd, 0u, "no transfer clocked");
}

ZTEST(cc3501e_host_driver, test_diag_stats_decodes_two_le32)
{
	uint32_t ok = 0u, err = 0u;
	zassert_equal(cc3501e_diag_stats(&fw, &ok, &err), ALP_OK, "DIAG_GET_STATS -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_DIAG_GET_STATS, "opcode 0x70");
	zassert_equal(ok, 0x11223344u, "frames_ok LE32");
	zassert_equal(err, 0x00000005u, "frames_err LE32");
}

ZTEST(cc3501e_host_driver, test_diag_log_level_encodes_level_byte)
{
	zassert_equal(cc3501e_diag_log_level(&fw, 3u), ALP_OK, "DIAG_LOG_LEVEL -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_DIAG_LOG_LEVEL, "opcode 0x71");
	zassert_equal(slave.req_len, 1u, "single level byte");
	zassert_equal(slave.req_pl[0], 3u, "level byte value");
}

/* =============================== WI-FI ===================================== */

ZTEST(cc3501e_host_driver, test_wifi_get_mac_decodes_6_bytes)
{
	uint8_t mac[CC3501E_MAC_LEN] = { 0 };
	zassert_equal(cc3501e_wifi_get_mac(&fw, mac, 100u), ALP_OK, "GET_MAC -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_GET_MAC, "opcode 0x03");
	zassert_mem_equal(mac, FIX_MAC, CC3501E_MAC_LEN, "6-byte MAC decoded");
}

ZTEST(cc3501e_host_driver, test_wifi_rssi_decodes_signed)
{
	int8_t rssi = 0;
	zassert_equal(cc3501e_wifi_rssi(&fw, &rssi), ALP_OK, "GET_RSSI -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_WIFI_GET_RSSI, "opcode 0x16");
	zassert_equal(rssi, -42, "signed dBm decoded");
}

/* The network-order -> dotted-quad fix: the wire octets arrive reversed and the
 * host reverses them.  0xC0A8010E on the wire must decode to {192,168,1,14}. */
ZTEST(cc3501e_host_driver, test_wifi_get_ip_byte_order)
{
	uint8_t ip[4] = { 0 };
	zassert_equal(cc3501e_wifi_get_ip(&fw, ip), ALP_OK, "GET_IP -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_WIFI_GET_IP, "opcode 0x17");
	zassert_equal(ip[0], 192, "ip[0]");
	zassert_equal(ip[1], 168, "ip[1]");
	zassert_equal(ip[2], 1, "ip[2]");
	zassert_equal(ip[3], 14, "ip[3] -- 0xC0A8010E -> 192.168.1.14");
}

ZTEST(cc3501e_host_driver, test_wifi_status_decodes_fields)
{
	alp_cc3501e_wifi_status_t st;
	memset(&st, 0xA5, sizeof(st));
	zassert_equal(cc3501e_wifi_status(&fw, &st), ALP_OK, "WIFI_STATUS -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_WIFI_STATUS, "opcode 0x1B");
	zassert_equal(st.state, ALP_CC3501E_WIFI_CONNECTED, "state");
	zassert_equal(st.fail_reason, ALP_CC3501E_WIFI_FAIL_NONE, "fail_reason");
	zassert_equal(st.rssi_dbm, -50, "rssi_dbm (signed)");
}

ZTEST(cc3501e_host_driver, test_wifi_status_null_out_invalid)
{
	zassert_equal(cc3501e_wifi_status(&fw, NULL), ALP_ERR_INVAL, "NULL out -> INVAL");
	zassert_equal(slave.cmd, 0u, "no transfer clocked");
}

/* SCAN_START reply is a packed sequence of records; the host walks them out into
 * the caller's array, copying + NUL-terminating each length-prefixed SSID. */
ZTEST(cc3501e_host_driver, test_wifi_scan_walks_records)
{
	cc3501e_scan_record_t recs[8];
	size_t                n = 0u;
	zassert_equal(cc3501e_wifi_scan(&fw, recs, 8u, &n, 100u), ALP_OK, "SCAN -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_WIFI_SCAN_START, "opcode 0x10");
	zassert_equal(n, 2u, "two records parsed");

	zassert_equal(recs[0].channel, 6u, "rec0 channel");
	zassert_equal(recs[0].rssi_dbm, -40, "rec0 rssi (signed)");
	zassert_equal(recs[0].ssid_len, 4u, "rec0 ssid_len");
	zassert_str_equal(recs[0].ssid, "Test", "rec0 SSID copied + NUL-terminated");
	zassert_equal(recs[0].security_info, 0x0400u, "rec0 security_info LE16");
	zassert_equal(cc3501e_wifi_sec_kind(recs[0].security_info), CC3501E_WIFI_SEC_WPA2, "rec0 WPA2");

	zassert_equal(recs[1].channel, 11u, "rec1 channel");
	zassert_str_equal(recs[1].ssid, "OpenNet", "rec1 SSID");
	zassert_equal(cc3501e_wifi_sec_kind(recs[1].security_info), CC3501E_WIFI_SEC_OPEN, "rec1 open");
}

/* The scan-security decoder is a pure host function over the raw TI SecurityInfo. */
ZTEST(cc3501e_host_driver, test_wifi_sec_kind_and_name)
{
	zassert_equal(cc3501e_wifi_sec_kind(0x0000u), CC3501E_WIFI_SEC_OPEN, "open");
	zassert_equal(cc3501e_wifi_sec_kind(0x0400u), CC3501E_WIFI_SEC_WPA2, "wpa2 (bit 0x04)");
	zassert_equal(cc3501e_wifi_sec_kind(0x0800u), CC3501E_WIFI_SEC_WPA3, "wpa3 (SAE bit 0x08)");
	zassert_equal(cc3501e_wifi_sec_kind(0x1000u), CC3501E_WIFI_SEC_WPA3, "wpa3 (SAE bit 0x10)");
	zassert_str_equal(cc3501e_wifi_sec_name(0x0000u), "open", "name open");
	zassert_str_equal(cc3501e_wifi_sec_name(0x0400u), "wpa2", "name wpa2");
	zassert_str_equal(cc3501e_wifi_sec_name(0x0800u), "wpa3", "name wpa3");
}

/* CONNECT packs the connect header (ssid_len | psk_len | security | rsvd) then
 * the inline SSID then the inline passphrase, with no padding. */
ZTEST(cc3501e_host_driver, test_wifi_connect_encodes_header_ssid_psk)
{
	zassert_equal(
	    cc3501e_wifi_connect(&fw, "mynet", 1u, "secretpw", 100u), ALP_OK, "CONNECT -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_WIFI_CONNECT_STA, "opcode 0x12");
	/* header(4) + ssid(5) + psk(8) = 17. */
	zassert_equal(slave.req_len, 4u + 5u + 8u, "payload = header + ssid + psk");
	zassert_equal(slave.req_pl[0], 5u, "ssid_len");
	zassert_equal(slave.req_pl[1], 8u, "psk_len");
	zassert_equal(slave.req_pl[2], 1u, "security");
	zassert_mem_equal(&slave.req_pl[4], "mynet", 5u, "inline SSID");
	zassert_mem_equal(&slave.req_pl[9], "secretpw", 8u, "inline passphrase");
}

ZTEST(cc3501e_host_driver, test_wifi_connect_oversize_ssid_rejected)
{
	static const char big[40] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"; /* 35 > 32 */
	zassert_equal(cc3501e_wifi_connect(&fw, big, 1u, "pw", 100u),
	              ALP_ERR_INVAL,
	              "SSID > 32 -> INVAL (host guard)");
	zassert_equal(slave.cmd, 0u, "no transfer clocked");
}

ZTEST(cc3501e_host_driver, test_wifi_ap_start_encodes_like_connect)
{
	zassert_equal(cc3501e_wifi_ap_start(&fw, "AP", 0u, "", 100u), ALP_OK, "AP_START -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_WIFI_AP_START, "opcode 0x14");
	zassert_equal(slave.req_pl[0], 2u, "ssid_len");
	zassert_equal(slave.req_pl[1], 0u, "psk_len (open)");
	zassert_mem_equal(&slave.req_pl[4], "AP", 2u, "inline SSID");
}

ZTEST(cc3501e_host_driver, test_wifi_disconnect_and_ap_stop_argless)
{
	zassert_equal(cc3501e_wifi_disconnect(&fw), ALP_OK, "DISCONNECT -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_WIFI_DISCONNECT, "opcode 0x13");
	slave_reset();
	zassert_equal(cc3501e_wifi_ap_stop(&fw), ALP_OK, "AP_STOP -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_WIFI_AP_STOP, "opcode 0x15");
}

/* =============================== SOCKETS =================================== */

ZTEST(cc3501e_host_driver, test_sock_open_encodes_and_decodes_handle)
{
	uint16_t h = 0u;
	zassert_equal(
	    cc3501e_sock_open(
	        &fw, ALP_CC3501E_SOCK_FAMILY_IPV4, ALP_CC3501E_SOCK_TYPE_STREAM, 0u, &h, 100u),
	    ALP_OK,
	    "SOCK_OPEN -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_SOCK_OPEN, "opcode 0x20");
	zassert_equal(slave.req_len, 4u, "open payload = {family,type,protocol,rsvd}");
	zassert_equal(slave.req_pl[0], (uint8_t)ALP_CC3501E_SOCK_FAMILY_IPV4, "family");
	zassert_equal(slave.req_pl[1], (uint8_t)ALP_CC3501E_SOCK_TYPE_STREAM, "type");
	zassert_equal(h, 0x1234u, "decoded LE16 handle");
}

ZTEST(cc3501e_host_driver, test_sock_open_null_handle_invalid)
{
	zassert_equal(
	    cc3501e_sock_open(&fw, 0u, 0u, 0u, NULL, 100u), ALP_ERR_INVAL, "NULL handle_out -> INVAL");
	zassert_equal(slave.cmd, 0u, "no transfer clocked");
}

/* CONNECT packs handle(LE16) | rsvd(2) | sock_addr{ family | rsvd | port(LE16) |
 * addr[16] }; the IPv4 octets land at addr[0..3]. */
ZTEST(cc3501e_host_driver, test_sock_connect_encodes_addr_and_port)
{
	const uint8_t ip[4] = { 93, 184, 216, 34 }; /* 93.184.216.34 */
	zassert_equal(cc3501e_sock_connect(&fw, 0x1234u, ip, 80u, 100u), ALP_OK, "CONNECT -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_SOCK_CONNECT, "opcode 0x21");
	zassert_equal(slave.req_len, 24u, "connect payload is 24 bytes");
	zassert_equal(slave.req_pl[0], 0x34u, "handle lo");
	zassert_equal(slave.req_pl[1], 0x12u, "handle hi");
	zassert_equal(slave.req_pl[4], (uint8_t)ALP_CC3501E_SOCK_FAMILY_IPV4, "peer.family");
	zassert_equal(slave.req_pl[6], 80u, "peer.port lo (host order on the wire)");
	zassert_equal(slave.req_pl[7], 0u, "peer.port hi");
	zassert_mem_equal(&slave.req_pl[8], ip, 4u, "peer.addr[0..3] = the IPv4 octets");
}

/* SEND packs the 8-byte send header (handle | flags | rsvd | data_len | rsvd2)
 * then the inline data, and decodes the LE16 accepted count from the reply. */
ZTEST(cc3501e_host_driver, test_sock_send_encodes_header_and_data)
{
	const uint8_t data[5] = { 'G', 'E', 'T', ' ', '/' };
	size_t        sent    = 0u;
	zassert_equal(
	    cc3501e_sock_send(&fw, 0x1234u, data, sizeof(data), &sent, 100u), ALP_OK, "SEND -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_SOCK_SEND, "opcode 0x22");
	zassert_equal(slave.req_len, 8u + 5u, "send payload = 8-byte header + data");
	zassert_equal(slave.req_pl[0], 0x34u, "handle lo");
	zassert_equal(slave.req_pl[1], 0x12u, "handle hi");
	zassert_equal(slave.req_pl[4], 5u, "data_len lo");
	zassert_equal(slave.req_pl[5], 0u, "data_len hi");
	zassert_mem_equal(&slave.req_pl[8], data, 5u, "inline data after the header");
	zassert_equal(sent, 5u, "decoded accepted byte count");
}

/* RECV requests up to @cap bytes and decodes the 24-byte recv-resp header +
 * the inline received bytes that follow it. */
ZTEST(cc3501e_host_driver, test_sock_recv_encodes_maxlen_and_decodes_data)
{
	uint8_t buf[32] = { 0 };
	size_t  got     = 0u;
	zassert_equal(
	    cc3501e_sock_recv(&fw, 0x1234u, buf, sizeof(buf), &got, 100u), ALP_OK, "RECV -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_SOCK_RECV, "opcode 0x23");
	zassert_equal(slave.req_len, 4u, "recv payload = {handle(LE16), max_len(LE16)}");
	zassert_equal(slave.req_pl[0], 0x34u, "handle lo");
	zassert_equal(slave.req_pl[1], 0x12u, "handle hi");
	zassert_equal(slave.req_pl[2], 32u, "max_len lo (= cap, bounded)");
	zassert_equal(got, 5u, "decoded data_len from the 24-byte resp header");
	zassert_mem_equal(buf, "hello", 5u, "inline received bytes copied out");
}

ZTEST(cc3501e_host_driver, test_sock_close_encodes_handle)
{
	zassert_equal(cc3501e_sock_close(&fw, 0x1234u, 100u), ALP_OK, "CLOSE -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_SOCK_CLOSE, "opcode 0x24");
	zassert_equal(slave.req_pl[0], 0x34u, "handle lo");
	zassert_equal(slave.req_pl[1], 0x12u, "handle hi");
}

/* ================================ BLE ====================================== */

ZTEST(cc3501e_host_driver, test_ble_enable_disable_argless)
{
	zassert_equal(cc3501e_ble_enable(&fw, 100u), ALP_OK, "BLE_ENABLE -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_BLE_ENABLE, "opcode 0x30");
	slave_reset();
	zassert_equal(cc3501e_ble_disable(&fw, 100u), ALP_OK, "BLE_DISABLE -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_BLE_DISABLE, "opcode 0x31");
}

/* BLE_SCAN_START reply is a packed sequence of advertising reports; the host
 * walks them, copying + NUL-terminating each length-prefixed device name. */
ZTEST(cc3501e_host_driver, test_ble_scan_walks_records)
{
	cc3501e_ble_scan_record_t recs[8];
	size_t                    n = 0u;
	zassert_equal(cc3501e_ble_scan(&fw, recs, 8u, &n, 100u), ALP_OK, "BLE_SCAN -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_BLE_SCAN_START, "opcode 0x34");
	zassert_equal(n, 2u, "two advertisers parsed");

	zassert_equal(recs[0].addr_type, 0u, "rec0 public addr");
	zassert_equal(recs[0].rssi_dbm, -55, "rec0 rssi (signed)");
	zassert_str_equal(recs[0].name, "MyBLE", "rec0 name copied + NUL-terminated");

	zassert_equal(recs[1].addr_type, 1u, "rec1 random addr");
	zassert_equal(recs[1].rssi_dbm, -88, "rec1 rssi");
	zassert_equal(recs[1].name_len, 0u, "rec1 nameless");
	zassert_str_equal(recs[1].name, "", "rec1 name empty");
}

/* ADV_START hand-packs the 7-byte header (the doc struct's 8th pad byte is
 * omitted on the wire) then the inline advertising data. */
ZTEST(cc3501e_host_driver, test_ble_adv_start_encodes_7byte_header)
{
	const uint8_t adv[3] = { 0x02, 0x01, 0x06 }; /* flags AD */
	zassert_equal(cc3501e_ble_adv_start(&fw, true, 100u, 200u, adv, sizeof(adv), 100u),
	              ALP_OK,
	              "ADV_START -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_BLE_ADV_START, "opcode 0x32");
	zassert_equal(slave.req_len, 7u + 3u, "payload = 7-byte header + adv data");
	zassert_equal(slave.req_pl[0], 1u, "connectable");
	zassert_equal(slave.req_pl[1], 0u, "reserved");
	zassert_equal(
	    (uint16_t)slave.req_pl[2] | ((uint16_t)slave.req_pl[3] << 8), 100u, "interval_min_ms LE16");
	zassert_equal(
	    (uint16_t)slave.req_pl[4] | ((uint16_t)slave.req_pl[5] << 8), 200u, "interval_max_ms LE16");
	zassert_equal(slave.req_pl[6], 3u, "adv_data_len");
	zassert_mem_equal(&slave.req_pl[7], adv, 3u, "inline adv data");
}

/* BLE_CONNECT packs addr_type FIRST, then the 6 address bytes. */
ZTEST(cc3501e_host_driver, test_ble_connect_encodes_addr_type_first)
{
	const uint8_t addr[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11 };
	zassert_equal(cc3501e_ble_connect(&fw, addr, 1u, 100u), ALP_OK, "BLE_CONNECT -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_BLE_CONNECT, "opcode 0x36");
	zassert_equal(slave.req_len, 7u, "payload = addr_type(1) + addr(6)");
	zassert_equal(slave.req_pl[0], 1u, "addr_type first");
	zassert_mem_equal(&slave.req_pl[1], addr, 6u, "addr[6] after addr_type");
}

/* GATT_WRITE packs handle(LE16) then the value bytes. */
ZTEST(cc3501e_host_driver, test_ble_gatt_write_encodes_handle_and_value)
{
	const uint8_t val[3] = { 0x11, 0x22, 0x33 };
	zassert_equal(
	    cc3501e_ble_gatt_write(&fw, 0x0042u, val, sizeof(val), 100u), ALP_OK, "GATT_WRITE -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_BLE_GATT_WRITE, "opcode 0x3B");
	zassert_equal(slave.req_len, 2u + 3u, "payload = handle(LE16) + value");
	zassert_equal(slave.req_pl[0], 0x42u, "handle lo");
	zassert_equal(slave.req_pl[1], 0x00u, "handle hi");
	zassert_mem_equal(&slave.req_pl[2], val, 3u, "value bytes after handle");
}

/* GATT_READ requests handle(LE16); the reply DATA is the attribute value. */
ZTEST(cc3501e_host_driver, test_ble_gatt_read_decodes_value)
{
	uint8_t out[8] = { 0 };
	size_t  n      = 0u;
	zassert_equal(
	    cc3501e_ble_gatt_read(&fw, 0x0042u, out, sizeof(out), &n, 100u), ALP_OK, "GATT_READ -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_BLE_GATT_READ, "opcode 0x3A");
	zassert_equal(slave.req_len, 2u, "request = handle(LE16)");
	zassert_equal(slave.req_pl[0], 0x42u, "handle lo");
	zassert_equal(n, 2u, "decoded value length");
	zassert_equal(out[0], 0xABu, "value[0]");
	zassert_equal(out[1], 0xCDu, "value[1]");
}

/* ======================== GPIO PROXY + POWER =============================== */

/* Configure -> write-high -> read-back-high -> write-low -> read-back-low
 * round-trips through the real wire encode/decode against the pin model. */
ZTEST(cc3501e_host_driver, test_gpio_configure_write_read_roundtrip)
{
	zassert_equal(cc3501e_gpio_configure(
	                  &fw, 13u, ALP_CC3501E_GPIO_DIR_OUTPUT, ALP_CC3501E_GPIO_PULL_NONE, 100u),
	              ALP_OK,
	              "CONFIGURE -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_GPIO_CONFIGURE, "opcode 0x50");
	zassert_equal(slave.req_pl[0], 13u, "pad index");
	zassert_equal(slave.req_pl[1], (uint8_t)ALP_CC3501E_GPIO_DIR_OUTPUT, "direction");

	zassert_equal(cc3501e_gpio_write(&fw, 13u, true, 100u), ALP_OK, "WRITE high -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_GPIO_WRITE, "opcode 0x51");
	zassert_equal(slave.req_pl[0], 13u, "pad index");
	zassert_equal(slave.req_pl[1], 1u, "level high");

	bool level = false;
	zassert_equal(cc3501e_gpio_read(&fw, 13u, &level, 100u), ALP_OK, "READ -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_GPIO_READ, "opcode 0x52");
	zassert_true(level, "read reflects the written-high level");

	zassert_equal(cc3501e_gpio_write(&fw, 13u, false, 100u), ALP_OK, "WRITE low -> OK");
	zassert_equal(cc3501e_gpio_read(&fw, 13u, &level, 100u), ALP_OK, "READ -> OK");
	zassert_false(level, "read reflects the written-low level");
}

ZTEST(cc3501e_host_driver, test_gpio_read_null_out_invalid)
{
	zassert_equal(cc3501e_gpio_read(&fw, 13u, NULL, 100u), ALP_ERR_INVAL, "NULL out -> INVAL");
	zassert_equal(slave.cmd, 0u, "no transfer clocked");
}

ZTEST(cc3501e_host_driver, test_gpio_set_interrupt_encodes_fields)
{
	zassert_equal(cc3501e_gpio_set_interrupt(&fw, 7u, ALP_CC3501E_GPIO_EDGE_RISING, true, 100u),
	              ALP_OK,
	              "SET_INTERRUPT -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_GPIO_SET_INTERRUPT, "opcode 0x53");
	zassert_equal(slave.req_pl[0], 7u, "pad index");
	zassert_equal(slave.req_pl[1], (uint8_t)ALP_CC3501E_GPIO_EDGE_RISING, "edge");
	zassert_equal(slave.req_pl[2], 1u, "enabled");
}

ZTEST(cc3501e_host_driver, test_cam_enable_disable_selects_opcode)
{
	zassert_equal(cc3501e_cam_enable(&fw, 1u, true, 100u), ALP_OK, "CAM_ENABLE -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_CAM_ENABLE, "on -> opcode 0x60");
	zassert_equal(slave.req_pl[0], 1u, "which LDO");
	slave_reset();
	zassert_equal(cc3501e_cam_enable(&fw, 0u, false, 100u), ALP_OK, "CAM_DISABLE -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_CAM_DISABLE, "off -> opcode 0x61");
}

/* POWER_POLICY hand-packs the 8-byte wire (policy | wake | rsvd(2) | idle(LE32)),
 * NOT the doc struct which carries alignment padding. */
ZTEST(cc3501e_host_driver, test_power_policy_encodes_8_bytes)
{
	const alp_cc3501e_power_policy_t pp = {
		.policy               = ALP_CC3501E_PP_BALANCED,
		.wake_events          = ALP_CC3501E_WAKE_HOST_SPI,
		.reserved             = 0u,
		.idle_ms_before_sleep = 1000u, /* 0x000003E8 */
	};
	zassert_equal(cc3501e_power_policy(&fw, &pp, 100u), ALP_OK, "POWER_POLICY -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_POWER_POLICY, "opcode 0x62");
	zassert_equal(slave.req_len, 8u, "hand-packed 8-byte wire");
	zassert_equal(slave.req_pl[0], (uint8_t)ALP_CC3501E_PP_BALANCED, "policy");
	zassert_equal(slave.req_pl[1], (uint8_t)ALP_CC3501E_WAKE_HOST_SPI, "wake_events");
	zassert_equal(slave.req_pl[2], 0u, "reserved lo");
	zassert_equal(slave.req_pl[3], 0u, "reserved hi");
	zassert_equal((uint32_t)slave.req_pl[4] | ((uint32_t)slave.req_pl[5] << 8) |
	                  ((uint32_t)slave.req_pl[6] << 16) | ((uint32_t)slave.req_pl[7] << 24),
	              1000u,
	              "idle_ms_before_sleep LE32");
}

ZTEST(cc3501e_host_driver, test_power_policy_null_invalid)
{
	zassert_equal(cc3501e_power_policy(&fw, NULL, 100u), ALP_ERR_INVAL, "NULL policy -> INVAL");
	zassert_equal(slave.cmd, 0u, "no transfer clocked");
}

ZTEST_SUITE(cc3501e_host_driver, NULL, NULL, reset_before, NULL, NULL);
