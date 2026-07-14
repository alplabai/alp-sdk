/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp companion wifi` -- CC3501E Wi-Fi (scan / connect / disconnect / ap /
 * ap-stop / status), Alif companion only.  Command-group TU of the
 * alp_console_companion.c split (#673 Phase 2): registers onto the
 * (alp, companion) dynamic subcommand set the core TU declares.  Shared
 * companion context + bridge-bus mutex come from
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
		k_mutex_lock(&companion_bus_lock, K_FOREVER);
		alp_status_t s = cc3501e_wifi_connect(
		    companion_cc3501e, conn_ssid, conn_sec, conn_pass, ALP_COMPANION_WIFI_CONN_MS);
		k_mutex_unlock(&companion_bus_lock);

		if (s == ALP_OK) {
			int8_t rssi = 0;
			k_mutex_lock(&companion_bus_lock, K_FOREVER);
			(void)cc3501e_wifi_rssi(companion_cc3501e, &rssi);
			k_mutex_unlock(&companion_bus_lock);
			shell_print(conn_sh, "wifi connected \"%s\"  rssi=%d dBm", conn_ssid, (int)rssi);
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
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_wifi_scan(
	    companion_cc3501e, recs, ALP_COMPANION_WIFI_SCAN_MAX, &n, ALP_COMPANION_WIFI_SCAN_MS);
	k_mutex_unlock(&companion_bus_lock);

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
	/* No passphrase -> open; a passphrase -> WPA2-PSK (the common case).  A
	 * trailing "wpa3" token forces WPA3-SAE. */
	uint8_t sec = (pass[0] == '\0') ? 0u : 1u;
	if (argc >= 4 && strcmp(argv[3], "wpa3") == 0) {
		sec = 2u;
	}

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
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_wifi_disconnect(companion_cc3501e);
	k_mutex_unlock(&companion_bus_lock);

	if (s != ALP_OK) {
		shell_error(sh, "disconnect failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "wifi disconnected");
	return 0;
}

static int cmd_companion_wifi_ap(const struct shell *sh, size_t argc, char **argv)
{
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	const char *ssid = argv[1];
	const char *pass = (argc >= 3) ? argv[2] : "";
	/* Same security rule as `wifi connect`: no passphrase -> open, a
	 * passphrase -> WPA2-PSK, a trailing "wpa3" token -> WPA3-SAE. */
	uint8_t sec = (pass[0] == '\0') ? 0u : 1u;
	if (argc >= 4 && strcmp(argv[3], "wpa3") == 0) {
		sec = 2u;
	}

	/* AP bring-up blocks for seconds in the firmware; run it under the bus
	 * lock so a concurrent BLE/GPIO op can't interleave on the bridge. */
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s =
	    cc3501e_wifi_ap_start(companion_cc3501e, ssid, sec, pass, ALP_COMPANION_WIFI_CONN_MS);
	k_mutex_unlock(&companion_bus_lock);

	if (s != ALP_OK) {
		shell_error(sh, "ap start \"%s\" failed (%d)", ssid, (int)s);
		return -EIO;
	}
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
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_wifi_ap_stop(companion_cc3501e);
	k_mutex_unlock(&companion_bus_lock);

	if (s != ALP_OK) {
		shell_error(sh, "ap stop failed (%d)", (int)s);
		return -EIO;
	}
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
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_wifi_status(companion_cc3501e, &st);
	k_mutex_unlock(&companion_bus_lock);

	if (s != ALP_OK) {
		shell_error(sh, "status failed (%d)", (int)s);
		return -EIO;
	}

	shell_print(sh, "state: %s", companion_wifi_state_name(st.state));
	if (st.state == ALP_CC3501E_WIFI_CONNECTED) {
		/* rssi_dbm is valid only when associated; the IP is a separate lease
		 * query (WIFI_GET_IP) -- print it only if the firmware has a lease. */
		shell_print(sh, "rssi:  %d dBm", (int)st.rssi_dbm);
		uint8_t ip[4] = { 0 };
		k_mutex_lock(&companion_bus_lock, K_FOREVER);
		alp_status_t ips = cc3501e_wifi_get_ip(companion_cc3501e, ip);
		k_mutex_unlock(&companion_bus_lock);
		if (ips == ALP_OK) {
			shell_print(sh, "ip:    %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
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
                  "show connection state + rssi + ip",
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
