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
#include "cc3501e_reply_model.h"

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
	/* Wire byte 3 -- alp_cc3501e_wifi_status_t::last_reason (#2099). */
	uint8_t wifi_last_reason;

	/* CMD_WIFI_CONNECT_STA is worker-routed in the real firmware (the
	 * connect body runs off-ISR and mirrors its outcome into the WIFI_STATUS
	 * latch above once it finishes) -- this fake slave has no worker delay,
	 * so its CONNECT_STA case COMMITS these three configured values into the
	 * wifi_conn_state/wifi_fail_reason/wifi_last_reason fields above the
	 * moment the submit is dispatched.  Set these before running `wifi
	 * connect` to control what cc3501e_wifi_connect()'s own status poll (and
	 * so the console's async result line) reads back. */
	uint8_t connect_result_state;
	uint8_t connect_result_fail_reason;
	uint8_t connect_result_last_reason;

	/* Response status CONNECT_STA's own submit exchange acks with.  The real
	 * firmware answers this worker-routed opcode RESP_ERR_BUSY (the drain
	 * runs the seconds-long connect body off THIS exchange; the host never
	 * collects the real outcome through the submit's own ack -- see
	 * cc3501e_wifi_connect()'s "SUBMIT ONCE" comment) -- default here matches
	 * that, so a test that leaves this unset still exercises the ack the
	 * driver actually has to tolerate.  Set to ALP_CC3501E_RESP_ERR_INVALID
	 * to model a synchronous reject (bad payload) instead, the one ack value
	 * cc3501e_wifi_connect() DOES trust and short-circuits on. */
	uint8_t connect_sta_resp;

	/* When true, CONNECT_STA's dispatch acks connect_sta_resp WITHOUT
	 * committing connect_result_* into the live latch fields -- models a
	 * submit that a concurrent worker op bounced BUSY, or that a transport
	 * IO fault lost, without ever actually queuing a new attempt: the latch
	 * is left exactly as it was (state, fail_reason, and last_reason all
	 * whatever an EARLIER, unrelated attempt left there). */
	bool connect_sta_skip_commit;

	/* Set true the moment CONNECT_STA's dispatch commits connect_result_*
	 * (see above) -- this fake slave's model of "the connect body just
	 * published a terminal outcome".  Used to isolate the WIFI_STATUS
	 * dispatch(es) that happen AFTER that point: the first is
	 * cc3501e_wifi_connect()'s own loop discovering the terminal state (must
	 * succeed normally, or the loop cannot return), every one after that is
	 * the CONSOLE's own separate diagnostic re-fetch. */
	bool connect_terminal_committed;

	/* Total WIFI_STATUS dispatches after connect_terminal_committed went
	 * true.  A test that arms wifi_status_fail_after_terminal below and
	 * asserts this stops at 2 (the loop's own terminal read, plus exactly
	 * ONE diagnostic re-fetch) fails if the diagnostic fetch is swapped back
	 * to the retrying cc3501e_wifi_status() -- that call would keep
	 * incrementing this on every retry instead of failing once. */
	unsigned int wifi_status_calls_after_terminal;

	/* When true, every WIFI_STATUS REQUEST-HEADER phase transceive AFTER the
	 * first one following connect_terminal_committed fails OUTRIGHT (as if
	 * the shared bridge transport itself were down) instead of ever reaching
	 * a decoded reply -- same shape as tests/zephyr/cc3501e_host_driver's
	 * g_status_io_down_remaining, and DELIBERATELY not a decoded
	 * RESP_ERR_RADIO/PROTOCOL/INTERNAL reply: cc3501e_core.c's poll_by_repeat
	 * treats THOSE as a terminal, non-retried decode failure, so they would
	 * not tell a bounded single attempt apart from a retrying one.  A
	 * pre-decode transport fault IS genuinely retryable, which is the whole
	 * point here -- models a transport fault landing in the gap between the
	 * connect loop's own terminal read and the console's separate diagnostic
	 * fetch. */
	bool wifi_status_fail_after_terminal;

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

	/* Total WIFI_STATUS dispatches, ever -- the entry-clean pre-check inside
	 * cc3501e_wifi_connect() issues one of these before every submit, so a
	 * test proving "no [additional] status fetch happened" needs the total,
	 * not an implicit zero. */
	unsigned int wifi_status_count;
} slave;

static void slave_reset(void)
{
	memset(&slave, 0, sizeof(slave));
	slave.phase            = PH_REQ_HDR;
	slave.wifi_conn_state  = ALP_CC3501E_WIFI_CONNECTED;
	slave.wifi_fail_reason = ALP_CC3501E_WIFI_FAIL_NONE;
	slave.wifi_conn_rssi   = FIX_RSSI_LATCHED;
	slave.connect_sta_resp = ALP_CC3501E_RESP_ERR_BUSY;
	slave.rssi_resp        = ALP_CC3501E_RESP_OK;
	slave.ip_resp          = ALP_CC3501E_RESP_OK;
}

/* RESP_OK stages the real MAJOR-4 shape (padded + CRC trailer -- the only
 * shape cc3501e_reply_verdict() accepts for a 0x5A status, see
 * cc3501e_reply_model.h); any ALP_CC3501E_RESP_ERR_* code keeps the plain
 * legacy shape, which the reply-verdict decode also accepts pre-negotiation. */
static void stage_reply(uint8_t st, const uint8_t *data, uint16_t n)
{
	if (st == ALP_CC3501E_RESP_OK) {
		slave.reply_len = cc3501e_model_stage_reply(slave.reply_pl, slave.cmd, st, data, n);
	} else {
		slave.reply_len = cc3501e_model_stage_legacy_reply(slave.reply_pl, st, data, n);
	}
}

static void slave_dispatch(void)
{
	switch (slave.cmd) {
	case ALP_CC3501E_CMD_WIFI_STATUS: {
		/* Attempt counting + transport-fault injection for this opcode both
		 * live in alp_spi_transceive()'s PH_REQ_HDR case below, matching
		 * tests/zephyr/cc3501e_host_driver's g_status_io_down_remaining shape
		 * -- a genuine transport-level fault must fail BEFORE any status
		 * byte is decoded (so poll_by_repeat's retry gate treats it as
		 * retryable), which this dispatch (reached only once the exchange
		 * already succeeded) is too late to model. */
		const uint8_t st[4] = {
			slave.wifi_conn_state,
			slave.wifi_fail_reason,
			(uint8_t)slave.wifi_conn_rssi,
			slave.wifi_last_reason,
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
	case ALP_CC3501E_CMD_WIFI_CONNECT_STA: {
		/* The submit's own ack is untrusted by the driver for anything other
		 * than ALP_ERR_INVAL (see cc3501e_wifi_connect()'s comment) -- ack
		 * with connect_sta_resp (RESP_ERR_BUSY by default, matching the real
		 * firmware).  Commit the configured outcome into the live latch
		 * fields the SAME exchange, so the very next WIFI_STATUS poll
		 * (cc3501e_wifi_connect()'s own loop) reads it straight back with no
		 * simulated worker delay to wait out -- unless connect_sta_skip_commit
		 * is set, modelling a submit that never actually queued an attempt. */
		if (!slave.connect_sta_skip_commit) {
			slave.wifi_conn_state            = slave.connect_result_state;
			slave.wifi_fail_reason           = slave.connect_result_fail_reason;
			slave.wifi_last_reason           = slave.connect_result_last_reason;
			slave.connect_terminal_committed = true;
		}
		stage_reply(slave.connect_sta_resp, NULL, 0u);
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
	if (slave.phase == PH_REQ_HDR && tx[0] == ALP_CC3501E_CMD_WIFI_STATUS) {
		slave.wifi_status_count++;
		if (slave.connect_terminal_committed) {
			slave.wifi_status_calls_after_terminal++;
			if (slave.wifi_status_fail_after_terminal &&
			    slave.wifi_status_calls_after_terminal > 1u) {
				/* Genuine transport-level fault, injected BEFORE
				 * slave.phase advances (so the next attempt starts clean) --
				 * see wifi_status_fail_after_terminal's own comment for why
				 * this must be pre-decode, not a staged RESP_ERR_* reply. */
				return ALP_ERR_IO;
			}
		}
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

/* alp_delay_us is a no-op under the sim; the GPIO seams are inert (the
 * fixture's ctx leaves reset/enable/ready pins unset, so the driver never
 * calls them). alp_delay_ms and alp_uptime_ms share one fake millisecond
 * counter (same pattern as tests/zephyr/cc3501e_poll_deadline), so
 * poll_by_repeat()'s deadline (issue #1953) still elapses deterministically
 * for any retry this suite drives, without any real sleeping. */
static uint64_t g_fake_now_ms;

void alp_delay_us(uint32_t us)
{
	(void)us;
}
void alp_delay_ms(uint32_t ms)
{
	g_fake_now_ms += ms;
}
uint64_t alp_uptime_ms(void)
{
	return g_fake_now_ms;
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

/* Run an async `wifi connect`, then wait (bounded, polling) for the
 * background companion_conn_thread to notice conn_pending and print its
 * result line -- identified by @p must_appear, a substring every outcome of
 * that line carries (`"failed ("` / `"timed out"`), so this helper works for
 * both the failure and timeout shapes without hardcoding the poll interval.
 *
 * FAILS THE TEST (via zassert) if @p must_appear never shows up in the
 * bounded window, rather than silently returning whatever was captured.
 * Without this, a caller asserting on the ABSENCE of something (e.g. no
 * "reason:" suffix) would pass vacuously if the result line never printed at
 * all -- the missing "reason:" and the missing line itself are
 * indistinguishable to a plain strstr()-is-NULL check.  Requiring
 * must_appear to have actually shown up is what makes the absence assertion
 * meaningful. */
static const char *run_wifi_connect_and_wait(const char *line, const char *must_appear)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();

	shell_backend_dummy_clear_output(sh);
	(void)shell_execute_cmd(sh, line);

	size_t      len  = 0;
	const char *out  = shell_backend_dummy_get_output(sh, &len);
	bool        seen = WAIT_FOR((out = shell_backend_dummy_get_output(sh, &len)) != NULL &&
	                                strstr(out, must_appear) != NULL,
	                            2000000,
	                            k_msleep(20));
	zassert_true(
	    seen, "result line containing \"%s\" never printed: %s", must_appear, out ? out : "(null)");
	/* companion_conn_thread clears conn_pending in the statement right AFTER
	 * the print WAIT_FOR just observed -- give it a short margin so the NEXT
	 * test's own `wifi connect` does not race a conn_pending that is still
	 * (briefly) true and get rejected as "already in progress". */
	k_msleep(20);
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

/* #2099: a failed connect whose WIFI_STATUS latch carries a non-zero
 * last_reason must print it, after the existing `fail:` line. */
ZTEST(cc3501e_console_wifi, test_status_prints_reason_when_recorded_2099)
{
	slave.wifi_conn_state  = ALP_CC3501E_WIFI_CONN_FAILED;
	slave.wifi_fail_reason = ALP_CC3501E_WIFI_FAIL_REJECTED;
	slave.wifi_last_reason = 15u;

	const char *out = run("alp companion wifi status");

	zassert_not_null(strstr(out, "fail:  2"), "fail line missing: %s", out);
	zassert_not_null(strstr(out, "reason: 15"), "reason line missing: %s", out);
}

/* The zero side of the same contract: older bridge firmware (or a failure
 * mode that never recorded one) sends last_reason == 0, and the console must
 * NOT print a "reason: 0" line that looks like a recorded-but-meaningless
 * code. */
ZTEST(cc3501e_console_wifi, test_status_omits_reason_when_zero_2099)
{
	slave.wifi_conn_state  = ALP_CC3501E_WIFI_CONN_FAILED;
	slave.wifi_fail_reason = ALP_CC3501E_WIFI_FAIL_TIMEOUT;
	slave.wifi_last_reason = 0u;

	const char *out = run("alp companion wifi status");

	zassert_not_null(strstr(out, "fail:  1"), "fail line missing: %s", out);
	zassert_is_null(strstr(out, "reason:"), "no reason line when nothing was recorded: %s", out);
}

/* ============================ #2099 (wifi connect) ========================= */

/* A failed `wifi connect` whose WIFI_STATUS latch carries a non-zero
 * last_reason must print it on the async result line, fetched via the
 * bounded cc3501e_wifi_status_once() rather than the down-window-retrying
 * cc3501e_wifi_status(). */
ZTEST(cc3501e_console_wifi, test_connect_prints_reason_when_recorded_2099)
{
	slave.connect_result_state       = ALP_CC3501E_WIFI_CONN_FAILED;
	slave.connect_result_fail_reason = ALP_CC3501E_WIFI_FAIL_REJECTED;
	slave.connect_result_last_reason = 15u;

	const char *out =
	    run_wifi_connect_and_wait("alp companion wifi connect myssid mypass wpa3", "failed (");

	zassert_not_null(strstr(out, "reason: 15"), "reason line missing: %s", out);
}

/* The zero side of the same contract: nothing recorded must print no reason
 * suffix at all -- not "reason: 0". */
ZTEST(cc3501e_console_wifi, test_connect_omits_reason_when_zero_2099)
{
	slave.connect_result_state       = ALP_CC3501E_WIFI_CONN_FAILED;
	slave.connect_result_fail_reason = ALP_CC3501E_WIFI_FAIL_REJECTED;
	slave.connect_result_last_reason = 0u;

	const char *out =
	    run_wifi_connect_and_wait("alp companion wifi connect myssid mypass wpa3", "failed (");

	zassert_is_null(strstr(out, "reason:"), "no reason line when nothing was recorded: %s", out);
}

/* The "timed out" shape gets the same fetch.  An event during ASSOCIATION
 * ends the attempt REJECTED, not TIMEOUT -- a TIMEOUT carrying a non-zero
 * last_reason instead comes from a DISCONNECT landing during the POST-
 * ASSOCIATION DHCP poll (the attempt is still open/CONNECTING at that point,
 * so the gate that records last_reason has not closed yet, even though the
 * overall connect ultimately times out waiting on the lease).  The timeout
 * line must surface that reason too, not just the generic failure line. */
ZTEST(cc3501e_console_wifi, test_connect_timeout_prints_reason_when_recorded_2099)
{
	slave.connect_result_state       = ALP_CC3501E_WIFI_CONN_FAILED;
	slave.connect_result_fail_reason = ALP_CC3501E_WIFI_FAIL_TIMEOUT;
	slave.connect_result_last_reason = 7u;

	const char *out =
	    run_wifi_connect_and_wait("alp companion wifi connect myssid mypass wpa3", "timed out");

	zassert_not_null(strstr(out, "reason: 7"), "reason line missing on timeout: %s", out);
}

/* Round-2 review (#2099): attempt A fails and leaves last_reason=15 on the
 * latch.  Attempt B's own CONNECT_STA submit is bounced BUSY by a concurrent
 * worker op (modelled here by connect_sta_skip_commit -- the ack is BUSY,
 * same as always, but no new outcome is ever queued), so B's own status poll
 * loop never sees CONN_FAILED and simply runs out its timeout_ms.  The stale
 * last_reason=15 from attempt A must NOT be printed against attempt B, which
 * never actually started -- the console only trusts last_reason when the
 * fetched latch's own state reads CONN_FAILED. */
ZTEST(cc3501e_console_wifi, test_connect_timeout_omits_stale_reason_from_unstarted_attempt_2099)
{
	slave.wifi_conn_state         = ALP_CC3501E_WIFI_DISCONNECTED;
	slave.wifi_fail_reason        = ALP_CC3501E_WIFI_FAIL_NONE;
	slave.wifi_last_reason        = 15u; /* stale, left by an earlier, unrelated attempt */
	slave.connect_sta_skip_commit = true;

	const char *out =
	    run_wifi_connect_and_wait("alp companion wifi connect myssid mypass wpa3", "timed out");

	zassert_is_null(strstr(out, "reason:"),
	                "must not print an unrelated prior attempt's stale reason: %s",
	                out);
}

/* CONNECT_STA's own submit exchange rejecting RESP_ERR_INVALID (bad payload)
 * is the ONE ack cc3501e_wifi_connect() trusts and short-circuits on -- no
 * attempt is ever queued, so the console must not fetch WIFI_STATUS at all
 * for its diagnostic reason (it would only ever read a PRIOR, unrelated
 * attempt's value).  The entry-clean pre-check inside cc3501e_wifi_connect()
 * still issues its own single WIFI_STATUS read before every submit
 * regardless of outcome, so the total count here is exactly 1, not 0. */
ZTEST(cc3501e_console_wifi, test_connect_invalid_skips_status_fetch_2099)
{
	slave.connect_sta_resp = ALP_CC3501E_RESP_ERR_INVALID;

	const char *out =
	    run_wifi_connect_and_wait("alp companion wifi connect myssid mypass wpa3", "failed (");

	zassert_is_null(strstr(out, "reason:"), "no reason line on a synchronous reject: %s", out);
	zassert_equal(slave.wifi_status_count,
	              1u,
	              "only the entry-clean pre-check's WIFI_STATUS read should have "
	              "happened, no diagnostic fetch on top of it: got %u",
	              slave.wifi_status_count);
}

/* A transport fault landing in the gap between cc3501e_wifi_connect()'s own
 * terminal WIFI_STATUS read and the console's separate diagnostic re-fetch
 * must cost exactly ONE extra WIFI_STATUS attempt, not a down-window's worth
 * of retries -- proving the console's fetch is cc3501e_wifi_status_once()
 * (bounded, non-retried), not the retrying cc3501e_wifi_status(). */
ZTEST(cc3501e_console_wifi, test_connect_diagnostic_fetch_is_not_retried_2099)
{
	slave.connect_result_state            = ALP_CC3501E_WIFI_CONN_FAILED;
	slave.connect_result_fail_reason      = ALP_CC3501E_WIFI_FAIL_REJECTED;
	slave.connect_result_last_reason      = 15u;
	slave.wifi_status_fail_after_terminal = true;

	const char *out =
	    run_wifi_connect_and_wait("alp companion wifi connect myssid mypass wpa3", "failed (");

	/* The diagnostic fetch itself failed (IO), so no reason line -- this
	 * assertion alone would also pass if the fetch retried and eventually
	 * gave up, which is exactly why the call-count assertion below is the
	 * one that actually proves the bound. */
	zassert_is_null(
	    strstr(out, "reason:"), "diagnostic fetch failed -- no reason to print: %s", out);
	zassert_equal(slave.wifi_status_calls_after_terminal,
	              2u,
	              "exactly one WIFI_STATUS attempt should follow the loop's own terminal "
	              "read (a bounded cc3501e_wifi_status_once() call) -- got %u; a value "
	              "far above 2 means the diagnostic fetch is retrying (cc3501e_wifi_status())",
	              slave.wifi_status_calls_after_terminal);
}

ZTEST_SUITE(cc3501e_console_wifi, NULL, suite_setup, reset_before, NULL, NULL);
