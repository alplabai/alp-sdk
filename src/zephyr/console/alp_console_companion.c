/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp companion` -- one portable command surface over two different
 * companions: the GD32 supervisor singleton on V2N, an app-registered
 * CC3501E on Alif.  ver/ping are portable; gpio is V2N-only.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <alp/console.h>
#include <alp/peripheral.h>

#include "alp_console.h"

#if IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
#include "../v2n_supervisor.h"
#endif

/* ---- Alif: app-registered CC3501E handle -------------------------------- */
static cc3501e_t *companion_cc3501e;

void alp_console_companion_set(cc3501e_t *ctx)
{
	companion_cc3501e = ctx;
}

#if !IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
/* Bridge-bus serialisation (Alif CC3501E).  The shell thread (companion command
 * bodies) and the async-connect result thread BOTH drive the inter-chip bridge,
 * and cc3501e_request is not internally locked -- two concurrent transactions
 * would interleave on the SPI bus and desync the link.  Every bridge
 * access from this file (each command body + the result thread's status poll)
 * takes this mutex. */
K_MUTEX_DEFINE(companion_bus_lock);
#endif

static int cmd_companion_ver(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

#if IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
	gd32g553_t  *ctx;
	alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);

	if (s != ALP_OK) {
		shell_error(sh, "supervisor acquire failed (%d)", (int)s);
		return -EIO;
	}

	gd32g553_version_t v;

	s = gd32g553_get_version(ctx, &v);
	alp_z_v2n_supervisor_release();
	if (s != ALP_OK) {
		shell_error(sh, "get_version failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "GD32 supervisor fw v%u.%u.%u", v.major, v.minor, v.patch);
	return 0;
#else
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered (call alp_console_companion_set)");
		return -ENODEV;
	}

	uint16_t ver = 0;
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_get_version(companion_cc3501e, &ver);
	k_mutex_unlock(&companion_bus_lock);

	if (s != ALP_OK) {
		shell_error(sh, "get_version failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "CC3501E protocol v%u", (unsigned int)ver);
	return 0;
#endif
}

static int cmd_companion_ping(const struct shell *sh, size_t argc, char **argv)
{
#if IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	gd32g553_t  *ctx;
	alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);

	if (s != ALP_OK) {
		shell_error(sh, "supervisor acquire failed (%d)", (int)s);
		return -EIO;
	}
	s = gd32g553_ping(ctx);
	alp_z_v2n_supervisor_release();
	shell_print(sh, "ping %s", s == ALP_OK ? "OK" : "FAIL");
	return s == ALP_OK ? 0 : -EIO;
#else
	/* CC3501E has no bare PING wrapper; a GET_VERSION round-trip is the
	 * liveness probe (this matches how on-silicon bring-up proved the link). */
	return cmd_companion_ver(sh, argc, argv);
#endif
}

#if !IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
/* ---- CC3501E Wi-Fi / BLE (Alif companion) ------------------------------- */
#define ALP_COMPANION_WIFI_SCAN_MAX 16u
#define ALP_COMPANION_WIFI_SCAN_MS  30000u
#define ALP_COMPANION_WIFI_CONN_MS  35000u
#define ALP_COMPANION_BLE_MS        30000u
#define ALP_COMPANION_BLE_SCAN_MAX  16u

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

static int cmd_companion_ble_enable(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_ble_enable(companion_cc3501e, ALP_COMPANION_BLE_MS);
	k_mutex_unlock(&companion_bus_lock);

	if (s == ALP_ERR_NOT_READY) {
		shell_warn(sh, "BLE not built into the CC3501E firmware (needs the -Ble image)");
		return -ENOTSUP;
	}
	if (s != ALP_OK) {
		shell_error(sh, "ble enable failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "BLE controller + NimBLE host up");
	return 0;
}

static int cmd_companion_ble_scan(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}

	static cc3501e_ble_scan_record_t recs[ALP_COMPANION_BLE_SCAN_MAX];
	size_t                           n = 0;
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_ble_scan(
	    companion_cc3501e, recs, ALP_COMPANION_BLE_SCAN_MAX, &n, ALP_COMPANION_BLE_MS);
	k_mutex_unlock(&companion_bus_lock);

	if (s == ALP_ERR_NOT_READY) {
		shell_warn(sh, "BLE not enabled -- run `alp companion ble enable` first");
		return -ENOTSUP;
	}
	if (s != ALP_OK) {
		shell_error(sh, "ble scan failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "%u device(s):", (unsigned int)n);
	for (size_t i = 0; i < n; i++) {
		/* BLE addresses are little-endian on the wire; print MSB-first (the
		 * conventional xx:xx:xx:xx:xx:xx display order). */
		shell_print(sh,
		            "  %02x:%02x:%02x:%02x:%02x:%02x  %4d dBm  %s",
		            recs[i].addr[5],
		            recs[i].addr[4],
		            recs[i].addr[3],
		            recs[i].addr[2],
		            recs[i].addr[1],
		            recs[i].addr[0],
		            (int)recs[i].rssi_dbm,
		            (recs[i].name[0] != '\0') ? recs[i].name : "(no name)");
	}
	return 0;
}

/* ---- bridge throughput bench ------------------------------------------- */
/* NOTE: the old `settle` tuning command was removed with the fixed req/reply
 * settle gaps -- the current cc3501e driver rendezvouses on the READY line
 * (see cc3501e.c / the `ready` gpio) instead of a tunable fixed delay, so
 * there is no longer a settle knob to get/set. */
static int cmd_companion_bench(const struct shell *sh, size_t argc, char **argv)
{
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	unsigned long n = 200u;
	if (argc >= 2 && (alp_console_parse_ulong(argv[1], &n) != 0 || n == 0u || n > 100000u)) {
		shell_error(sh, "usage: alp companion bench [1..100000 ops]");
		return -EINVAL;
	}
	uint16_t     ver   = 0;
	unsigned int fails = 0;
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	int64_t t0 = k_uptime_get();
	for (unsigned long i = 0; i < n; i++) {
		if (cc3501e_get_version(companion_cc3501e, &ver) != ALP_OK) {
			fails++;
		}
	}
	int64_t dt = k_uptime_get() - t0;
	k_mutex_unlock(&companion_bus_lock);
	if (dt <= 0) {
		dt = 1;
	}
	shell_print(sh,
	            "bench: %lu GET_VERSION ops in %lld ms = %lld us/op, %lld ops/s (fails=%u)",
	            n,
	            (long long)dt,
	            (long long)((dt * 1000) / (long long)n),
	            (long long)(((long long)n * 1000) / dt),
	            fails);
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
    SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(alp_companion_ble_subcmds,
                               SHELL_CMD_ARG(enable,
                                             NULL,
                                             "enable the BLE controller + NimBLE host",
                                             cmd_companion_ble_enable,
                                             1,
                                             0),
                               SHELL_CMD_ARG(scan,
                                             NULL,
                                             "scan for BLE advertisers (needs `ble enable` first)",
                                             cmd_companion_ble_scan,
                                             1,
                                             0),
                               SHELL_SUBCMD_SET_END);
#endif /* !CONFIG_ALP_SDK_V2N_SUPERVISOR */

#if IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
static int cmd_companion_gpio_read(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	unsigned long pin;

	if (alp_console_parse_ulong(argv[1], &pin) != 0 || pin > 31) {
		shell_error(sh, "usage: alp companion gpio read <0..31>");
		return -EINVAL;
	}

	gd32g553_t  *ctx;
	alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);

	if (s != ALP_OK) {
		shell_error(sh, "supervisor acquire failed (%d)", (int)s);
		return -EIO;
	}

	uint32_t levels = 0;

	s = gd32g553_gpio_read(ctx, BIT(pin), &levels);
	alp_z_v2n_supervisor_release();
	if (s != ALP_OK) {
		shell_error(sh, "gpio_read failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "companion pin %lu = %d", pin, (levels & BIT(pin)) ? 1 : 0);
	return 0;
}

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)
static int cmd_companion_gpio_write(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	unsigned long pin;
	unsigned long val;

	if (alp_console_parse_ulong(argv[1], &pin) != 0 || pin > 31 ||
	    alp_console_parse_ulong(argv[2], &val) != 0 || val > 1) {
		shell_error(sh, "usage: alp companion gpio write <0..31> <0|1>");
		return -EINVAL;
	}

	gd32g553_t  *ctx;
	alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);

	if (s != ALP_OK) {
		shell_error(sh, "supervisor acquire failed (%d)", (int)s);
		return -EIO;
	}

	s = gd32g553_gpio_write(ctx, BIT(pin), val ? BIT(pin) : 0);
	alp_z_v2n_supervisor_release();
	if (s != ALP_OK) {
		shell_error(sh, "gpio_write failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "companion pin %lu <- %lu", pin, val);
	return 0;
}
#endif /* CONFIG_ALP_SDK_CONSOLE_UNSAFE */

SHELL_STATIC_SUBCMD_SET_CREATE(
    alp_companion_gpio_subcmds,
    SHELL_CMD_ARG(
        read, NULL, "read <0..31>  -- sample a GD32 GPIO pin", cmd_companion_gpio_read, 2, 0),
#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)
    SHELL_CMD_ARG(write,
                  NULL,
                  "write <0..31> <0|1>  -- drive a GD32 GPIO pin (UNSAFE)",
                  cmd_companion_gpio_write,
                  3,
                  0),
#endif
    SHELL_SUBCMD_SET_END);
#endif /* CONFIG_ALP_SDK_V2N_SUPERVISOR */

SHELL_STATIC_SUBCMD_SET_CREATE(
    alp_companion_subcmds,
    SHELL_CMD_ARG(ver, NULL, "companion firmware version", cmd_companion_ver, 1, 0),
    SHELL_CMD_ARG(ping, NULL, "liveness round-trip", cmd_companion_ping, 1, 0),
#if IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
    SHELL_CMD(gpio, &alp_companion_gpio_subcmds, "companion GPIO (V2N only)", NULL),
#else
    SHELL_CMD(wifi, &alp_companion_wifi_subcmds, "CC3501E Wi-Fi (scan / connect)", NULL),
    SHELL_CMD(ble, &alp_companion_ble_subcmds, "CC3501E BLE (enable / scan)", NULL),
    SHELL_CMD_ARG(
        bench, NULL, "bench [n] -- time n GET_VERSION round-trips", cmd_companion_bench, 1, 1),
#endif
    SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD(
    (alp), companion, &alp_companion_subcmds, "Companion chip bridge (GD32 / CC3501E)", NULL, 1, 0);
