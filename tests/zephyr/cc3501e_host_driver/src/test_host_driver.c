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
 * (cc3501e-bridge-firmware:hal/ti/transport_hw_ti_spi.c): a 4-byte LE header
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

	/* ---- fire-and-forget WIFI_CONNECT_STA / WIFI_STATUS model (#1376/#1377/
	 * #1378) -----------------------------------------------------------------
	 * A dedicated snapshot of the CONNECT_STA submit's own request bytes,
	 * because the generic cmd/req_len/req_pl above reflect whichever request
	 * was dispatched LAST -- cc3501e_wifi_connect() now issues the submit
	 * once and then polls WIFI_STATUS afterwards, so by the time it returns
	 * the generic fields hold WIFI_STATUS's (empty) request, not the
	 * connect's. */
	uint16_t connect_last_req_len;
	uint8_t  connect_last_req_pl[ALP_CC3501E_MAX_PAYLOAD];
	uint32_t connect_submit_count; /* how many CONNECT_STA submits landed */

	/* Same snapshot for WIFI_AP_START (#1385): AP_START is worker-routed
	 * through the IDENTICAL firmware handler as CONNECT_STA
	 * (handle_worker_routed_payload) and its host wrapper retries, so the
	 * generic req_pl/req_len hold whichever attempt landed LAST. */
	uint16_t ap_start_last_req_len;
	uint8_t  ap_start_last_req_pl[ALP_CC3501E_MAX_PAYLOAD];
	uint32_t ap_start_submit_count; /* how many AP_START submits landed */

	/* The WIFI_STATUS latch WIFI_CONNECT_STA drives + WIFI_STATUS reads --
	 * models the firmware's async connect-status latch (handle_wifi_status /
	 * cc3501e_hw_wifi_conn_status). */
	uint8_t wifi_conn_state;
	uint8_t wifi_fail_reason;
	int8_t  wifi_conn_rssi;

	/* Number of WIFI_STATUS polls that must still read CONNECTING before the
	 * latch above is reported -- simulates an association that takes a few
	 * polls to resolve, so a regression test can prove cc3501e_wifi_connect()
	 * polls WIFI_STATUS repeatedly while submitting CONNECT_STA exactly once. */
	uint32_t status_polls_before_terminal;

	/* Number of WIFI_GET_RSSI submits that must still ack RESP_ERR_BUSY
	 * before the real value is handed back -- models GET_RSSI's real
	 * worker-routed submit/collect shape (#1377). */
	uint32_t rssi_busy_polls_remaining;

	/* Every WIFI_STATUS request-header phase clocked, fault-injected or not
	 * -- lets a test count how many WIFI_STATUS attempts
	 * cc3501e_wifi_connect()'s own poll loop made for a given timeout_ms
	 * (#1382 timeout-accounting regression). */
	uint32_t wifi_status_attempt_count;

	/* #1435 entry-clean ordering: every opcode dispatched, in order.
	 * slave.cmd alone only ever holds the LAST opcode dispatched, which
	 * cannot prove WIFI_DISCONNECT landed BEFORE WIFI_CONNECT_STA -- this
	 * log can. Capacity is generous for one cc3501e_wifi_connect() call's
	 * worth of traffic; entries past capacity are dropped (cmd_log_count
	 * still counts them) but no #1435 test drives that many. */
	uint8_t  cmd_log[16];
	uint32_t cmd_log_count;

	/* Request FLAGS byte of every request-header phase clocked, in order
	 * (proto v8): bits 3..7 carry the retry seq, and the property under test
	 * is that it stays CONSTANT across the retries of one logical command and
	 * CHANGES between commands.  slave.cmd cannot show either -- it holds one
	 * opcode with no attempt history -- and cmd_log holds opcodes, not flags.
	 * Same capacity + drop-past-capacity rule as cmd_log above. */
	uint8_t  flags_log[16];
	uint32_t flags_log_count;
} slave;

/* Set by test_wifi_scan_buf_is_per_context_740 / test_ble_scan_buf_is_per_context_740
 * to pick the second context's distinct staged reply from slave_dispatch(); cleared by
 * slave_reset() so every other test keeps seeing the default fixtures. */
static bool g_scan_stage_ctx_b;

/* #1378 mutant control: when true, the WIFI_CONNECT_STA / WIFI_AP_START
 * submit handler stages a literal RESP_OK (0x00) status byte instead of the
 * real firmware's unconditional RESP_ERR_BUSY submit ack -- reproducing "a
 * valid header followed by an all-zero payload phase" (this repo's own
 * silicon finding: a dead bus phase reads back 0x00000000, which happens to
 * equal RESP_OK).  Cleared by slave_reset(). */
static bool g_connect_submit_force_ok;
/* Radio role GET_DIAG_INFO reports.  cc3501e_wifi_ap_start() confirms its
 * submit against this field (#1696), so a test can drive the AP up by setting
 * it to ALP_CC3501E_ROLE_WIFI_AP.  Defaults to STA = 'AP not up'. */
static uint8_t g_diag_role = ALP_CC3501E_ROLE_WIFI_STA;

/* FLASH-derived pending image reported in OTA_STATUS byte [12].
 * cc3501e_ota_promote() refuses to commit unless this says STAGED (#1123),
 * so it defaults to STAGED: the promote tests model a device that really
 * does have an installable image waiting. */
static uint8_t g_ota_pending = ALP_CC3501E_OTA_PENDING_STAGED;

/* #1377 mutant control: while > 0, a whole WIFI_STATUS transaction's REQUEST
 * HEADER phase fails outright (as if the shared bridge transport itself were
 * down, e.g. a radio op in flight) -- decremented per attempt, letting a
 * test prove cc3501e_wifi_status() rides the down-window out.  The failure
 * is injected before slave.phase advances, so the next attempt starts clean
 * (models a transport-level fault, not a framing desync needing a resync).
 * Cleared by slave_reset(). */
static uint32_t g_status_io_down_remaining;

/* #1371 mutant controls for cc3501e_reset()'s wire-protocol compatibility
 * gate.  Both cleared by slave_reset() so every other test keeps seeing the
 * default (matching-version, answers-immediately) fixture. */
static bool     g_get_version_override_active; /* stage a specific reply value below */
static uint16_t g_get_version_override_value;
static uint32_t g_get_version_io_down_remaining; /* fail the transaction outright, N times */

/* SPI1 TRANSFER mutant control: when true, the reply echoes seq+1 instead of
 * the request's real seq -- models a desynced/stale reply (the firmware
 * answering some OTHER request) so a test can prove cc3501e_spi1_transfer()
 * treats a seq mismatch as ALP_ERR_IO rather than handing back another
 * transaction's RX bytes.  Cleared by slave_reset(). */
static bool g_spi1_reply_bad_seq;

/* Stage the 16-byte protocol-v8 DIAG_GET_STATS reply instead of the 8-byte v7
 * one.  Cleared by slave_reset(), so the default across the suite is the OLD
 * firmware -- the compatibility direction that would otherwise go untested. */
static bool g_diag_stats_v8;

static void slave_reset(void)
{
	memset(&slave, 0, sizeof(slave));
	slave.phase                        = PH_REQ_HDR;
	slave.wifi_conn_state              = ALP_CC3501E_WIFI_CONNECTED; /* preserves the pre-#1376
	                                                                * behaviour of tests that
	                                                                * poll WIFI_STATUS directly
	                                                                * without going through
	                                                                * cc3501e_wifi_connect() first. */
	slave.wifi_fail_reason             = ALP_CC3501E_WIFI_FAIL_NONE;
	slave.wifi_conn_rssi               = -50;
	slave.status_polls_before_terminal = 0u;
	g_scan_stage_ctx_b                 = false;
	g_connect_submit_force_ok          = false;
	g_diag_role                        = ALP_CC3501E_ROLE_WIFI_STA;
	g_ota_pending                      = ALP_CC3501E_OTA_PENDING_STAGED;
	g_status_io_down_remaining         = 0u;
	g_get_version_override_active      = false;
	g_get_version_override_value       = 0u;
	g_get_version_io_down_remaining    = 0u;
	g_spi1_reply_bad_seq               = false;
	g_diag_stats_v8                    = false;
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

/* A single, deliberately DIFFERENT-content Wi-Fi scan record ("Ctx2Net") used
 * by test_wifi_scan_buf_is_per_context_740 to stage a second context's own
 * scan reply -- so a byte-compare of ctx A's raw decode buffer after ctx B's
 * scan is a meaningful "did B's bytes leak into A" check, not a comparison
 * of two identical payloads that would pass even if they aliased. */
static uint16_t build_wifi_scan_ctx_b(uint8_t *p)
{
	uint16_t      o     = 0u;
	const uint8_t b0[6] = { 0x99, 0x88, 0x77, 0x66, 0x55, 0x44 };
	memcpy(&p[o], b0, 6);
	o += 6u;
	p[o++] = (uint8_t)(-60);      /* rssi */
	p[o++] = 1u;                  /* channel */
	p[o++] = 0x00u;               /* security_info LE lo */
	p[o++] = 0x00u;               /* security_info LE hi -> open */
	p[o++] = 7u;                  /* ssid_len */
	memcpy(&p[o], "Ctx2Net", 7u); /* ssid */
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

/* Single, deliberately DIFFERENT-content BLE record ("Ctx2Dev") -- BLE
 * counterpart of build_wifi_scan_ctx_b(), see its comment. */
static uint16_t build_ble_scan_ctx_b(uint8_t *p)
{
	uint16_t      o     = 0u;
	const uint8_t a0[6] = { 0x60, 0x50, 0x40, 0x30, 0x20, 0x10 };
	memcpy(&p[o], a0, 6);
	o += 6u;
	p[o++] = 1u; /* addr_type random */
	p[o++] = (uint8_t)(-33);
	p[o++] = 7u;
	memcpy(&p[o], "Ctx2Dev", 7u);
	o += 7u;
	return o;
}

/* Build the reply for the just-received request. */
static void slave_dispatch(void)
{
	switch (slave.cmd) {
	case ALP_CC3501E_CMD_PING:
	case ALP_CC3501E_CMD_RESET:
	case ALP_CC3501E_CMD_WIFI_DISCONNECT:
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
	/* OTA_PROMOTE (0x46) belongs in THIS bucket, not with the worker-routed
	 * submits below: handle_ota_promote() returns
	 * hw_to_resp(cc3501e_hw_ota_promote()), and the TI HAL's
	 * cc3501e_hw_ota_promote() arms the deferred swap-reboot and returns
	 * CC3501E_HW_OK unconditionally -- a bare RESP_OK is its ONLY success
	 * reply.  Modelled here so test_ota_promote_bare_ok_still_accepted_1385
	 * fences the #1385 check against being over-extended onto it. */
	case ALP_CC3501E_CMD_OTA_PROMOTE:
		/* Argless / write-only ops: success is the bare OK status. */
		stage_status(ALP_CC3501E_RESP_OK);
		break;

	case ALP_CC3501E_CMD_OTA_STATUS: {
		/* 16 bytes: state(1) | reserved(3) | bytes_written(LE32) |
		 * total_len(LE32) | pending(1) | reserved2(3).  Only `pending` matters
		 * to the promote path -- it is the flash-derived byte the commit is
		 * gated on, and the one an ack cannot forge. */
		uint8_t d[16] = { 0 };
		d[0]          = ALP_CC3501E_OTA_STATE_STAGED;
		d[12]         = g_ota_pending;
		stage_reply(ALP_CC3501E_RESP_OK, d, 16u);
		break;
	}

	case ALP_CC3501E_CMD_WIFI_CONNECT_STA:
		/* Fire-and-forget submit model (#1376/#1377/#1378): snapshot the
		 * submit's own request bytes separately (the generic req_pl/req_len
		 * above get overwritten by cc3501e_wifi_connect()'s follow-up
		 * WIFI_STATUS polls before the caller ever sees them), count the
		 * submit, and ack RESP_ERR_BUSY -- the firmware's real, unconditional
		 * WORKER_IDLE ack -- unless a test forces the #1378 dead-phase-alias
		 * scenario via g_connect_submit_force_ok. */
		slave.connect_last_req_len = slave.req_len;
		memcpy(slave.connect_last_req_pl, slave.req_pl, slave.req_len);
		slave.connect_submit_count++;
		stage_status(g_connect_submit_force_ok ? ALP_CC3501E_RESP_OK : ALP_CC3501E_RESP_ERR_BUSY);
		break;

	case ALP_CC3501E_CMD_WIFI_AP_START:
		/* #1385: AP_START runs through the SAME firmware handler as
		 * CONNECT_STA (handle_worker_routed_payload), so its submit ack is
		 * the same unconditional RESP_ERR_BUSY -- and the WORKER_DONE branch
		 * that would reply a bare RESP_OK is unreachable to the host, because
		 * worker_run_pending() calls worker_reset() for CONNECT_STA/AP_START
		 * before cc3501e_bridge_ready() re-arms the link.  The old model
		 * staged a bare RESP_OK here (the argless bucket above), which is a
		 * byte pattern the real firmware can never produce for this opcode --
		 * it modelled the dead-phase alias itself as success. */
		slave.ap_start_last_req_len = slave.req_len;
		memcpy(slave.ap_start_last_req_pl, slave.req_pl, slave.req_len);
		slave.ap_start_submit_count++;
		stage_status(g_connect_submit_force_ok ? ALP_CC3501E_RESP_OK : ALP_CC3501E_RESP_ERR_BUSY);
		break;

	case ALP_CC3501E_CMD_GET_VERSION: {
		uint16_t      ver  = g_get_version_override_active ? g_get_version_override_value
		                                                   : (uint16_t)ALP_CC3501E_PROTOCOL_VERSION;
		const uint8_t v[2] = { (uint8_t)(ver & 0xFFu), (uint8_t)((ver >> 8) & 0xFFu) };
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
		d[3]          = g_diag_role;
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
		/* v7 answers 8 bytes, v8 answers 16 -- ADDITIVELY, same first two
		 * counters.  Default to the v7 shape so every pre-existing test keeps
		 * exercising the old-firmware path; g_diag_stats_v8 opts in. */
		uint8_t s[16] = { 0x44, 0x33, 0x22, 0x11,   /* frames_ok        = 0x11223344 */
			              0x05, 0x00, 0x00, 0x00,   /* frames_err       = 0x00000005 */
			              0x07, 0x00, 0x00, 0x00,   /* worker_execs     = 0x00000007 */
			              0x03, 0x00, 0x00, 0x00 }; /* retry_latch_hits = 0x00000003 */
		stage_reply(ALP_CC3501E_RESP_OK, s, g_diag_stats_v8 ? 16u : 8u);
		break;
	}
	case ALP_CC3501E_CMD_WIFI_GET_RSSI: {
		/* Worker-routed (#1377): ack BUSY for rssi_busy_polls_remaining
		 * submits (simulating the firmware's WORKER_IDLE-then-collect shape)
		 * before finally handing back the value. */
		if (slave.rssi_busy_polls_remaining > 0u) {
			slave.rssi_busy_polls_remaining--;
			stage_status(ALP_CC3501E_RESP_ERR_BUSY);
			break;
		}
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
		if (slave.status_polls_before_terminal > 0u) {
			/* Still resolving: report CONNECTING (bridge busy on the real
			 * link) rather than the terminal latch below. */
			slave.status_polls_before_terminal--;
			st[0] = ALP_CC3501E_WIFI_CONNECTING;
			st[1] = ALP_CC3501E_WIFI_FAIL_NONE;
			st[2] = 0;
			st[3] = 0u;
		} else {
			st[0] = slave.wifi_conn_state;
			st[1] = slave.wifi_fail_reason;
			st[2] = (uint8_t)slave.wifi_conn_rssi;
			st[3] = 0u;
		}
		stage_reply(ALP_CC3501E_RESP_OK, st, 4u);
		break;
	}
	case ALP_CC3501E_CMD_WIFI_SCAN_START: {
		uint8_t  recs[ALP_CC3501E_MAX_PAYLOAD];
		uint16_t n = g_scan_stage_ctx_b ? build_wifi_scan_ctx_b(recs) : build_wifi_scan(recs);
		stage_reply(ALP_CC3501E_RESP_OK, recs, n);
		break;
	}
	case ALP_CC3501E_CMD_BLE_SCAN_START: {
		uint8_t  recs[ALP_CC3501E_MAX_PAYLOAD];
		uint16_t n = g_scan_stage_ctx_b ? build_ble_scan_ctx_b(recs) : build_ble_scan(recs);
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
	case ALP_CC3501E_CMD_SPI1_CONFIGURE: {
		/* reply DATA = alp_cc3501e_spi1_config_resp_t { freq_hz(LE32) |
		 * max_xfer(LE16) | bits_per_word | reserved }.  Reports the request
		 * back as the "actual" rate (no divider to quantise here) and this
		 * family's real chunk cap, exactly what CONFIGURE hands a real host. */
		const uint32_t freq_hz = (uint32_t)slave.req_pl[0] | ((uint32_t)slave.req_pl[1] << 8) |
		                         ((uint32_t)slave.req_pl[2] << 16) |
		                         ((uint32_t)slave.req_pl[3] << 24);
		const uint8_t  d[8]    = {
			(uint8_t)(freq_hz & 0xFFu),
			(uint8_t)((freq_hz >> 8) & 0xFFu),
			(uint8_t)((freq_hz >> 16) & 0xFFu),
			(uint8_t)((freq_hz >> 24) & 0xFFu),
			(uint8_t)(ALP_CC3501E_SPI1_MAX_XFER & 0xFFu),
			(uint8_t)((ALP_CC3501E_SPI1_MAX_XFER >> 8) & 0xFFu),
			slave.req_pl[5], /* bits_per_word echoed back */
			0u,
		};
		stage_reply(ALP_CC3501E_RESP_OK, d, 8u);
		break;
	}
	case ALP_CC3501E_CMD_SPI1_TRANSFER: {
		/* Software model of the real stub HAL (hal/cc3501e_hw_stub.c in the
		 * firmware repo): a wire loop, MOSI tied straight to MISO, so a test
		 * that clocks bytes out gets those same bytes back.  Self-delimiting
		 * on the request's own len/flags/seq, same as the real firmware. */
		const uint16_t len     = (uint16_t)slave.req_pl[0] | ((uint16_t)slave.req_pl[1] << 8);
		const uint8_t  flags   = slave.req_pl[2];
		const uint8_t  seq     = slave.req_pl[3];
		const uint8_t  tx_fill = slave.req_pl[4];
		const bool     no_rx   = (flags & ALP_CC3501E_SPI1_XFER_NO_RX) != 0u;
		const bool     no_tx   = (flags & ALP_CC3501E_SPI1_XFER_NO_TX) != 0u;
		uint8_t        d[4u + ALP_CC3501E_SPI1_MAX_XFER];

		d[0] = no_rx ? 0u : (uint8_t)(len & 0xFFu);
		d[1] = no_rx ? 0u : (uint8_t)((len >> 8) & 0xFFu);
		d[2] = (uint8_t)(flags & ALP_CC3501E_SPI1_XFER_CS_HOLD); /* echo of requested CS_HOLD */
		d[3] = g_spi1_reply_bad_seq ? (uint8_t)(seq + 1u) : seq; /* #g_spi1_reply_bad_seq mutant */
		if (!no_rx) {
			if (!no_tx) {
				memcpy(&d[4], &slave.req_pl[8], len);
			} else {
				memset(&d[4], tx_fill, len);
			}
		}
		stage_reply(ALP_CC3501E_RESP_OK, d, (uint16_t)(4u + (no_rx ? 0u : len)));
		break;
	}
	case ALP_CC3501E_CMD_SPI1_RELEASE:
		/* Argless escape hatch: bare OK, same as the real firmware. */
		stage_status(ALP_CC3501E_RESP_OK);
		break;
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
	if (slave.phase == PH_REQ_HDR && tx[0] == ALP_CC3501E_CMD_WIFI_STATUS) {
		slave.wifi_status_attempt_count++;
	}
	if (slave.phase == PH_REQ_HDR && tx[0] == ALP_CC3501E_CMD_WIFI_STATUS &&
	    g_status_io_down_remaining > 0u) {
		g_status_io_down_remaining--;
		return ALP_ERR_IO;
	}
	/* #1371: fail a GET_VERSION transaction outright -- models the CC3501E's
	 * documented Puya cold-boot flash bug (chips/cc3501e/cc3501e_core.c's
	 * cc3501e_hard_reset comment), where the slave has not armed its SPI yet
	 * and the request never lands. */
	if (slave.phase == PH_REQ_HDR && tx[0] == ALP_CC3501E_CMD_GET_VERSION &&
	    g_get_version_io_down_remaining > 0u) {
		g_get_version_io_down_remaining--;
		return ALP_ERR_IO;
	}
	switch (slave.phase) {
	case PH_REQ_HDR:
		slave.cmd = tx[0];
		if (slave.cmd_log_count < sizeof(slave.cmd_log)) {
			slave.cmd_log[slave.cmd_log_count] = slave.cmd;
		}
		slave.cmd_log_count++;
		if (slave.flags_log_count < sizeof(slave.flags_log)) {
			slave.flags_log[slave.flags_log_count] = tx[1];
		}
		slave.flags_log_count++;
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

/* ---- #1371: cc3501e_reset()'s wire-protocol compatibility gate ------------- *
 *
 * DESIGN.md always claimed "host refuses a mismatch" for GET_VERSION; these
 * pin the gate that now makes that claim true, and its two required
 * non-effects: the #1116 concurrency suite drives cc3501e_get_version()
 * directly (never through cc3501e_reset()) against a modelled slave that
 * never claims ALP_CC3501E_PROTOCOL_VERSION, and the cold-boot liveness
 * soaks use cc3501e_get_version() as a bare round-trip probe -- neither may
 * regress from this gate living in cc3501e_reset() instead. */

/* Any non-NULL pointer -- alp_gpio_write() is stubbed ALP_ERR_NOSUPPORT and
 * its result is (void)-discarded by cc3501e_reset(), so these are never
 * dereferenced; they only need to be non-NULL to clear reset()'s "pins not
 * bound" gate. */
#define FAKE_RESET_PIN  ((alp_gpio_t *)&fw)
#define FAKE_ENABLE_PIN ((alp_gpio_t *)&slave)

ZTEST(cc3501e_host_driver, test_reset_accepts_matching_protocol_version_1371)
{
	fw.reset_pin  = FAKE_RESET_PIN;
	fw.enable_pin = FAKE_ENABLE_PIN;

	zassert_equal(cc3501e_reset(&fw), ALP_OK, "matching GET_VERSION -> reset succeeds");
	zassert_true(fw.initialised, "a matching version leaves the context usable");
}

ZTEST(cc3501e_host_driver, test_reset_refuses_protocol_version_mismatch_1371)
{
	fw.reset_pin  = FAKE_RESET_PIN;
	fw.enable_pin = FAKE_ENABLE_PIN;

	g_get_version_override_active = true;
	g_get_version_override_value  = (uint16_t)ALP_CC3501E_PROTOCOL_VERSION + 1u;

	zassert_equal(cc3501e_reset(&fw),
	              ALP_ERR_VERSION,
	              "GET_VERSION answered with a different value -> ALP_ERR_VERSION");
	zassert_false(fw.initialised, "a refused context is left uninitialised");

	/* The dead end this leaves behind, deliberately: once refused, EVERY
	 * later call (including re-reading the version for a diagnostic) fails
	 * ALP_ERR_NOT_READY rather than reporting a value nobody re-measured. */
	uint16_t v = 0xDEADu;
	zassert_equal(cc3501e_get_version(&fw, &v),
	              ALP_ERR_NOT_READY,
	              "cc3501e_get_version() does not keep working across a refusal");
}

ZTEST(cc3501e_host_driver, test_reset_tolerates_transport_failure_during_probe_1371)
{
	fw.reset_pin  = FAKE_RESET_PIN;
	fw.enable_pin = FAKE_ENABLE_PIN;

	/* Models the CC3501E's documented Puya cold-boot flash bug: the FIRST
	 * boot's GET_VERSION never lands at all (transport failure), which is
	 * NOT a version verdict -- only an answered request can be compared, so
	 * this must stay non-fatal and leave the context usable for a caller's
	 * own hard-reset retry (examples/peripheral-io/alp-console's
	 * cc3501e_bridge_bringup 8-iteration soak; aen-cc3501e-gpio's liveness
	 * gate) -- exactly like it was before this context ever probed. */
	g_get_version_io_down_remaining = 1u;

	zassert_equal(cc3501e_reset(&fw),
	              ALP_OK,
	              "an unanswered GET_VERSION probe must not be treated as a refusal");
	zassert_true(fw.initialised, "an unanswered probe leaves the context usable for a retry");

	/* And the retry lands normally afterwards (the down-counter above is
	 * exhausted; the fixture reverts to its default matching reply). */
	uint16_t v = 0u;
	zassert_equal(cc3501e_get_version(&fw, &v), ALP_OK, "a following GET_VERSION works");
	zassert_equal(v, (uint16_t)ALP_CC3501E_PROTOCOL_VERSION, "and reads the real value");
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
	cc3501e_diag_stats_t st;
	memset(&st, 0xA5, sizeof(st));
	zassert_equal(cc3501e_diag_stats(&fw, &st), ALP_OK, "DIAG_GET_STATS -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_DIAG_GET_STATS, "opcode 0x70");
	zassert_equal(st.frames_ok, 0x11223344u, "frames_ok LE32");
	zassert_equal(st.frames_err, 0x00000005u, "frames_err LE32");
}

/* A v7 firmware answers only the first two counters.  The two v8 counters must
 * then report as ABSENT, not as a measured zero -- a bench run that reads
 * "retry_latch_hits = 0" off firmware that never counted them would record a
 * pass for a mechanism that was not running. */
ZTEST(cc3501e_host_driver, test_diag_stats_short_v7_reply_reports_counters_absent)
{
	cc3501e_diag_stats_t st;
	memset(&st, 0xA5, sizeof(st));
	zassert_equal(cc3501e_diag_stats(&fw, &st), ALP_OK, "8-byte reply is not a fault");
	zassert_false(st.has_worker_counters, "v7 firmware does not report the worker counters");
	zassert_equal(st.worker_execs, 0u, "absent counters read zero, flagged by has_*");
	zassert_equal(st.retry_latch_hits, 0u, "absent counters read zero, flagged by has_*");
	zassert_equal(st.frames_ok, 0x11223344u, "the two v7 counters still decode");
}

ZTEST(cc3501e_host_driver, test_diag_stats_v8_reply_decodes_worker_counters)
{
	g_diag_stats_v8 = true;
	cc3501e_diag_stats_t st;
	memset(&st, 0xA5, sizeof(st));
	zassert_equal(cc3501e_diag_stats(&fw, &st), ALP_OK, "16-byte reply -> OK");
	zassert_true(st.has_worker_counters, "v8 firmware reports the worker counters");
	zassert_equal(st.frames_ok, 0x11223344u, "frames_ok LE32");
	zassert_equal(st.frames_err, 0x00000005u, "frames_err LE32");
	zassert_equal(st.worker_execs, 0x00000007u, "worker_execs LE32 at offset 8");
	zassert_equal(st.retry_latch_hits, 0x00000003u, "retry_latch_hits LE32 at offset 12");
}

/* ---- proto v8 request identity (cc3501e-bridge-firmware#102) --------------
 *
 * The firmware can only absorb a retry if the retry is RECOGNISABLE, which
 * means the seq in flags bits 3..7 is identical across every attempt of one
 * logical command.  These assert the wire property directly off the captured
 * header bytes, because that is the contract the firmware reads -- not the
 * host-side counter that produced it. */

static uint8_t seq_of(uint8_t flags)
{
	return (uint8_t)((flags >> ALP_CC3501E_FLAG_REQ_SEQ_SHIFT) & ALP_CC3501E_REQ_SEQ_MASK);
}

ZTEST(cc3501e_host_driver, test_retry_seq_is_constant_across_one_commands_retries)
{
	slave.rssi_busy_polls_remaining = 3u; /* 3 BUSY acks, then the value */
	int8_t rssi                     = 0;
	zassert_equal(cc3501e_wifi_rssi(&fw, &rssi), ALP_OK, "GET_RSSI -> OK after riding out BUSY");
	zassert_true(slave.flags_log_count >= 4u, "3 BUSY attempts + the collect were clocked");

	const uint8_t seq = seq_of(slave.flags_log[0]);
	zassert_not_equal(seq,
	                  ALP_CC3501E_REQ_SEQ_NONE,
	                  "a retryable command must carry a real seq, not the reserved 0");
	for (uint32_t i = 1u; i < slave.flags_log_count && i < ARRAY_SIZE(slave.flags_log); i++) {
		zassert_equal(seq_of(slave.flags_log[i]),
		              seq,
		              "every retry of ONE logical command re-sends the SAME seq");
	}
}

ZTEST(cc3501e_host_driver, test_each_logical_command_gets_a_different_seq)
{
	int8_t rssi = 0;
	zassert_equal(cc3501e_wifi_rssi(&fw, &rssi), ALP_OK, "first command");
	const uint8_t  first       = seq_of(slave.flags_log[0]);
	const uint32_t after_first = slave.flags_log_count;
	zassert_equal(cc3501e_wifi_rssi(&fw, &rssi), ALP_OK, "second command");
	zassert_true(slave.flags_log_count > after_first, "the second command clocked a header");
	zassert_not_equal(seq_of(slave.flags_log[after_first]),
	                  first,
	                  "a NEW logical command must not reuse the previous command's seq, or the "
	                  "firmware would serve it the previous command's cached reply");
}

/* The single-shot path has no retry loop, so nothing it sends is ever a repeat
 * of anything -- it must therefore be un-latchable.  Seq 0 says exactly that. */
ZTEST(cc3501e_host_driver, test_single_shot_request_sends_the_reserved_seq_none)
{
	cc3501e_diag_stats_t st;
	zassert_equal(cc3501e_diag_stats(&fw, &st), ALP_OK, "DIAG_GET_STATS -> OK");
	zassert_true(slave.flags_log_count >= 1u, "a header was clocked");
	zassert_equal(seq_of(slave.flags_log[0]),
	              ALP_CC3501E_REQ_SEQ_NONE,
	              "cc3501e_request() is single-shot -- it must not claim an identity");
	zassert_true((slave.flags_log[0] & ALP_CC3501E_FLAG_RESP_REQUIRED) != 0u,
	             "the v1 flag bits are untouched by the seq");
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

/* #1377: WIFI_GET_RSSI is worker-routed on the firmware side -- a fresh
 * submit acks RESP_ERR_BUSY and only later returns the value once the drain
 * has collected it.  Against the pre-fix single cc3501e_request() this test
 * fails outright: that call collects only the first BUSY ack and returns
 * ALP_ERR_BUSY, leaving the job orphaned.  Poll-by-repeat must ride the busy
 * window out and still land on the real value. */
ZTEST(cc3501e_host_driver, test_wifi_rssi_retries_worker_busy_1377)
{
	slave.rssi_busy_polls_remaining = 3u;
	int8_t rssi                     = 0;
	zassert_equal(cc3501e_wifi_rssi(&fw, &rssi), ALP_OK, "GET_RSSI -> OK after riding out BUSY");
	zassert_equal(rssi, -42, "signed dBm decoded once the drain collected it");
	zassert_equal(slave.rssi_busy_polls_remaining, 0u, "all staged busy polls were consumed");
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

/* #1377: `alp companion wifi status` returned -5 (ALP_ERR_IO) repeatedly right
 * after a healthy ver/scan/connect sequence -- the shared bridge transport is
 * briefly down whenever a radio op is in flight, and a status read landing in
 * that window desynced like any other transaction.  Against the pre-fix
 * single cc3501e_request() this test fails outright: the first (injected)
 * transport fault returns ALP_ERR_IO immediately, with no retry.  Riding the
 * down-window out via poll-by-repeat must still land on the real state. */
ZTEST(cc3501e_host_driver, test_wifi_status_retries_transient_io_1377)
{
	g_status_io_down_remaining = 5u;
	alp_cc3501e_wifi_status_t st;
	memset(&st, 0xA5, sizeof(st));
	zassert_equal(
	    cc3501e_wifi_status(&fw, &st), ALP_OK, "WIFI_STATUS -> OK after riding out the IO window");
	zassert_equal(st.state, ALP_CC3501E_WIFI_CONNECTED, "state decoded once the link recovered");
	zassert_equal(g_status_io_down_remaining, 0u, "all staged transport faults were consumed");
}

/* Same fault, but never recovers within a short caller budget: this proves
 * the retry is BOUNDED (not an infinite spin) and the honest answer -- an
 * unrecoverable transport -- surfaces as ALP_ERR_TIMEOUT, not the raw
 * ALP_ERR_IO of a single attempt, once the down-window elapses. */
ZTEST(cc3501e_host_driver, test_wifi_status_gives_up_after_the_down_window_1377)
{
	g_status_io_down_remaining = UINT32_MAX;
	alp_cc3501e_wifi_status_t st;
	alp_status_t              s = cc3501e_wifi_status(&fw, &st);
	zassert_equal(s, ALP_ERR_TIMEOUT, "permanently-down transport -> bounded TIMEOUT, not a hang");
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

/* #740: cc3501e_wifi_scan's decode buffer moved from a function-local `static`
 * (shared by every cc3501e_t, process-wide) into per-context storage
 * (ctx->wifi_scan_buf). A second, independent context must not be able to
 * disturb a first context's already-decoded scratch.
 *
 * Regression-proof (not vacuous -- see the fix-round note): the FIRST
 * zassert_mem_equal below compares ctx A's ctx->wifi_scan_buf against the
 * exact raw wire bytes the mock slave staged, independently reconstructed
 * via build_wifi_scan() -- it is not a self-referential snapshot-vs-itself
 * check. That assertion FAILS against the pre-#740 cc3501e_wifi_scan (a
 * function-local `static scan_buf`, reverted from origin/dev): that form
 * never writes ctx->wifi_scan_buf at all, so the field stays all-zero and
 * the byte-compare against the real staged reply mismatches immediately.
 * ctx B then runs its OWN real cc3501e_wifi_scan() call (through the driver,
 * not a direct memset) with a DIFFERENT staged reply (build_wifi_scan_ctx_b,
 * a distinct BSSID/SSID/channel), and ctx A's buffer must still read back
 * unchanged afterward -- the non-aliasing half of #740, made meaningful by
 * using different content for A and B instead of two identical payloads. */
ZTEST(cc3501e_host_driver, test_wifi_scan_buf_is_per_context_740)
{
	cc3501e_scan_record_t recs[8];
	size_t                n = 0u;
	zassert_equal(cc3501e_wifi_scan(&fw, recs, 8u, &n, 100u), ALP_OK, "SCAN ctx A -> OK");
	zassert_equal(n, 2u, "two records parsed on ctx A");

	uint8_t  wire_a[ALP_CC3501E_MAX_PAYLOAD];
	uint16_t wire_a_len = build_wifi_scan(wire_a);
	zassert_mem_equal(fw.wifi_scan_buf,
	                  wire_a,
	                  wire_a_len,
	                  "ctx A's cc3501e_wifi_scan must decode into ctx->wifi_scan_buf itself "
	                  "(#740) -- fails against the pre-fix function-local `static` buffer, "
	                  "which never touches this field");

	uint8_t snapshot[ALP_CC3501E_MAX_PAYLOAD];
	memcpy(snapshot, fw.wifi_scan_buf, sizeof(snapshot));

	/* Independent second context runs its OWN real scan, through the same
	 * driver entry point, staged with genuinely different content. */
	cc3501e_t ctx_b;
	zassert_equal(cc3501e_init(&ctx_b, fake_bus), ALP_OK, "init ctx B");
	slave_reset();
	g_scan_stage_ctx_b = true;
	zassert_equal(cc3501e_wifi_scan(&ctx_b, recs, 8u, &n, 100u), ALP_OK, "SCAN ctx B -> OK");
	zassert_equal(n, 1u, "ctx B's distinct single-record reply parsed");
	zassert_str_equal(recs[0].ssid, "Ctx2Net", "ctx B decoded ITS OWN staged SSID");

	zassert_mem_equal(fw.wifi_scan_buf,
	                  snapshot,
	                  sizeof(snapshot),
	                  "ctx A's scan buffer must be unaffected by ctx B's OWN real scan (#740)");
}

/* #740: same-context reentrancy is now an explicit ALP_ERR_BUSY instead of
 * silently racing the shared decode buffer. */
ZTEST(cc3501e_host_driver, test_wifi_scan_busy_rejects_reentrant_same_ctx_740)
{
	cc3501e_scan_record_t recs[8];
	size_t                n = 123u;

	fw.wifi_scan_busy = true; /* simulate an in-flight scan on this ctx */
	zassert_equal(cc3501e_wifi_scan(&fw, recs, 8u, &n, 100u), ALP_ERR_BUSY, "reentrant -> BUSY");
	zassert_equal(slave.cmd, 0u, "no transfer clocked while busy");
	zassert_equal(n, 0u, "count still reset to 0 before the busy check");

	fw.wifi_scan_busy = false;
	zassert_equal(cc3501e_wifi_scan(&fw, recs, 8u, &n, 100u), ALP_OK, "cleared -> scan proceeds");
	zassert_equal(n, 2u, "normal scan after the busy flag clears");
}

ZTEST(cc3501e_host_driver, test_wifi_scan_null_ctx_not_ready)
{
	cc3501e_scan_record_t recs[8];
	zassert_equal(
	    cc3501e_wifi_scan(NULL, recs, 8u, NULL, 100u), ALP_ERR_NOT_READY, "NULL ctx -> NOT_READY");
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
 * the inline SSID then the inline passphrase, with no padding.  The wire
 * bytes are asserted off the DEDICATED connect_last_req_* snapshot, not the
 * generic slave.req_pl/req_len -- by the time cc3501e_wifi_connect() returns,
 * those reflect its own follow-up WIFI_STATUS poll (empty request), not the
 * CONNECT_STA submit (see the mock's WIFI_CONNECT_STA case). */
ZTEST(cc3501e_host_driver, test_wifi_connect_encodes_header_ssid_psk)
{
	zassert_equal(
	    cc3501e_wifi_connect(&fw, "mynet", 1u, "secretpw", 100u), ALP_OK, "CONNECT -> OK");
	zassert_equal(slave.connect_submit_count, 1u, "exactly one submit");
	/* header(4) + ssid(5) + psk(8) = 17. */
	zassert_equal(slave.connect_last_req_len, 4u + 5u + 8u, "submit payload = header + ssid + psk");
	zassert_equal(slave.connect_last_req_pl[0], 5u, "ssid_len");
	zassert_equal(slave.connect_last_req_pl[1], 8u, "psk_len");
	zassert_equal(slave.connect_last_req_pl[2], 1u, "security");
	zassert_mem_equal(&slave.connect_last_req_pl[4], "mynet", 5u, "inline SSID");
	zassert_mem_equal(&slave.connect_last_req_pl[9], "secretpw", 8u, "inline passphrase");
}

ZTEST(cc3501e_host_driver, test_wifi_connect_oversize_ssid_rejected)
{
	static const char big[40] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"; /* 35 > 32 */
	zassert_equal(cc3501e_wifi_connect(&fw, big, 1u, "pw", 100u),
	              ALP_ERR_INVAL,
	              "SSID > 32 -> INVAL (host guard)");
	zassert_equal(slave.cmd, 0u, "no transfer clocked");
}

/* #1376 regression proof: an association that takes several WIFI_STATUS polls
 * to resolve must still have submitted CONNECT_STA exactly ONCE.  Against the
 * pre-fix poll_by_repeat(WIFI_CONNECT_STA, ...) contract this fails outright
 * -- every 50 ms repeat that landed on the mock's stateless "always ack OK"
 * WIFI_CONNECT_STA case (the model faithful to the OLD contract) would have
 * been read as immediate success; against the OLD real firmware contract
 * (fire-and-forget + a job slot reset to IDLE the instant it drains) each
 * retry submits a brand-new association -- five retries, five real joins,
 * for one user command, per issue #1376. */
ZTEST(cc3501e_host_driver, test_wifi_connect_submits_exactly_once_1376)
{
	slave.status_polls_before_terminal = 4u; /* CONNECTING for 4 polls, then terminal */
	zassert_equal(cc3501e_wifi_connect(&fw, "slownet", 1u, "pw", 5000u),
	              ALP_OK,
	              "eventually CONNECTED -> OK");
	zassert_equal(slave.connect_submit_count,
	              1u,
	              "exactly one CONNECT_STA submit regardless of how many status polls it took");
	/* Reviewer finding: the success path was asserted only via ALP_OK + the
	 * submit count, never off slave.cmd -- a mutant that tore the
	 * association down on the SUCCESS path too (e.g. an unconditional
	 * post-submit disconnect) would still pass both of the above. The last
	 * thing a healthy connect touches the wire with is a WIFI_STATUS poll
	 * reading CONNECTED, never WIFI_DISCONNECT; fence that here. */
	zassert_equal(slave.cmd,
	              ALP_CC3501E_CMD_WIFI_STATUS,
	              "success must end on the WIFI_STATUS read, not a stray WIFI_DISCONNECT");
}

/* #1376: a connection to an SSID that never actually associates must not be
 * reported as connected.  The mock never advances past DISCONNECTED here
 * (the fixture default after slave_reset() is CONNECTED, so override it) --
 * cc3501e_wifi_connect() must time out, not report ALP_OK, and must still
 * have submitted only once. */
ZTEST(cc3501e_host_driver, test_wifi_connect_never_confirmed_times_out_1376)
{
	slave.wifi_conn_state = ALP_CC3501E_WIFI_DISCONNECTED;
	zassert_equal(cc3501e_wifi_connect(&fw, "ghostnet", 1u, "pw", 120u),
	              ALP_ERR_TIMEOUT,
	              "never confirmed -> TIMEOUT, not a false OK");
	zassert_equal(slave.connect_submit_count, 1u, "still exactly one submit, not a retry storm");
}

/* #1382 timeout-accounting regression: cc3501e_wifi_connect()'s poll loop
 * must OWN the retry budget, not delegate it to an inner call that retries
 * on its own.  Before the fix, the loop called the public
 * cc3501e_wifi_status(), whose own poll_by_repeat rides out an IO fault for
 * up to CC3501E_WIFI_DOWN_WINDOW_MS (10 s) per call, while the outer loop's
 * `remaining -= gap` only ever debited its own 50 ms sleep -- so a
 * permanently wedged transport meant every outer iteration hid up to 10 s
 * the caller's declared timeout_ms never accounted for. Measured against
 * this exact harness with a wedged transport: connect(timeout_ms=200) made
 * 1005 WIFI_STATUS attempts (50250 ms simulated), 251x the declared budget.
 *
 * Wedge the transport permanently (g_status_io_down_remaining = UINT32_MAX,
 * as test_wifi_status_gives_up_after_the_down_window_1377 does for a direct
 * cc3501e_wifi_status() call) and assert the number of WIFI_STATUS attempts
 * cc3501e_wifi_connect() makes stays in the ballpark ITS OWN cadence
 * predicts -- ceil(timeout_ms / CC3501E_REQ_TMO_MS) + 1 -- not the
 * 1005-attempt blowup the un-fixed nesting produced for the same budget. */
ZTEST(cc3501e_host_driver, test_wifi_connect_bounds_status_attempts_on_wedged_transport_1382)
{
	g_status_io_down_remaining = UINT32_MAX;
	alp_status_t s             = cc3501e_wifi_connect(&fw, "wedgednet", 1u, "pw", 200u);
	zassert_equal(s, ALP_ERR_TIMEOUT, "permanently wedged transport -> bounded TIMEOUT");
	zassert_true(slave.wifi_status_attempt_count <= 4u,
	             "WIFI_STATUS attempts must stay bounded by connect()'s own 200 ms budget, not "
	             "an inner down-window retry loop it doesn't account for (got %u attempts)",
	             slave.wifi_status_attempt_count);
}

/* #1481 regression: a HEALTHY poll (every WIFI_STATUS read returns ALP_OK,
 * simply reporting CONNECTING) must debit only the CC3501E_WIFI_STATUS_POLL_GAP_MS
 * (50 ms) it actually slept, not the CC3501E_REQ_TMO_MS (100 ms) worst-case
 * attempt cost the ss != ALP_OK path reserves for a failed read that never
 * happened here.  Before the fix, that 100 ms was debited on EVERY iteration
 * regardless of ss, so a healthy 3-iterations-of-CONNECTING poll burned
 * 3 * 150 ms = 450 ms of a caller's declared budget for only 3 * 50 ms =
 * 150 ms of real elapsed time (alp_delay_ms is a no-op stub here, but the
 * `remaining` accounting is exactly what a real caller's wall clock would
 * see) -- collapsing timeout_ms to roughly 1/3 of what was asked for.
 *
 * slave.status_polls_before_terminal = 4u yields four CONNECTING reads
 * total: one consumed by cc3501e_wifi_connect()'s own entry stale-
 * association check (before the loop's `remaining` budget is even
 * initialised, same fixture-order accounting
 * test_wifi_connect_submits_exactly_once_1376 relies on), then three more
 * inside the poll loop, before the fifth WIFI_STATUS read reports the
 * fixture default CONNECTED and the call returns ALP_OK.  A timeout_ms of
 * 320 ms comfortably covers the honest 3 * 50 ms = 150 ms the fixed loop
 * actually spends, but is well under the 3 * 150 ms = 450 ms the pre-fix
 * unconditional debit would have needed -- so this proves ALP_OK on the
 * fix and would have proven a premature ALP_ERR_TIMEOUT on the bug. */
ZTEST(cc3501e_host_driver, test_wifi_connect_healthy_poll_not_over_debited_1481)
{
	slave.status_polls_before_terminal = 4u; /* CONNECTING x3 in-loop, then CONNECTED */
	alp_status_t s                     = cc3501e_wifi_connect(&fw, "healthynet", 1u, "pw", 320u);
	zassert_equal(s,
	              ALP_OK,
	              "a healthy CONNECTING poll must consume ~wall-clock time (150 ms), not "
	              "~3x it (450 ms) against a 320 ms budget (got status %d)",
	              s);
}

/* #1376/#1378: an association that genuinely FAILS (auth reject / no AP) must
 * be reported as a failure, not an OK -- and, as above, from exactly one
 * submit. */
ZTEST(cc3501e_host_driver, test_wifi_connect_reports_failure_not_ok_1376)
{
	slave.wifi_conn_state  = ALP_CC3501E_WIFI_CONN_FAILED;
	slave.wifi_fail_reason = ALP_CC3501E_WIFI_FAIL_REJECTED;
	zassert_equal(cc3501e_wifi_connect(&fw, "securenet", 1u, "wrongpw", 5000u),
	              ALP_ERR_IO,
	              "CONN_FAILED/REJECTED -> IO, not OK");
	zassert_equal(slave.connect_submit_count, 1u, "exactly one submit");
}

/* #1435 helper: first index in slave.cmd_log at which @p cmd appears, or
 * slave.cmd_log_count (never a valid index) if it never did. slave.cmd alone
 * only ever holds the LAST opcode dispatched -- not enough to prove ORDER
 * (WIFI_DISCONNECT strictly before WIFI_CONNECT_STA), which is the actual
 * property under test below. */
static uint32_t cmd_log_index_of(uint8_t cmd)
{
	uint32_t n =
	    (slave.cmd_log_count < sizeof(slave.cmd_log)) ? slave.cmd_log_count : sizeof(slave.cmd_log);
	for (uint32_t i = 0; i < n; i++) {
		if (slave.cmd_log[i] == cmd) {
			return i;
		}
	}
	return slave.cmd_log_count;
}

/* #1435 bench-proven stale-association wedge: a connect that FOLLOWS a
 * failed attempt (the WIFI_STATUS latch already reads CONN_FAILED when this
 * one is entered) must clear it -- issue WIFI_DISCONNECT (0x13) -- BEFORE
 * submitting WIFI_CONNECT_STA (0x12), else the new association's own
 * Wlan_Connect kick fails against the leftover NWP state (ALP_CC3501E_WIFI_
 * FAIL_KICK) even for a correct SSID/passphrase -- reproduced 2/2 on real
 * silicon (E1M-AEN801 r1). Checking ORDER (not just "a disconnect happened
 * somewhere") is the point: the previous round of this fix cleared on the
 * way OUT of a failed connect instead, which a same-opcode-count assertion
 * could not have told apart from clearing on the way IN.
 *
 * The connect must also PROCEED normally afterwards, not short-circuit at
 * the clear: connect_submit_count == 1 proves the submit still happened, and
 * the terminal read afterwards (the mock's latch is untouched by
 * WIFI_DISCONNECT, matching a fresh CONN_FAILED still being current) still
 * reports the CONNECT's own ALP_ERR_IO, not the clear's own ALP_OK. */
ZTEST(cc3501e_host_driver, test_wifi_connect_entry_clears_stale_failed_association_1435)
{
	slave.wifi_conn_state  = ALP_CC3501E_WIFI_CONN_FAILED;
	slave.wifi_fail_reason = ALP_CC3501E_WIFI_FAIL_REJECTED;
	alp_status_t s         = cc3501e_wifi_connect(&fw, "securenet", 1u, "wrongpw", 5000u);
	zassert_equal(
	    s, ALP_ERR_IO, "CONN_FAILED/REJECTED -> the CONNECT's own IO, not the clear's OK");
	zassert_equal(slave.connect_submit_count,
	              1u,
	              "the entry clean must not short-circuit -- CONNECT_STA still submits (#1435)");
	uint32_t disc_idx    = cmd_log_index_of(ALP_CC3501E_CMD_WIFI_DISCONNECT);
	uint32_t connect_idx = cmd_log_index_of(ALP_CC3501E_CMD_WIFI_CONNECT_STA);
	zassert_true(disc_idx < slave.cmd_log_count, "WIFI_DISCONNECT must be issued at all (#1435)");
	zassert_true(connect_idx < slave.cmd_log_count, "WIFI_CONNECT_STA must still be submitted");
	zassert_true(disc_idx < connect_idx,
	             "WIFI_DISCONNECT (idx %u) must land strictly BEFORE WIFI_CONNECT_STA (idx %u)",
	             disc_idx,
	             connect_idx);
}

/* Negative case: a latch that is NOT CONN_FAILED at entry must never see a
 * WIFI_DISCONNECT -- the entry clean is conditional on the failed latch, not
 * unconditional. DISCONNECTED is the "nothing to clear" baseline. */
ZTEST(cc3501e_host_driver, test_wifi_connect_entry_skips_clean_when_disconnected_1435)
{
	slave.wifi_conn_state = ALP_CC3501E_WIFI_DISCONNECTED;
	alp_status_t s        = cc3501e_wifi_connect(&fw, "ghostnet", 1u, "pw", 120u);
	zassert_equal(s, ALP_ERR_TIMEOUT, "never confirmed -> TIMEOUT via poll-loop exhaustion");
	zassert_equal(cmd_log_index_of(ALP_CC3501E_CMD_WIFI_DISCONNECT),
	              slave.cmd_log_count,
	              "DISCONNECTED at entry -> no WIFI_DISCONNECT issued (#1435)");
}

/* CONNECTING at entry is a LIVE attempt, not a stale one -- must be left
 * alone. Today's behaviour (documented on cc3501e_wifi_connect(), not
 * exercised further here) is that the new submit bounces BUSY and the poll
 * loop below keeps tracking the OLD attempt; this test only proves the entry
 * clean itself does not fire on it. */
ZTEST(cc3501e_host_driver, test_wifi_connect_entry_skips_clean_when_connecting_1435)
{
	slave.wifi_conn_state = ALP_CC3501E_WIFI_CONNECTING;
	alp_status_t s        = cc3501e_wifi_connect(&fw, "ghostnet", 1u, "pw", 120u);
	zassert_equal(s, ALP_ERR_TIMEOUT, "still CONNECTING at the deadline -> TIMEOUT");
	zassert_equal(cmd_log_index_of(ALP_CC3501E_CMD_WIFI_DISCONNECT),
	              slave.cmd_log_count,
	              "CONNECTING at entry -> a live attempt, no WIFI_DISCONNECT issued (#1435)");
}

/* CONNECTED at entry is connect-while-connected -- a pre-existing, separate,
 * unowned semantic this fix does not expand into. Must be left alone. */
ZTEST(cc3501e_host_driver, test_wifi_connect_entry_skips_clean_when_connected_1435)
{
	slave.wifi_conn_state = ALP_CC3501E_WIFI_CONNECTED;
	alp_status_t s        = cc3501e_wifi_connect(&fw, "mynet", 1u, "pw", 100u);
	zassert_equal(s, ALP_OK, "already CONNECTED -> OK (out of scope for #1435 to change)");
	zassert_equal(cmd_log_index_of(ALP_CC3501E_CMD_WIFI_DISCONNECT),
	              slave.cmd_log_count,
	              "CONNECTED at entry -> no WIFI_DISCONNECT issued (#1435)");
}

/* Regression the rework undoes: a failure discovered DURING the poll loop
 * (not already latched at entry, so the entry clean does not fire -- one
 * busy poll delays the terminal read past the entry check) must NOT issue
 * WIFI_DISCONNECT from either post-submit error exit any more. The previous
 * round of this fix teared down here; this proves that shape is gone. */
ZTEST(cc3501e_host_driver, test_wifi_connect_failure_exit_no_longer_tears_down_1435)
{
	slave.wifi_conn_state              = ALP_CC3501E_WIFI_CONN_FAILED;
	slave.wifi_fail_reason             = ALP_CC3501E_WIFI_FAIL_REJECTED;
	slave.status_polls_before_terminal = 1u; /* entry sees CONNECTING, not the terminal state */
	alp_status_t s = cc3501e_wifi_connect(&fw, "securenet", 1u, "wrongpw", 5000u);
	zassert_equal(s, ALP_ERR_IO, "CONN_FAILED/REJECTED discovered mid-poll -> IO");
	zassert_equal(cmd_log_index_of(ALP_CC3501E_CMD_WIFI_DISCONNECT),
	              slave.cmd_log_count,
	              "a failure exit must not itself issue WIFI_DISCONNECT any more (#1435 rework)");
}

/* #1378's own reproduction: force the mock's WIFI_CONNECT_STA submit ack to
 * read back a literal RESP_OK (0x00) -- "a valid header followed by an
 * all-zero payload phase", exactly what a dead bus phase clocks on real
 * silicon per this repo's own finding (hal/ti/cc3501e_hw_ti_wifi.c: "the
 * host then reads 0x00000000 from a dead link").  The association never
 * actually confirms (state stays DISCONNECTED).  cc3501e_wifi_connect() must
 * NOT report ALP_OK: it does not trust the submit's own ack in either
 * direction, only the independent WIFI_STATUS latch -- so this proves the
 * property the issue names: ALP_OK requires positive evidence the device
 * framed a reply, not merely the absence of evidence that it did not. */
ZTEST(cc3501e_host_driver, test_wifi_connect_ignores_dead_phase_ok_alias_1378)
{
	g_connect_submit_force_ok = true;
	slave.wifi_conn_state     = ALP_CC3501E_WIFI_DISCONNECTED;
	alp_status_t s            = cc3501e_wifi_connect(&fw, "ghostnet", 1u, "pw", 120u);
	zassert_not_equal(
	    s, ALP_OK, "a bare-OK submit ack alone must never make connect() report success");
}

/* #1378 at the transport layer directly: cc3501e_request_locked() itself
 * must refuse to hand back ALP_OK for a WIFI_CONNECT_STA submit whose reply
 * is a bare RESP_OK status byte (resp_payload_len == 1) -- the firmware's
 * WORKER_IDLE ack for this opcode is UNCONDITIONALLY RESP_ERR_BUSY, so a
 * synchronous OK is never a value this exchange can legitimately produce;
 * seeing one is self-evidently the dead-phase alias.  This is "the case
 * that matters": a valid reply HEADER (opcode echo + payload_len=1, both
 * staged normally) followed by an all-zero PAYLOAD phase (status byte 0x00)
 * must not yield ALP_OK. */
ZTEST(cc3501e_host_driver, test_connect_sta_dead_phase_alias_rejected_at_transport_1378)
{
	g_connect_submit_force_ok = true;
	uint8_t      req[4]       = { 5u, 0u, 1u, 0u }; /* minimal connect header, no SSID/PSK bytes */
	alp_status_t s            = cc3501e_request(
	    &fw, ALP_CC3501E_CMD_WIFI_CONNECT_STA, req, sizeof(req), NULL, 0, NULL, 100u);
	zassert_not_equal(s,
	                  ALP_OK,
	                  "a dead-phase 0x00 alias for CONNECT_STA's submit ack must not read as "
	                  "ALP_OK (#1378)");
	zassert_equal(s, ALP_ERR_IO, "rejected as a transport error, not silently accepted");
}

ZTEST(cc3501e_host_driver, test_wifi_ap_start_encodes_like_connect)
{
	/* The wire encoding is the assertion here; the RETURN is deliberately not
	 * ALP_OK.  AP_START's firmware handler acks every submit RESP_ERR_BUSY and
	 * the WORKER_DONE branch that would reply RESP_OK is wiped by
	 * worker_run_pending()'s worker_reset() before the host may clock again --
	 * so the opcode cannot synchronously succeed.  A retry loop around it is
	 * therefore provably unwinnable, so cc3501e_wifi_ap_start() submits exactly
	 * ONCE.  Since #1696 it then CONFIRMS that submit against GET_DIAG_INFO's
	 * radio role rather than reporting a blind timeout; g_diag_role is left at
	 * ROLE_WIFI_STA here, so the AP never comes up and the confirmation poll
	 * exhausts its budget -- which is what keeps ALP_ERR_TIMEOUT the expected
	 * outcome for THIS test.  test_wifi_ap_start_confirms_via_diag_role_1696
	 * covers the success direction. */
	zassert_equal(cc3501e_wifi_ap_start(&fw, "AP", 0u, "", 100u),
	              ALP_ERR_TIMEOUT,
	              "role never reaches ROLE_WIFI_AP -- the confirmation poll must exhaust "
	              "timeout_ms rather than inventing a success");
	/* NOT `slave.cmd`: the confirmation polls issue GET_DIAG_INFO after the
	 * submit, so the LAST opcode the mock saw is no longer AP_START.  The
	 * capture below is opcode-specific (the mock only fills
	 * ap_start_last_req_* from the AP_START arm), so it proves 0x14 went out
	 * without depending on it being the most recent frame. */
	zassert_true(slave.ap_start_last_req_len > 0u, "AP_START (0x14) reached the wire");
	zassert_equal(slave.ap_start_last_req_pl[0], 2u, "ssid_len");
	zassert_equal(slave.ap_start_last_req_pl[1], 0u, "psk_len (open)");
	zassert_mem_equal(&slave.ap_start_last_req_pl[4], "AP", 2u, "inline SSID");
	zassert_equal(slave.ap_start_submit_count,
	              1u,
	              "exactly one submit -- not the retry storm a poll-by-repeat wrapper would "
	              "cause, each re-issue of which would submit a BRAND NEW AP RoleUp");
}

/* #1696: the success direction.  Before this, cc3501e_wifi_ap_start() had no
 * reply it could frame as success and returned ALP_ERR_TIMEOUT even for an AP
 * that came up perfectly.  The firmware does publish the outcome -- ap_start
 * latches ROLE_WIFI_AP into the radio role, which GET_DIAG_INFO carries -- so
 * the host confirms against that.
 *
 * Drive the mock's role to AP and the same call must now report ALP_OK, while
 * STILL submitting exactly once (re-submitting would put a fresh Wlan_RoleUp on
 * live radio hardware -- the #1376 storm). */
ZTEST(cc3501e_host_driver, test_wifi_ap_start_confirms_via_diag_role_1696)
{
	g_diag_role = ALP_CC3501E_ROLE_WIFI_AP;

	zassert_equal(cc3501e_wifi_ap_start(&fw, "AP", 0u, "", 1000u),
	              ALP_OK,
	              "GET_DIAG_INFO reporting ROLE_WIFI_AP is what makes the submit confirmable");
	zassert_equal(slave.ap_start_submit_count,
	              1u,
	              "confirmation must poll a non-disturbing opcode, never re-submit AP_START");
}

/* #1385 at the transport layer, the direct analogue of
 * test_connect_sta_dead_phase_alias_rejected_at_transport_1378:
 * cc3501e_request_locked() must refuse to hand back ALP_OK for a
 * WIFI_AP_START submit whose reply is a bare RESP_OK status byte
 * (resp_payload_len == 1).  A valid reply HEADER (opcode echo +
 * payload_len=1) followed by an all-zero PAYLOAD phase is the dead-phase
 * alias this repo measured on silicon ("the host then reads 0x00000000 from a
 * dead link"), and RESP_OK is 0x00.  For this opcode a synchronous OK is not
 * a value the firmware can produce at all: handle_worker_routed_payload acks
 * WORKER_IDLE with RESP_ERR_BUSY, and worker_run_pending() resets the job
 * slot for CONNECT_STA/AP_START BEFORE cc3501e_bridge_ready() lets the host
 * clock again, so the WORKER_DONE -> RESP_OK branch can never be collected. */
ZTEST(cc3501e_host_driver, test_ap_start_dead_phase_alias_rejected_at_transport_1385)
{
	g_connect_submit_force_ok = true;
	uint8_t      req[4]       = { 2u, 0u, 0u, 0u }; /* minimal AP header, no SSID/PSK bytes */
	alp_status_t s =
	    cc3501e_request(&fw, ALP_CC3501E_CMD_WIFI_AP_START, req, sizeof(req), NULL, 0, NULL, 100u);
	zassert_not_equal(s,
	                  ALP_OK,
	                  "a dead-phase 0x00 alias for AP_START's submit ack must not read as "
	                  "ALP_OK (#1385)");
	zassert_equal(s, ALP_ERR_IO, "rejected as a transport error, not silently accepted");
}

/* #1385, the same property one level up: a dead payload phase on every
 * AP_START attempt must never surface from cc3501e_wifi_ap_start() as
 * success.  Before this fix the bare 0x00 was mapped straight through
 * resp_to_status() to ALP_OK and poll_by_repeat() returned it on the first
 * attempt -- a reported AP that never came up.
 *
 * Since #1385's submit-once restructure (77e258dc), cc3501e_wifi_ap_start()
 * squashes every outcome except ALP_ERR_INVAL/ALP_ERR_NOT_READY into
 * ALP_ERR_TIMEOUT unconditionally -- so `!= ALP_OK` on ITS return alone
 * cannot fail no matter what the dead-phase-alias check does; reverting the
 * WIFI_AP_START reject clause in cc3501e_request_locked()
 * (chips/cc3501e/cc3501e_core.c) still left this assertion passing.  Replay
 * the EXACT bytes cc3501e_wifi_ap_start() just staged on the wire (captured
 * by the mock in slave.ap_start_last_req_pl/len) straight through
 * cc3501e_request() -- the layer where the alias check actually runs -- so a
 * regression there fails this test. */
ZTEST(cc3501e_host_driver, test_wifi_ap_start_ignores_dead_phase_ok_alias_1385)
{
	g_connect_submit_force_ok = true;
	zassert_not_equal(cc3501e_wifi_ap_start(&fw, "ghostap", 1u, "pw", 100u),
	                  ALP_OK,
	                  "a bare-OK submit ack alone must never make ap_start() report success");
	alp_status_t raw = cc3501e_request(&fw,
	                                   ALP_CC3501E_CMD_WIFI_AP_START,
	                                   slave.ap_start_last_req_pl,
	                                   slave.ap_start_last_req_len,
	                                   NULL,
	                                   0,
	                                   NULL,
	                                   100u);
	zassert_equal(raw,
	              ALP_ERR_IO,
	              "the dead-phase 0x00 alias for AP_START's own wire payload must be rejected as a "
	              "transport error, not read back as ALP_OK");
}

/* #1385 fence in the OPPOSITE direction: OTA_PROMOTE (0x46) must stay OFF the
 * per-opcode dead-phase reject list.  A bare RESP_OK is that opcode's ONLY
 * success reply, so extending the alias check to it (as #1385's title invites)
 * would make cc3501e_ota_promote() always return ALP_ERR_IO and break firmware
 * promotion outright.  This test fails the moment someone adds
 * ALP_CC3501E_CMD_OTA_PROMOTE to that list.
 *
 * The bare ack is no longer trusted on its own, though -- since #1123 the
 * promote is gated on OTA_STATUS's flash-derived `pending` byte, which the mock
 * reports as STAGED here.  That is the guarantee the ack could never provide,
 * and it is why the alias check does not need to cover this opcode. */
ZTEST(cc3501e_host_driver, test_ota_promote_bare_ok_still_accepted_1385)
{
	zassert_equal(cc3501e_ota_promote(&fw, 100u),
	              ALP_OK,
	              "a STAGED pending image plus the bare RESP_OK is a legitimate promote");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_OTA_PROMOTE, "opcode 0x46 was the last frame");
}

/* #1123: the commit must be refused when the image store has nothing
 * installable.  Before this, promote was an unconditional OK that armed a
 * swap-reboot regardless -- so an aborted or abandoned session's promote
 * "succeeded" and rebooted the device for nothing. */
ZTEST(cc3501e_host_driver, test_ota_promote_refused_when_nothing_pending_1123)
{
	g_ota_pending = ALP_CC3501E_OTA_PENDING_NONE;
	zassert_equal(cc3501e_ota_promote(&fw, 100u),
	              ALP_ERR_NOT_READY,
	              "no installable image -> refuse to commit, do not reboot");
	zassert_not_equal(slave.cmd,
	                  ALP_CC3501E_CMD_OTA_PROMOTE,
	                  "the promote must never reach the wire when nothing is pending");
}

/* UNKNOWN is refused too: 'the store could not be queried' is not consent to
 * reboot.  Reading it as 'nothing pending' would be equally wrong in the other
 * direction -- it must not silently discard an image that may be installable. */
ZTEST(cc3501e_host_driver, test_ota_promote_refused_when_pending_unknown_1123)
{
	g_ota_pending = ALP_CC3501E_OTA_PENDING_UNKNOWN;
	zassert_equal(cc3501e_ota_promote(&fw, 100u),
	              ALP_ERR_NOT_READY,
	              "cannot-determine must refuse, not commit");
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

/* #740: same non-aliasing + explicit-BUSY guarantees as
 * test_wifi_scan_buf_is_per_context_740 / test_wifi_scan_busy_rejects_reentrant_same_ctx_740,
 * for the BLE scan decode buffer (ctx->ble_scan_buf). See
 * test_wifi_scan_buf_is_per_context_740's comment for why this form (raw
 * wire-byte compare + a genuinely distinct ctx B reply) actually
 * discriminates the pre-#740 function-local `static` buffer, unlike the
 * original snapshot-vs-itself version of this test. */
ZTEST(cc3501e_host_driver, test_ble_scan_buf_is_per_context_740)
{
	cc3501e_ble_scan_record_t recs[8];
	size_t                    n = 0u;
	zassert_equal(cc3501e_ble_scan(&fw, recs, 8u, &n, 100u), ALP_OK, "BLE_SCAN ctx A -> OK");
	zassert_equal(n, 2u, "two advertisers parsed on ctx A");

	uint8_t  wire_a[ALP_CC3501E_MAX_PAYLOAD];
	uint16_t wire_a_len = build_ble_scan(wire_a);
	zassert_mem_equal(fw.ble_scan_buf,
	                  wire_a,
	                  wire_a_len,
	                  "ctx A's cc3501e_ble_scan must decode into ctx->ble_scan_buf itself "
	                  "(#740) -- fails against the pre-fix function-local `static` buffer, "
	                  "which never touches this field");

	uint8_t snapshot[ALP_CC3501E_MAX_PAYLOAD];
	memcpy(snapshot, fw.ble_scan_buf, sizeof(snapshot));

	cc3501e_t ctx_b;
	zassert_equal(cc3501e_init(&ctx_b, fake_bus), ALP_OK, "init ctx B");
	slave_reset();
	g_scan_stage_ctx_b = true;
	zassert_equal(cc3501e_ble_scan(&ctx_b, recs, 8u, &n, 100u), ALP_OK, "BLE_SCAN ctx B -> OK");
	zassert_equal(n, 1u, "ctx B's distinct single-record reply parsed");
	zassert_str_equal(recs[0].name, "Ctx2Dev", "ctx B decoded ITS OWN staged name");

	zassert_mem_equal(fw.ble_scan_buf,
	                  snapshot,
	                  sizeof(snapshot),
	                  "ctx A's BLE scan buffer must be unaffected by ctx B's OWN real scan (#740)");
}

ZTEST(cc3501e_host_driver, test_ble_scan_busy_rejects_reentrant_same_ctx_740)
{
	cc3501e_ble_scan_record_t recs[8];
	size_t                    n = 123u;

	fw.ble_scan_busy = true;
	zassert_equal(cc3501e_ble_scan(&fw, recs, 8u, &n, 100u), ALP_ERR_BUSY, "reentrant -> BUSY");
	zassert_equal(slave.cmd, 0u, "no transfer clocked while busy");
	zassert_equal(n, 0u, "count still reset to 0 before the busy check");

	fw.ble_scan_busy = false;
	zassert_equal(cc3501e_ble_scan(&fw, recs, 8u, &n, 100u), ALP_OK, "cleared -> scan proceeds");
	zassert_equal(n, 2u, "normal scan after the busy flag clears");
}

ZTEST(cc3501e_host_driver, test_ble_scan_null_ctx_not_ready)
{
	cc3501e_ble_scan_record_t recs[8];
	zassert_equal(
	    cc3501e_ble_scan(NULL, recs, 8u, NULL, 100u), ALP_ERR_NOT_READY, "NULL ctx -> NOT_READY");
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
	zassert_equal(cc3501e_power_policy(&fw, &pp, NULL, 100u), ALP_OK, "POWER_POLICY -> OK");
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
	zassert_equal(
	    cc3501e_power_policy(&fw, NULL, NULL, 100u), ALP_ERR_INVAL, "NULL policy -> INVAL");
	zassert_equal(slave.cmd, 0u, "no transfer clocked");
}

/* ------------------------------------------------------------------ */
/* #733: layout of the directly-serialized (struct-punned) payloads.   */
/*                                                                     */
/* gpio_configure / gpio_write / gpio_set_interrupt / wifi_connect are */
/* NOT hand-packed -- the host hands &struct straight to the SPI DMA   */
/* and the firmware casts the wire buffer back to the struct type, so  */
/* the struct's byte image IS the wire frame.  The _Static_asserts in  */
/* protocol/cc3501e.h fail the build if padding ever creeps in; this   */
/* test documents the intended byte representation at runtime and      */
/* proves the host toolchain lays these out with no interior padding.  */
ZTEST(cc3501e_host_driver, test_punned_payload_layout_733)
{
	zassert_equal(sizeof(alp_cc3501e_gpio_configure_t), 4u, "gpio_configure = 4 wire bytes");
	zassert_equal(sizeof(alp_cc3501e_gpio_write_t), 4u, "gpio_write = 4 wire bytes");
	zassert_equal(
	    sizeof(alp_cc3501e_gpio_set_interrupt_t), 4u, "gpio_set_interrupt = 4 wire bytes");
	zassert_equal(sizeof(alp_cc3501e_wifi_connect_t), 4u, "wifi_connect header = 4 wire bytes");

	const alp_cc3501e_gpio_configure_t c = {
		.cc3501e_gpio = 13u, .direction = 1u, .pull = 2u, .reserved = 0u
	};
	const uint8_t *cb = (const uint8_t *)&c;
	zassert_equal(cb[0], 13u, "byte0 = cc3501e_gpio");
	zassert_equal(cb[1], 1u, "byte1 = direction");
	zassert_equal(cb[2], 2u, "byte2 = pull");

	const alp_cc3501e_wifi_connect_t w = {
		.ssid_len = 5u, .psk_len = 8u, .security = 1u, .reserved = 0u
	};
	const uint8_t *wb = (const uint8_t *)&w;
	zassert_equal(wb[0], 5u, "byte0 = ssid_len");
	zassert_equal(wb[1], 8u, "byte1 = psk_len");
	zassert_equal(wb[2], 1u, "byte2 = security");

	/* The issue's canonical trap: this struct's sizeof is 8, but the wire
	 * header is 7 -- which is exactly why cc3501e_ble_adv_start hand-packs
	 * it (see test_ble_adv_start_encodes_7byte_header) instead of memcpy. */
	zassert_equal(sizeof(alp_cc3501e_ble_adv_start_t), 8u, "ble_adv_start struct = 8, wire = 7");
}

/* ==================== SPI1 HOST PASSTHROUGH (0x55..0x57) =================== */

ZTEST(cc3501e_host_driver, test_spi1_transfer_before_configure_is_not_ready)
{
	/* SESSION gate (the collision this closes): a freshly cc3501e_init()'d ctx
	 * must refuse TRANSFER locally -- never touching the wire -- until a real
	 * CONFIGURE has succeeded in THIS session.  See spi1_configured's comment
	 * in include/alp/chips/cc3501e/core.h. */
	const uint8_t tx[4] = { 0xDEu, 0xADu, 0xBEu, 0xEFu };
	uint8_t       rx[4] = { 0 };
	zassert_equal(cc3501e_spi1_transfer(&fw, tx, rx, 4u, 0u, false, 100u),
	              ALP_ERR_NOT_READY,
	              "TRANSFER before any CONFIGURE in this session -> NOT_READY");
	zassert_equal(slave.cmd, 0u, "rejected locally, never clocked the bus");
}

ZTEST(cc3501e_host_driver, test_spi1_configure_encodes_request_and_decodes_reply)
{
	uint32_t actual_freq_hz = 0u;
	uint16_t max_xfer       = 0u;
	zassert_equal(cc3501e_spi1_configure(
	                  &fw, 10000000u, 0u, ALP_CC3501E_SPI1_CS0, &actual_freq_hz, &max_xfer, 100u),
	              ALP_OK,
	              "CONFIGURE -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_SPI1_CONFIGURE, "opcode 0x55");
	zassert_equal(slave.req_len, 8u, "payload = alp_cc3501e_spi1_configure_t (8 B)");
	zassert_equal(slave.req_pl[0], 0x80u, "freq_hz byte0");
	zassert_equal(slave.req_pl[1], 0x96u, "freq_hz byte1");
	zassert_equal(slave.req_pl[2], 0x98u, "freq_hz byte2");
	zassert_equal(slave.req_pl[3], 0x00u, "freq_hz byte3 (10000000 = 0x00989680 LE)");
	zassert_equal(slave.req_pl[4], 0x00u, "mode");
	zassert_equal(slave.req_pl[5], 0x08u, "bits_per_word is pinned at 8, not a caller parameter");
	zassert_equal(slave.req_pl[6], (uint8_t)ALP_CC3501E_SPI1_CS0, "cs");
	zassert_equal(actual_freq_hz, 10000000u, "decoded actual SCK");
	zassert_equal(max_xfer, (uint16_t)ALP_CC3501E_SPI1_MAX_XFER, "decoded peer chunk cap");
}

ZTEST(cc3501e_host_driver, test_spi1_transfer_encodes_request_matches_protocol_vector)
{
	/* tests/protocol_vectors.txt (firmware repo): spi1_transfer_request =
	 * 56000C000400000100000000DEADBEEF -- header {56 00 0C 00}, payload
	 * {04 00 | 00 | 01 | 00 | 00 00 00 | DE AD BE EF}.  seq 1 is what the
	 * FIRST TRANSFER on a freshly cc3501e_init()'d ctx always carries. */
	zassert_equal(
	    cc3501e_spi1_configure(&fw, 10000000u, 0u, ALP_CC3501E_SPI1_CS0, NULL, NULL, 100u),
	    ALP_OK,
	    "CONFIGURE -> OK");

	const uint8_t tx[4] = { 0xDEu, 0xADu, 0xBEu, 0xEFu };
	uint8_t       rx[4] = { 0 };
	zassert_equal(
	    cc3501e_spi1_transfer(&fw, tx, rx, 4u, 0u, false, 100u), ALP_OK, "TRANSFER -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_SPI1_TRANSFER, "opcode 0x56");
	zassert_equal(slave.req_len, 12u, "8-byte header + 4 inline TX bytes");
	const uint8_t want[12] = {
		0x04u, 0x00u,               /* len LE16 = 4              */
		0x00u,                      /* flags = 0 (single-shot)   */
		0x01u,                      /* seq = 1, first transfer   */
		0x00u,                      /* tx_fill (ignored, tx set) */
		0x00u, 0x00u, 0x00u,        /* reserved                  */
		0xDEu, 0xADu, 0xBEu, 0xEFu, /* tx, packed inline         */
	};
	zassert_mem_equal(
	    slave.req_pl, want, sizeof(want), "emitted bytes match spi1_transfer_request");
	zassert_mem_equal(rx, tx, sizeof(tx), "the loopback model echoes MOSI onto MISO");
}

ZTEST(cc3501e_host_driver, test_spi1_transfer_no_tx_and_no_rx_flags)
{
	zassert_equal(cc3501e_spi1_configure(&fw, 1000000u, 0u, ALP_CC3501E_SPI1_CS0, NULL, NULL, 100u),
	              ALP_OK,
	              "CONFIGURE -> OK");

	/* NO_TX: tx == NULL clocks tx_fill instead -- the model loops the fill
	 * byte back on rx so a decode proves the fill was what actually clocked,
	 * not leftover buffer content. */
	uint8_t rx[3] = { 0 };
	zassert_equal(
	    cc3501e_spi1_transfer(&fw, NULL, rx, 3u, 0xA5u, false, 100u), ALP_OK, "NO_TX -> OK");
	zassert_equal(slave.req_pl[2], (uint8_t)ALP_CC3501E_SPI1_XFER_NO_TX, "flags = NO_TX only");
	zassert_equal(slave.req_len, 8u, "NO_TX carries no inline TX bytes");
	zassert_equal(rx[0], 0xA5u, "rx[0] = the fill byte");
	zassert_equal(rx[1], 0xA5u, "rx[1] = the fill byte");
	zassert_equal(rx[2], 0xA5u, "rx[2] = the fill byte");

	/* NO_RX: rx == NULL discards MISO -- ALP_OK, the caller's own buffer never
	 * touched, and the request still carries its inline TX bytes. */
	const uint8_t tx[2] = { 0x11u, 0x22u };
	zassert_equal(cc3501e_spi1_transfer(&fw, tx, NULL, 2u, 0u, false, 100u), ALP_OK, "NO_RX -> OK");
	zassert_equal(slave.req_pl[2], (uint8_t)ALP_CC3501E_SPI1_XFER_NO_RX, "flags = NO_RX only");
	zassert_equal(slave.req_len, 10u, "8-byte header + 2 inline TX bytes");
}

ZTEST(cc3501e_host_driver, test_spi1_transfer_seq_mismatch_reply_is_io)
{
	zassert_equal(cc3501e_spi1_configure(&fw, 1000000u, 0u, ALP_CC3501E_SPI1_CS0, NULL, NULL, 100u),
	              ALP_OK,
	              "CONFIGURE -> OK");
	g_spi1_reply_bad_seq = true;

	/* A reply that echoes a DIFFERENT seq than this transfer's is the answer
	 * to some OTHER request (the firmware's cache, or a desynced read) --
	 * spi1_take_rx() must report the desync, not hand back those bytes. */
	const uint8_t tx[2] = { 0xAAu, 0xBBu };
	uint8_t       rx[2] = { 0 };
	zassert_equal(cc3501e_spi1_transfer(&fw, tx, rx, 2u, 0u, false, 100u),
	              ALP_ERR_IO,
	              "a reply echoing the wrong seq is a desync, not this transfer's answer");
}

ZTEST(cc3501e_host_driver, test_spi1_release_argless)
{
	zassert_equal(cc3501e_spi1_release(&fw, 100u), ALP_OK, "RELEASE -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_SPI1_RELEASE, "opcode 0x57");
	zassert_equal(slave.req_len, 0u, "RELEASE carries no request payload");
}

ZTEST_SUITE(cc3501e_host_driver, NULL, NULL, reset_before, NULL, NULL);
