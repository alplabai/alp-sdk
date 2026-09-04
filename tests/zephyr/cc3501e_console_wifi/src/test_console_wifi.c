/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * What `alp companion wifi status` PRINTS, on a link whose firmware behaves
 * the way the real CC3501E firmware behaves.
 *
 * The defect this suite pins (issue #1387): the WIFI_STATUS reply carries an
 * rssi_dbm byte that the firmware NEVER populates -- every terminal outcome in
 * cc3501e-bridge-firmware:hal/ti/cc3501e_hw_ti_wifi.c publishes it through
 * wifi_conn_set(), which always sets it to 0 -- and the console printed that
 * byte as a measurement.
 * 0 dBm is a legal int8 RSSI near the top of the range, so nothing downstream
 * can reject it: the user got a confident, plausible, unmeasured number while
 * the very same session's `wifi connect` reported the true -49 dBm from
 * WIFI_GET_RSSI, which is a genuinely different (and working) source.
 *
 * So the slave model below reproduces exactly that asymmetry: the WIFI_STATUS
 * latch reports CONNECTED with an rssi byte of 0, while WIFI_GET_RSSI reports
 * a real reading.  A status print that shows 0 dBm is reporting the fiction.
 *
 * The seams: this test supplies alp_spi_transceive() (the software model of
 * the firmware SPI slave, same lockstep shape as
 * tests/zephyr/cc3501e_host_driver) and the `companion_cc3501e` handle the
 * console TU takes as an extern from alp_console_companion.c.  Everything in
 * between -- the console command, the host driver, the wire format -- is the
 * production code.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

#include "alp/chips/cc3501e.h"
#include "alp/protocol/cc3501e.h"

/* ---- what the two RSSI sources report ------------------------------------- *
 * FIX_RSSI_MEASURED is the real reading WIFI_GET_RSSI hands back.  The latch
 * byte is FIX_RSSI_LATCHED = 0 because that is the only value the firmware
 * latch has ever been able to hold -- not because this test chose a
 * convenient one. */
#define FIX_RSSI_MEASURED (-49)
#define FIX_RSSI_LATCHED  0

/* ---- software model of the firmware SPI slave ------------------------------ *
 * CS-less 3-wire lockstep: request header -> (request payload) -> reply header
 * -> reply payload.  Header is 4 bytes LE [cmd | flags | payload_len(LE16)];
 * the reply payload's first byte is the ALP_CC3501E_RESP_* status. */
enum slave_phase {
	PH_REQ_HDR = 0,
	PH_REQ_PL,
	PH_REPLY_HDR,
	PH_REPLY_PL,
};

static struct {
	enum slave_phase phase;
	uint8_t          cmd;
	uint16_t         req_len;
	uint8_t          req_pl[ALP_CC3501E_MAX_PAYLOAD];

	uint8_t  reply_pl[ALP_CC3501E_MAX_PAYLOAD]; /* status byte + data */
	uint16_t reply_len;

	/* The connection-status latch WIFI_STATUS reads (models the firmware's
	 * g_wifi_conn / handle_wifi_status). */
	uint8_t wifi_conn_state;
	uint8_t wifi_fail_reason;
	int8_t  wifi_conn_rssi;

	/* Response status WIFI_GET_RSSI answers with -- RESP_OK stages the real
	 * measurement, anything else models a radio read that could not be
	 * served (e.g. the radio went back down between the two requests). */
	uint8_t rssi_resp;

	/* Response status WIFI_GET_IP answers with -- RESP_OK stages the leased
	 * address, anything else models a lease query that could not be served. */
	uint8_t ip_resp;

	/* Bumped on every WIFI_GET_RSSI dispatch, regardless of what opcode (if
	 * any) is dispatched after it.  slave.cmd alone only records the LAST
	 * opcode seen, so it cannot tell "no radio read was issued" from "a radio
	 * read was issued and then something else ran after it". */
	unsigned int get_rssi_count;
} slave;

static void slave_reset(void)
{
	memset(&slave, 0, sizeof(slave));
	slave.phase            = PH_REQ_HDR;
	slave.wifi_conn_state  = ALP_CC3501E_WIFI_CONNECTED;
	slave.wifi_fail_reason = ALP_CC3501E_WIFI_FAIL_NONE;
	slave.wifi_conn_rssi   = FIX_RSSI_LATCHED;
	slave.rssi_resp        = ALP_CC3501E_RESP_OK;
	slave.ip_resp          = ALP_CC3501E_RESP_OK;
}

static void stage_reply(uint8_t st, const uint8_t *data, uint16_t n)
{
	slave.reply_pl[0] = st;
	if (n > 0u) {
		memcpy(&slave.reply_pl[1], data, n);
	}
	slave.reply_len = (uint16_t)(1u + n);
}

static void slave_dispatch(void)
{
	switch (slave.cmd) {
	case ALP_CC3501E_CMD_WIFI_STATUS: {
		const uint8_t st[4] = {
			slave.wifi_conn_state,
			slave.wifi_fail_reason,
			(uint8_t)slave.wifi_conn_rssi,
			0u,
		};
		stage_reply(ALP_CC3501E_RESP_OK, st, 4u);
		break;
	}
	case ALP_CC3501E_CMD_WIFI_GET_RSSI: {
		slave.get_rssi_count++;
		const uint8_t r = (uint8_t)FIX_RSSI_MEASURED;
		if (slave.rssi_resp != ALP_CC3501E_RESP_OK) {
			stage_reply(slave.rssi_resp, NULL, 0u);
			break;
		}
		stage_reply(ALP_CC3501E_RESP_OK, &r, 1u);
		break;
	}
	case ALP_CC3501E_CMD_WIFI_GET_IP: {
		if (slave.ip_resp != ALP_CC3501E_RESP_OK) {
			stage_reply(slave.ip_resp, NULL, 0u);
			break;
		}
		/* Wire order is REVERSED (the firmware emits the lwIP network-order
		 * u32 MSB-first); 192.168.1.14 = 0xC0A8010E -> {0x0E,0x01,0xA8,0xC0}. */
		const uint8_t wire[4] = { 0x0E, 0x01, 0xA8, 0xC0 };
		stage_reply(ALP_CC3501E_RESP_OK, wire, 4u);
		break;
	}
	default:
		stage_reply(ALP_CC3501E_RESP_ERR_INVALID, NULL, 0u);
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
 * leaves reset/enable/ready pins unset, so the driver never calls them). */
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

/* ---- console scaffolding --------------------------------------------------- *
 * The console TU under test registers its `wifi` group onto the (alp,
 * companion) dynamic subcommand set that alp_console_companion.c creates, and
 * reads the app-registered handle from `companion_cc3501e`.  Declare exactly
 * those two things here (mirroring alp_console.c / alp_console_companion.c)
 * rather than pulling in the whole SDK console. */
cc3501e_t *companion_cc3501e;

SHELL_SUBCMD_SET_CREATE(alp_subcmds, (alp));
SHELL_SUBCMD_SET_CREATE(alp_companion_subcmds, (alp, companion));
SHELL_SUBCMD_ADD((alp),
                 companion,
                 &alp_companion_subcmds,
                 "Companion chip bridge (GD32 / CC3501E)",
                 NULL,
                 1,
                 0);
SHELL_CMD_REGISTER(alp, &alp_subcmds, "Alp SoM diagnostics console", NULL);

/* ---- fixture --------------------------------------------------------------- */

static cc3501e_t  fw;
static alp_spi_t *fake_bus = (alp_spi_t *)&fw; /* opaque, non-NULL; the stub ignores it */

/* Run a shell line on the dummy backend and return its captured output. */
static const char *run(const char *line)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();

	shell_backend_dummy_clear_output(sh);
	(void)shell_execute_cmd(sh, line);

	size_t      len;
	const char *out = shell_backend_dummy_get_output(sh, &len);
	return out;
}

static void *suite_setup(void)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();

	WAIT_FOR(shell_ready(sh), 20000, k_msleep(1));
	zassert_true(shell_ready(sh), "timed out waiting for dummy shell backend");
	return NULL;
}

static void reset_before(void *fixture)
{
	(void)fixture;
	slave_reset();
	zassert_equal(cc3501e_init(&fw, fake_bus), ALP_OK, "init binds the (fake) bus");
	companion_cc3501e = &fw;
}

/* ============================ #1387 ======================================== */

/* The reported bug, verbatim: a live connection whose firmware latch carries
 * the unpopulated 0 must NOT be reported as "rssi:  0 dBm".  Against the
 * pre-fix console (which printed alp_cc3501e_wifi_status_t::rssi_dbm straight
 * out of the WIFI_STATUS reply) this fails on both assertions -- the printed
 * line is the latch's fiction, not the -49 dBm the radio actually measures. */
ZTEST(cc3501e_console_wifi, test_status_rssi_is_measured_not_the_latch_byte_1387)
{
	const char *out = run("alp companion wifi status");

	zassert_not_null(strstr(out, "state: connected"), "state line missing: %s", out);
	zassert_is_null(strstr(out, "rssi:  0 dBm"),
	                "printed the unpopulated WIFI_STATUS latch byte as a measurement: %s",
	                out);
	zassert_not_null(
	    strstr(out, "rssi:  -49 dBm"), "expected the WIFI_GET_RSSI measurement (-49 dBm): %s", out);
}

/* The other half of the contract: when the only real source cannot be read,
 * say so.  Never fall back to the latch byte, and never print a number.  Same
 * shape #1382 landed on the connect path ("rssi=unavailable (%d)").
 *
 * RESP_ERR_NOT_READY is a deterministic firmware reject (not BUSY / not IO),
 * so poll_by_repeat does not retry it -- the console sees ALP_ERR_NOT_READY. */
ZTEST(cc3501e_console_wifi, test_status_says_unavailable_when_the_radio_read_fails_1387)
{
	slave.rssi_resp = ALP_CC3501E_RESP_ERR_NOT_READY;

	const char *out = run("alp companion wifi status");

	zassert_not_null(strstr(out, "state: connected"), "state line missing: %s", out);
	zassert_not_null(strstr(out, "rssi:  unavailable"), "a failed RSSI read must say so: %s", out);
	zassert_is_null(strstr(out, "dBm"), "no dBm number may be printed at all: %s", out);
}

/* A guard against fixing #1387 by simply dropping the RSSI line: the rest of
 * the status report -- the two fields the issue confirms were already correct
 * -- must survive unchanged. */
ZTEST(cc3501e_console_wifi, test_status_still_reports_state_and_ip)
{
	const char *out = run("alp companion wifi status");

	zassert_not_null(strstr(out, "state: connected"), "state line missing: %s", out);
	zassert_not_null(strstr(out, "ip:    192.168.1.14"), "ip line missing: %s", out);
}

/* Same shape as the RSSI-unavailable case, for the IP lease query: when the
 * WIFI_GET_IP read fails, say so and print no address, rather than silently
 * omitting the line (which is indistinguishable from "not connected yet"). */
ZTEST(cc3501e_console_wifi, test_status_says_unavailable_when_the_ip_read_fails_1387)
{
	slave.ip_resp = ALP_CC3501E_RESP_ERR_NOT_READY;

	const char *out = run("alp companion wifi status");

	zassert_not_null(strstr(out, "state: connected"), "state line missing: %s", out);
	zassert_not_null(strstr(out, "ip:    unavailable"), "a failed IP read must say so: %s", out);
	zassert_is_null(
	    strstr(out, "ip:    192.168.1.14"), "no address may be printed at all: %s", out);
}

/* Not associated: no RSSI line at all, and -- the point -- no WIFI_GET_RSSI
 * radio op issued for a link that has none to measure. */
ZTEST(cc3501e_console_wifi, test_status_disconnected_reports_no_rssi)
{
	slave.wifi_conn_state = ALP_CC3501E_WIFI_DISCONNECTED;

	const char *out = run("alp companion wifi status");

	zassert_not_null(strstr(out, "state: disconnected"), "state line missing: %s", out);
	zassert_is_null(strstr(out, "rssi:"), "no RSSI belongs on an unassociated link: %s", out);
	/* Count, not slave.cmd (the last opcode seen): a trailing request after a
	 * WIFI_GET_RSSI would make slave.cmd that trailing opcode and pass this
	 * assertion vacuously while a radio read WAS issued. */
	zassert_equal(slave.get_rssi_count, 0, "no radio read should be issued when not associated");
}

ZTEST_SUITE(cc3501e_console_wifi, NULL, suite_setup, reset_before, NULL, NULL);
