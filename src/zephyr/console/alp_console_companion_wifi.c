/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp companion wifi` -- CC3501E Wi-Fi (scan / connect / disconnect / ap /
 * ap-stop / status), Alif companion only.  Command-group TU of the
 * alp_console_companion.c split (#673 Phase 2): registers onto the
 * (alp, companion) dynamic subcommand set the core TU declares.  Shared
 * companion context comes from
 * alp_console_companion_internal.h.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <alp/ext/cc3501e/console.h>
#include <alp/peripheral.h>

#include "alp_console_companion_internal.h"

#if !IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
/* ---- CC3501E Wi-Fi (Alif companion) ------------------------------------- */
#define ALP_COMPANION_WIFI_SCAN_MAX 16u
#define ALP_COMPANION_WIFI_SCAN_MS  30000u
/* Cover the CC3501E connect budget: L2 assoc up to 30s (WPA3-SAE is slower than
 * WPA2 -- see cc3501e_hw_ti.c) + the STA DHCP poll (~10s) = ~40s, plus margin. */
#define ALP_COMPANION_WIFI_CONN_MS 50000u

/* ---- async Wi-Fi connect (the bridge can't block the shell) -------------- *
 * `wifi connect` SUBMITS the request (records SSID/sec/pass, sets conn_pending)
 * and returns immediately so the shell stays live.  The low-rate worker thread
 * below then runs dev's BLOCKING cc3501e_wifi_connect() itself -- the ~15 s
 * association blocks THIS thread, not the shell -- and prints the result. */
#define ALP_COMPANION_CONN_POLL_MS 200

static volatile bool       conn_pending;  /* an async connect is awaiting its run */
static const struct shell *conn_sh;       /* shell to print the async result on   */
static char                conn_ssid[33]; /* SSID to connect to                   */
static char                conn_pass[64]; /* passphrase ("" = open)               */
static uint8_t             conn_sec;      /* 0=open, 1=wpa2-psk, 2=wpa3-sae        */

static void companion_conn_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		k_msleep(ALP_COMPANION_CONN_POLL_MS);
		if (!conn_pending || companion_cc3501e == NULL) {
			continue;
		}

		/* Run the BLOCKING connect on this worker thread (dev cc3501e_wifi_connect
		 * blocks until the firmware reports the association result); the shell
		 * thread runs above this one, so the prompt stays responsive throughout. */
		alp_status_t s = cc3501e_wifi_connect(
		    companion_cc3501e, conn_ssid, conn_sec, conn_pass, ALP_COMPANION_WIFI_CONN_MS);

		if (s == ALP_OK) {
			/* cc3501e_wifi_connect() only returns ALP_OK once the independent
			 * WIFI_STATUS latch itself reported CONNECTED (see cc3501e_wifi.c),
			 * so this print is reporting a firmware-confirmed association, not
			 * an echo of what the shell was asked to connect to.  The RSSI
			 * read is a SEPARATE request, though, and can itself fail (e.g. the
			 * radio just went back down) -- check its status instead of
			 * printing a zero-initialised local as if it were a real reading
			 * (issue #1376: a discarded failure here rendered as a
			 * plausible-looking "rssi=0 dBm" on any connect success). */
			int8_t       rssi = 0;
			alp_status_t rs   = cc3501e_wifi_rssi(companion_cc3501e, &rssi);
			if (rs == ALP_OK) {
				shell_print(conn_sh, "wifi connected \"%s\"  rssi=%d dBm", conn_ssid, (int)rssi);
			} else {
				shell_print(
				    conn_sh, "wifi connected \"%s\"  rssi=unavailable (%d)", conn_ssid, (int)rs);
			}
		} else if (s == ALP_ERR_TIMEOUT) {
			shell_warn(conn_sh, "wifi connect to \"%s\": timed out", conn_ssid);
		} else {
			shell_error(conn_sh, "wifi connect to \"%s\" failed (%d)", conn_ssid, (int)s);
		}
		conn_pending = false;
	}
}

/* Low priority (7); the shell + companion commands run above it.  Idles on
 * conn_pending == false, so it costs nothing until a connect submits. */
K_THREAD_DEFINE(companion_conn_tid, 1024, companion_conn_thread, NULL, NULL, NULL, 7, 0, 0);

static int cmd_companion_wifi_scan(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}

	static cc3501e_scan_record_t recs[ALP_COMPANION_WIFI_SCAN_MAX];
	size_t                       n = 0;
	alp_status_t                 s = cc3501e_wifi_scan(
	    companion_cc3501e, recs, ALP_COMPANION_WIFI_SCAN_MAX, &n, ALP_COMPANION_WIFI_SCAN_MS);

	if (s != ALP_OK) {
		shell_error(sh, "scan failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "%u AP(s):", (unsigned int)n);
	for (size_t i = 0; i < n; i++) {
		/* security decoded from the raw TI SecurityInfo (sec-type bitmap). */
		shell_print(sh,
		            "  %-32s ch%-3u %4d dBm  %s",
		            recs[i].ssid,
		            (unsigned int)recs[i].channel,
		            (int)recs[i].rssi_dbm,
		            cc3501e_wifi_sec_name(recs[i].security_info));
	}
	return 0;
}

static int cmd_companion_wifi_connect(const struct shell *sh, size_t argc, char **argv)
{
	/* Validate the token shape BEFORE any state check, so a usage error is
	 * reported as one (#1376).  This ordering is deliberate: reporting
	 * "companion not registered" for a mis-typed command hides the real
	 * problem, and the argument shape is wrong regardless of whether a
	 * companion happens to be bound.
	 *
	 * The 4th token is the ONLY optional flag, and it must be "wpa3".  It used
	 * to be tested with `== 0` and otherwise IGNORED, which made the dangerous
	 * case silent: an UNQUOTED SSID containing a space splits across
	 * argv[1]/argv[2], pushing the real passphrase into argv[3], where it was
	 * dropped without a word.  The user then saw a confident "connecting" line
	 * naming an SSID they never typed, with their passphrase consumed as a
	 * security token.  Associating with a truncated SSID is worse than
	 * refusing, so an unrecognised token is now an error. */
	if (argc >= 4 && strcmp(argv[3], "wpa3") != 0) {
		shell_error(sh,
		            "unrecognised argument \"%s\" -- the only optional 4th token is "
		            "\"wpa3\".",
		            argv[3]);
		shell_error(sh,
		            "an SSID or passphrase containing spaces must be quoted: "
		            "wifi connect \"my ssid\" \"my pass\" [wpa3]");
		return -EINVAL;
	}
	/* WPA3-SAE has no open mode, so "wpa3" with an EMPTY passphrase is a
	 * contradiction, not a configuration (#1552). It used to be accepted: the
	 * empty passphrase selected open and the token then overwrote that with
	 * WPA3-SAE-and-no-PSK. Same reasoning as the token guard above -- a
	 * contradictory argument shape is wrong whether or not a companion is
	 * bound, so it is refused HERE, before the state checks. `argc >= 4`
	 * implies `argc >= 3`, so argv[2] is always readable in this branch. */
	if (argc >= 4 && argv[2][0] == '\0') {
		shell_error(sh, "\"wpa3\" requires a passphrase -- WPA3-SAE has no open mode.");
		shell_error(sh,
		            "for an open network drop the token: "
		            "wifi connect \"my ssid\"");
		return -EINVAL;
	}
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	if (conn_pending) {
		shell_warn(sh, "a connect is already in progress -- wait for its result");
		return -EBUSY;
	}
	const char *ssid = argv[1];
	const char *pass = (argc >= 3) ? argv[2] : "";
	/* No passphrase -> open; a passphrase -> WPA2-PSK (the common case); a
	 * trailing "wpa3" token -> WPA3-SAE (the token itself validated above).
	 *
	 * ONE expression, deliberately: this used to be an assignment followed by
	 * `if (argc >= 4) { sec = 2u; }`, i.e. a later step silently overriding an
	 * earlier decision -- the shape that produced #1552, where an empty
	 * passphrase decided "open" and the wpa3 token then overwrote it with
	 * WPA3-SAE-with-no-PSK. Stating the precedence in a single expression
	 * leaves no window for one branch to undo another. */
	uint8_t sec = (argc >= 4) ? 2u : ((pass[0] == '\0') ? 0u : 1u);

	strncpy(conn_ssid, ssid, sizeof(conn_ssid) - 1u);
	conn_ssid[sizeof(conn_ssid) - 1u] = '\0';
	strncpy(conn_pass, pass, sizeof(conn_pass) - 1u);
	conn_pass[sizeof(conn_pass) - 1u] = '\0';
	conn_sec                          = sec;
	conn_sh                           = sh;
	conn_pending                      = true;
	shell_print(sh,
	            "connecting to \"%s\" (%s) in background -- result prints here; shell stays live",
	            ssid,
	            (sec == 0u) ? "open" : (sec == 2u ? "wpa3" : "wpa2"));
	return 0;
}

static int cmd_companion_wifi_disconnect(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	alp_status_t s = cc3501e_wifi_disconnect(companion_cc3501e);

	if (s != ALP_OK) {
		shell_error(sh, "disconnect failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "wifi disconnected");
	return 0;
}

static int cmd_companion_wifi_ap(const struct shell *sh, size_t argc, char **argv)
{
	/* Validate the token shape BEFORE any state check -- same ordering
	 * rationale as cmd_companion_wifi_connect() above (#1376): a wrong
	 * argument shape is wrong regardless of whether a companion is bound,
	 * and this guard was the one `wifi ap` was missing (#1480). The 4th
	 * token is the ONLY optional flag, and it must be "wpa3". It used to be
	 * tested with `== 0` and otherwise IGNORED, which made the dangerous
	 * case silent: an UNQUOTED SSID containing a space splits across
	 * argv[1]/argv[2], pushing the real passphrase into argv[3], where it
	 * was dropped without a word -- the AP came up on an SSID fragment as
	 * its PSK, and a mistyped security token silently downgraded WPA3-SAE
	 * to WPA2-PSK. Both are worse than refusing, so an unrecognised token
	 * is now an error. */
	if (argc >= 4 && strcmp(argv[3], "wpa3") != 0) {
		shell_error(sh,
		            "unrecognised argument \"%s\" -- the only optional 4th token is "
		            "\"wpa3\".",
		            argv[3]);
		shell_error(sh,
		            "an SSID or passphrase containing spaces must be quoted: "
		            "wifi ap \"my ssid\" \"my pass\" [wpa3]");
		return -EINVAL;
	}
	/* Same contradiction as `wifi connect`'s, refused the same way and in the
	 * same position (#1552): WPA3-SAE has no open mode, so the "wpa3" token
	 * with an empty passphrase would have started a soft-AP declaring
	 * WPA3-SAE with no PSK. */
	if (argc >= 4 && argv[2][0] == '\0') {
		shell_error(sh, "\"wpa3\" requires a passphrase -- WPA3-SAE has no open mode.");
		shell_error(sh,
		            "for an open soft-AP drop the token: "
		            "wifi ap \"my ssid\"");
		return -EINVAL;
	}
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	const char *ssid = argv[1];
	const char *pass = (argc >= 3) ? argv[2] : "";
	/* Same security rule as `wifi connect`, and the same single-expression
	 * form for the same reason (#1552): no passphrase -> open, a passphrase ->
	 * WPA2-PSK, a trailing "wpa3" token -> WPA3-SAE (validated above). */
	uint8_t sec = (argc >= 4) ? 2u : ((pass[0] == '\0') ? 0u : 1u);

	/* cc3501e_wifi_ap_start() submits WIFI_AP_START exactly ONCE and returns
	 * immediately (#1385) -- it does not block for seconds here the way it
	 * once did; see the function's own comment for why a retry loop around
	 * this opcode is provably unwinnable. ALP_COMPANION_WIFI_CONN_MS is passed
	 * for call-site consistency with the other companion Wi-Fi commands, but
	 * cc3501e_wifi_ap_start() does not currently use it (documented on the
	 * declaration). */
	alp_status_t s =
	    cc3501e_wifi_ap_start(companion_cc3501e, ssid, sec, pass, ALP_COMPANION_WIFI_CONN_MS);

	if (s != ALP_OK) {
		/* #1385: against protocol v4 this is the EXPECTED outcome even for an
		 * AP that came up fine -- WIFI_AP_START acks every submit BUSY and has
		 * no status latch to confirm on, so cc3501e_wifi_ap_start() cannot
		 * report success.  Say "unconfirmed", not "failed": printing a flat
		 * failure for a working AP is the mirror of the false "up" the
		 * dead-phase 0x00 alias used to print. */
		shell_error(sh,
		            "ap start \"%s\" unconfirmed (%d) -- firmware v4 has no AP status latch "
		            "(#1385); check for the SSID out of band",
		            ssid,
		            (int)s);
		return -EIO;
	}
	/* Unreachable against protocol v4 today (see the @warning on
	 * cc3501e_wifi_ap_start()'s declaration: ALP_OK is not a value this call
	 * can currently return) -- kept, not deleted, because it is still the
	 * CORRECT branch for the day a firmware-side AP confirmation channel
	 * lands and makes ALP_OK reachable again; no code change would then be
	 * needed here. */
	shell_print(
	    sh, "ap \"%s\" up (%s)", ssid, (sec == 0u) ? "open" : (sec == 2u ? "wpa3" : "wpa2"));
	return 0;
}

static int cmd_companion_wifi_ap_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	alp_status_t s = cc3501e_wifi_ap_stop(companion_cc3501e);

	if (s != ALP_OK) {
		/* #1553: reaching here does NOT mean the teardown failed.
		 * Bench-measured on E1M-AEN801 (2026-08-18), reproducibly: with no AP
		 * running cc3501e_wifi_ap_stop() returns ALP_OK (the firmware does ack
		 * this opcode), but with an AP ACTUALLY RUNNING it returns
		 * ALP_ERR_TIMEOUT every time -- the teardown takes the bridge through
		 * the radio-down window and the ack is not observed across it. So the
		 * one case a caller cares about is exactly the one that cannot report
		 * its own outcome, the same shape `ap start` above has (#1385).
		 *
		 * "failed" therefore stated more than was known. Say "unconfirmed",
		 * as `ap start` does, and keep the error return -- inconclusive, not
		 * success. */
		shell_error(sh,
		            "ap stop unconfirmed (%d) -- firmware v4 has no AP-stop acknowledgement "
		            "(#1553, same gap as #1385); confirm the SSID is gone out of band",
		            (int)s);
		return -EIO;
	}
	/* Reachable, unlike the matching branch in `ap start`: bench-measured,
	 * cc3501e_wifi_ap_stop() returns ALP_OK when there was no AP to stop. So
	 * this line prints for the no-op case -- which is exactly the case where
	 * "stopped" is unambiguously true. */
	shell_print(sh, "ap stopped");
	return 0;
}

/* Map the connection-state latch to a short label for `wifi status`. */
static const char *companion_wifi_state_name(uint8_t state)
{
	switch (state) {
	case ALP_CC3501E_WIFI_DISCONNECTED:
		return "disconnected";
	case ALP_CC3501E_WIFI_CONNECTING:
		return "connecting";
	case ALP_CC3501E_WIFI_CONNECTED:
		return "connected";
	case ALP_CC3501E_WIFI_CONN_FAILED:
		return "failed";
	default:
		return "unknown";
	}
}

static int cmd_companion_wifi_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}

	alp_cc3501e_wifi_status_t st = { 0 };
	alp_status_t              s  = cc3501e_wifi_status(companion_cc3501e, &st);

	if (s != ALP_OK) {
		shell_error(sh, "status failed (%d)", (int)s);
		return -EIO;
	}

	shell_print(sh, "state: %s", companion_wifi_state_name(st.state));
	if (st.state == ALP_CC3501E_WIFI_CONNECTED) {
		/* Do NOT print st.rssi_dbm: the WIFI_STATUS latch byte is NOT a
		 * measurement.  Every terminal outcome in
		 * cc3501e-bridge-firmware:hal/ti/cc3501e_hw_ti_wifi.c publishes it through
		 * wifi_conn_set(), which always sets it to 0 (the firmware may not read
		 * it there -- a Wlan_Get(WLAN_GET_RSSI) close to associate blocks the
		 * worker), so that byte has only ever held 0.  And 0 dBm is a LEGAL
		 * int8 RSSI, so there is no in-band sentinel the host can test: printed
		 * as-is it is a confident, plausible, unmeasured number (issue #1387).
		 *
		 * Take the RSSI from the only real source instead -- WIFI_GET_RSSI, a
		 * worker-routed radio read, the same one the connect result already
		 * uses successfully (#1382) -- and report unavailable rather than a
		 * number we cannot vouch for when that read fails.  The IP is a third,
		 * separate lease query (WIFI_GET_IP); print it only on a lease.
		 *
		 * Cost: unlike the WIFI_STATUS/WIFI_GET_IP reads above, WIFI_GET_RSSI
		 * is worker-routed (protocol.c: handle_worker_routed), so it always
		 * costs at least a submit -> RESP_ERR_BUSY -> poll round trip, and
		 * cc3501e_wifi_rssi() floors its poll_by_repeat budget to
		 * CC3501E_WIFI_DOWN_WINDOW_MS (10 s) -- on a wedged transport this
		 * command's worst case roughly doubles, from ~10.1 s to ~20.1 s, and
		 * this call runs on the SHELL THREAD.  Gated on
		 * st.state == ALP_CC3501E_WIFI_CONNECTED so no additional
		 * worker-routed radio read is issued while a `wifi connect` is in
		 * flight.  This does NOT make `wifi status` itself cheap during an
		 * in-flight connect (#1377): cc3501e_wifi_status() above can still
		 * poll-by-repeat up to CC3501E_WIFI_DOWN_WINDOW_MS if it lands in the
		 * transport's down-window; that cost is pre-existing and unrelated to
		 * this RSSI change. */
		int8_t       rssi = 0;
		alp_status_t rs   = cc3501e_wifi_rssi(companion_cc3501e, &rssi);
		if (rs == ALP_OK) {
			shell_print(sh, "rssi:  %d dBm", (int)rssi);
		} else {
			shell_print(sh, "rssi:  unavailable (%d)", (int)rs);
		}
		uint8_t      ip[4] = { 0 };
		alp_status_t ips   = cc3501e_wifi_get_ip(companion_cc3501e, ip);
		if (ips == ALP_OK) {
			shell_print(sh, "ip:    %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
		} else {
			shell_print(sh, "ip:    unavailable (%d)", (int)ips);
		}
	} else if (st.state == ALP_CC3501E_WIFI_CONN_FAILED) {
		shell_print(sh, "fail:  %u", (unsigned int)st.fail_reason);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    alp_companion_wifi_subcmds,
    SHELL_CMD_ARG(scan, NULL, "scan for Wi-Fi APs", cmd_companion_wifi_scan, 1, 0),
    SHELL_CMD_ARG(connect,
                  NULL,
                  "connect <ssid> [pass] [wpa3]  -- associate (no pass = open)",
                  cmd_companion_wifi_connect,
                  2,
                  2),
    SHELL_CMD_ARG(disconnect,
                  NULL,
                  "tear down the STA association",
                  cmd_companion_wifi_disconnect,
                  1,
                  0),
    SHELL_CMD_ARG(ap,
                  NULL,
                  "ap <ssid> [pass] [wpa3]  -- start a soft-AP (no pass = open)",
                  cmd_companion_wifi_ap,
                  2,
                  2),
    /* "ap-stop" is a shell command name, not a subtraction expression. */
    /* clang-format off */
    SHELL_CMD_ARG(ap-stop, NULL, "stop the soft-AP", cmd_companion_wifi_ap_stop, 1, 0),
    /* clang-format on */
    SHELL_CMD_ARG(status,
                  NULL,
                  "show connection state + rssi + ip (rssi is a live read, can take ~10s)",
                  cmd_companion_wifi_status,
                  1,
                  0),
    SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((alp, companion),
                 wifi,
                 &alp_companion_wifi_subcmds,
                 "CC3501E Wi-Fi (scan / connect / disconnect / ap / ap-stop / status)",
                 NULL,
                 1,
                 0);
#endif /* !CONFIG_ALP_SDK_V2N_SUPERVISOR */
