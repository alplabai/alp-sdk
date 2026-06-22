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

	uint16_t     ver = 0;
	alp_status_t s   = cc3501e_get_version(companion_cc3501e, &ver);

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
#define ALP_COMPANION_WIFI_CONN_MS  20000u
#define ALP_COMPANION_BLE_MS        30000u
#define ALP_COMPANION_BLE_SCAN_MAX  16u

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
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	const char *ssid = argv[1];
	const char *pass = (argc >= 3) ? argv[2] : "";
	/* No passphrase -> open; a passphrase -> WPA2-PSK (the common case).  A
	 * trailing "wpa3" token forces WPA3-SAE. */
	uint8_t sec = (pass[0] == '\0') ? 0u : 1u;
	if (argc >= 4 && strcmp(argv[3], "wpa3") == 0) {
		sec = 2u;
	}

	shell_print(
	    sh, "connecting to \"%s\" (%s)...", ssid, (sec == 0u) ? "open" : (sec == 2u ? "wpa3" : "wpa2"));
	alp_status_t s =
	    cc3501e_wifi_connect(companion_cc3501e, ssid, sec, pass, ALP_COMPANION_WIFI_CONN_MS);
	if (s != ALP_OK) {
		shell_error(sh, "connect failed (%d)", (int)s);
		return -EIO;
	}

	int8_t  rssi  = 0;
	uint8_t ip[4] = { 0 };
	(void)cc3501e_wifi_rssi(companion_cc3501e, &rssi);
	shell_print(sh, "connected  rssi=%d dBm", (int)rssi);
	if (cc3501e_wifi_get_ip(companion_cc3501e, ip) == ALP_OK) {
		shell_print(sh, "ip %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
	}
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
	alp_status_t s = cc3501e_ble_enable(companion_cc3501e, ALP_COMPANION_BLE_MS);

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
	alp_status_t                     s = cc3501e_ble_scan(
        companion_cc3501e, recs, ALP_COMPANION_BLE_SCAN_MAX, &n, ALP_COMPANION_BLE_MS);

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
#endif
    SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD(
    (alp), companion, &alp_companion_subcmds, "Companion chip bridge (GD32 / CC3501E)", NULL, 1, 0);
