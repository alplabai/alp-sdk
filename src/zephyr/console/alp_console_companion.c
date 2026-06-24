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
 * `wifi connect` SUBMITS the association and returns immediately; the firmware runs
 * it in the background (holding the bridge READY line BUSY).  This low-rate thread
 * watches the raw READY line (cc3501e_bus_ready) and, once the bridge is ready
 * again, reads the non-blocking status latch (cc3501e_wifi_status) and prints the
 * result -- so the shell stays fully responsive during the ~15 s association. */
#define ALP_COMPANION_CONN_POLL_MS     200
#define ALP_COMPANION_CONN_DEADLINE_MS 45000

static volatile bool       conn_pending;  /* an async connect is awaiting its result */
static const struct shell *conn_sh;        /* shell to print the async result on      */
static char                conn_ssid[33];  /* SSID we are connecting to (for the print) */
static int64_t             conn_deadline;  /* k_uptime_get() ms after which we give up  */

static const char *conn_fail_name(uint8_t reason)
{
	switch (reason) {
	case ALP_CC3501E_WIFI_FAIL_TIMEOUT:
		return "timeout (no connect event)";
	case ALP_CC3501E_WIFI_FAIL_REJECTED:
		return "rejected (auth/assoc)";
	case ALP_CC3501E_WIFI_FAIL_KICK:
		return "radio kick failed";
	default:
		return "unknown";
	}
}

/* Report + stop polling when the connect attempt is past its deadline. */
static bool conn_check_deadline(const char *why)
{
	if (k_uptime_get() < conn_deadline) {
		return false;
	}
	shell_warn(conn_sh, "wifi connect to \"%s\": timed out (%s)", conn_ssid, why);
	conn_pending = false;
	return true;
}

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

		/* Only clock the bridge when the CC35 is READY: while it runs the
		 * association its SPI-slave DMA is dead and the READY line (GPIO17 -> P2_6,
		 * a rev-1 wire) is LOW.  Reading the raw line first avoids driving a
		 * transaction into a dead slave (which would desync the link). */
		if (!cc3501e_bus_ready()) {
			(void)conn_check_deadline("still associating");
			continue;
		}

		alp_cc3501e_wifi_status_t st;
		k_mutex_lock(&companion_bus_lock, K_FOREVER);
		alp_status_t s = cc3501e_wifi_status(companion_cc3501e, &st);
		k_mutex_unlock(&companion_bus_lock);

		if (s != ALP_OK) {
			/* BUSY (raced the gate) / IO (bridge re-arming) -- retry next tick. */
			(void)conn_check_deadline("bridge busy");
			continue;
		}
		if (st.state == ALP_CC3501E_WIFI_CONNECTING) {
			(void)conn_check_deadline("still associating");
			continue; /* association still running */
		}
		if (st.state == ALP_CC3501E_WIFI_CONNECTED) {
			shell_print(
			    conn_sh, "wifi connected \"%s\"  rssi=%d dBm", conn_ssid, (int)st.rssi_dbm);
		} else if (st.state == ALP_CC3501E_WIFI_CONN_FAILED) {
			shell_error(conn_sh,
			            "wifi connect to \"%s\" failed: %s",
			            conn_ssid,
			            conn_fail_name(st.fail_reason));
		} else { /* DISCONNECTED: the submit never started an attempt (should not happen) */
			shell_warn(conn_sh, "wifi connect to \"%s\": not started", conn_ssid);
		}
		conn_pending = false;
	}
}

/* Low priority (7, like the RGB thread); the shell + companion commands run above
 * it.  Idles on conn_pending == false, so it costs nothing until a connect submits. */
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

	/* SUBMIT the association and RETURN -- do NOT block the shell.  The firmware runs
	 * the connect in the background (READY held BUSY); the companion_conn_thread polls
	 * the status latch once the bridge is ready again and prints the result here. */
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_wifi_connect_async(companion_cc3501e, ssid, sec, pass);
	k_mutex_unlock(&companion_bus_lock);
	if (s != ALP_OK) {
		shell_error(sh, "connect submit failed (%d)", (int)s);
		return -EIO;
	}

	/* Arm the background result poll (hand-off to companion_conn_thread). */
	strncpy(conn_ssid, ssid, sizeof(conn_ssid) - 1u);
	conn_ssid[sizeof(conn_ssid) - 1u] = '\0';
	conn_sh                           = sh;
	conn_deadline                     = k_uptime_get() + ALP_COMPANION_CONN_DEADLINE_MS;
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

/* ---- bridge throughput tuning (settle + bench) -------------------------- */
/* Two independently-tunable settle levers (see cc3501e.c):
 *   - req-payload settle  (LOAD-BEARING, keep ~200us): the req-header->req-payload gap
 *   - reply settle        (throughput-sensitive, sweep toward 0): the pre-reply gap
 * Usage:
 *   settle              -- print both
 *   settle req <us>     -- set the req-payload settle (payload requests; keep ~200us)
 *   settle reply <us>   -- set the reply settle (header-only common case; sweep to 0) */
static int cmd_companion_settle(const struct shell *sh, size_t argc, char **argv)
{
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	if (argc >= 3) {
		unsigned long us;
		if (alp_console_parse_ulong(argv[2], &us) != 0 || us > 100000u) {
			shell_error(sh, "usage: alp companion settle [req|reply] [0..100000 us]");
			return -EINVAL;
		}
		if (strcmp(argv[1], "req") == 0) {
			cc3501e_set_req_payload_settle_us((uint32_t)us);
		} else if (strcmp(argv[1], "reply") == 0) {
			cc3501e_set_reply_settle_us((uint32_t)us);
		} else {
			shell_error(sh, "usage: alp companion settle [req|reply] [0..100000 us]");
			return -EINVAL;
		}
	} else if (argc == 2) {
		shell_error(sh, "usage: alp companion settle [req|reply] [0..100000 us]");
		return -EINVAL;
	}
	shell_print(sh,
	            "bridge settle: req-payload = %u us, reply = %u us",
	            (unsigned int)cc3501e_get_req_payload_settle_us(),
	            (unsigned int)cc3501e_get_reply_settle_us());
	return 0;
}

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
	            "bench: %lu GET_VERSION ops in %lld ms = %lld us/op, %lld ops/s (fails=%u, "
	            "settle req=%u reply=%u us)",
	            n,
	            (long long)dt,
	            (long long)((dt * 1000) / (long long)n),
	            (long long)(((long long)n * 1000) / dt),
	            fails,
	            (unsigned int)cc3501e_get_req_payload_settle_us(),
	            (unsigned int)cc3501e_get_reply_settle_us());
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

SHELL_STATIC_SUBCMD_SET_CREATE(
    alp_companion_ble_subcmds,
    SHELL_CMD_ARG(
        enable, NULL, "enable the BLE controller + NimBLE host", cmd_companion_ble_enable, 1, 0),
    SHELL_CMD_ARG(scan, NULL, "scan for BLE advertisers (needs `ble enable` first)",
                  cmd_companion_ble_scan, 1, 0),
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
    SHELL_CMD_ARG(settle,
                  NULL,
                  "settle [req|reply <us>] -- get/set bridge settle (req-payload + reply)",
                  cmd_companion_settle,
                  1,
                  2),
    SHELL_CMD_ARG(bench, NULL, "bench [n] -- time n GET_VERSION round-trips", cmd_companion_bench, 1, 1),
#endif
    SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD(
    (alp), companion, &alp_companion_subcmds, "Companion chip bridge (GD32 / CC3501E)", NULL, 1, 0);
