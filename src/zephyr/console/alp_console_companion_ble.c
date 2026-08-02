/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp companion ble` -- CC3501E BLE: enable / disable / scan / scan-stop /
 * adv / adv-stop / connect / disconnect / gatt (register / read / write /
 * notify), Alif companion only.  Command-group TU of the
 * alp_console_companion.c split (#673 Phase 2): registers onto the
 * (alp, companion) dynamic subcommand set the core TU declares.  Shared
 * companion context + bridge-bus mutex come from
 * alp_console_companion_internal.h.
 *
 * NOTE: GATT async notifications (the CC3501E's inbound write-req events,
 * EVT_BLE_GATT_WRITE_REQ 0x3F) need the async-event path (task #17, in the
 * core TU), which is not wired on this HW rev -- these subcommands issue
 * the outbound commands only; there is no async delivery of peer-initiated
 * writes yet.
 */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <alp/ext/cc3501e/console.h>
#include <alp/peripheral.h>

#include "alp_console.h"
#include "alp_console_companion_internal.h"

#if !IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
/* ---- CC3501E BLE (Alif companion) --------------------------------------- */
#define ALP_COMPANION_BLE_MS              30000u
#define ALP_COMPANION_BLE_SCAN_MAX        16u
#define ALP_COMPANION_BLE_ADV_INTERVAL_MS 100u
#define ALP_COMPANION_BLE_GATT_MAX        64u /* CLI descriptor / value byte cap */

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

/* Parse "aa:bb:cc:dd:ee:ff" (conventional MSB-first display order) into the
 * wire's LSB-first order (addr[0] = rightmost octet), so it round-trips with
 * cc3501e_ble_scan_record_t::addr.  Returns 0 on success, -1 on a malformed
 * address. */
static int companion_parse_ble_addr(const char *s, uint8_t addr[6])
{
	for (int i = 0; i < 6; i++) {
		char         *end = NULL;
		unsigned long v   = strtoul(s, &end, 16);
		if (end == s || v > 0xFFu) return -1;
		addr[5 - i] = (uint8_t)v;
		if (i < 5) {
			if (*end != ':') return -1;
			s = end + 1;
		} else if (*end != '\0') {
			return -1;
		}
	}
	return 0;
}

/* Parse a contiguous hex string ("0011aabb") into bytes (two nibbles/byte).
 * Rejects an empty string, an odd length, a non-hex char, or an overflow of
 * @cap.  Returns 0 on success, -1 otherwise. */
static int companion_parse_hex(const char *s, uint8_t *out, size_t cap, size_t *out_len)
{
	size_t n = strlen(s);
	if (n == 0u || (n % 2u) != 0u || (n / 2u) > cap) return -1;
	for (size_t i = 0; i < n; i += 2u) {
		char          oct[3] = { s[i], s[i + 1u], '\0' };
		char         *end    = NULL;
		unsigned long v      = strtoul(oct, &end, 16);
		if (end != oct + 2) return -1;
		out[i / 2u] = (uint8_t)v;
	}
	*out_len = n / 2u;
	return 0;
}

static int cmd_companion_ble_disable(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_ble_disable(companion_cc3501e, ALP_COMPANION_BLE_MS);
	k_mutex_unlock(&companion_bus_lock);
	if (s != ALP_OK) {
		shell_error(sh, "ble disable failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "BLE disabled");
	return 0;
}

static int cmd_companion_ble_adv(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	/* Start connectable advertising at a fixed interval with no adv-data; a
	 * richer adv-data payload is left to a firmware profile, not the CLI. */
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_ble_adv_start(companion_cc3501e,
	                                       true,
	                                       ALP_COMPANION_BLE_ADV_INTERVAL_MS,
	                                       ALP_COMPANION_BLE_ADV_INTERVAL_MS,
	                                       NULL,
	                                       0u,
	                                       ALP_COMPANION_BLE_MS);
	k_mutex_unlock(&companion_bus_lock);
	if (s != ALP_OK) {
		shell_error(sh, "ble adv start failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "advertising (connectable, %u ms)", ALP_COMPANION_BLE_ADV_INTERVAL_MS);
	return 0;
}

static int cmd_companion_ble_adv_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_ble_adv_stop(companion_cc3501e, ALP_COMPANION_BLE_MS);
	k_mutex_unlock(&companion_bus_lock);
	if (s != ALP_OK) {
		shell_error(sh, "ble adv stop failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "advertising stopped");
	return 0;
}

static int cmd_companion_ble_scan_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_ble_scan_stop(companion_cc3501e, ALP_COMPANION_BLE_MS);
	k_mutex_unlock(&companion_bus_lock);
	if (s != ALP_OK) {
		shell_error(sh, "ble scan stop failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "scan stopped");
	return 0;
}

static int cmd_companion_ble_connect(const struct shell *sh, size_t argc, char **argv)
{
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	uint8_t addr[6];
	if (companion_parse_ble_addr(argv[1], addr) != 0) {
		shell_error(sh, "usage: alp companion ble connect <aa:bb:cc:dd:ee:ff> [random]");
		return -EINVAL;
	}
	uint8_t addr_type = (argc >= 3 && strcmp(argv[2], "random") == 0) ? 1u : 0u;
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_ble_connect(companion_cc3501e, addr, addr_type, ALP_COMPANION_BLE_MS);
	k_mutex_unlock(&companion_bus_lock);
	if (s != ALP_OK) {
		shell_error(sh, "ble connect failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "connected");
	return 0;
}

static int cmd_companion_ble_disconnect(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_ble_disconnect(companion_cc3501e, ALP_COMPANION_BLE_MS);
	k_mutex_unlock(&companion_bus_lock);
	if (s != ALP_OK) {
		shell_error(sh, "ble disconnect failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "disconnected");
	return 0;
}

static int cmd_companion_ble_gatt_register(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	uint8_t desc[ALP_COMPANION_BLE_GATT_MAX];
	size_t  len = 0;
	if (companion_parse_hex(argv[1], desc, sizeof(desc), &len) != 0) {
		shell_error(sh, "usage: alp companion ble gatt register <hexbytes> (opaque descriptor)");
		return -EINVAL;
	}
	uint16_t handles[ALP_CC3501E_BLE_GATT_MAX_CHARS];
	size_t   num_handles = 0;
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_ble_gatt_register(companion_cc3501e,
	                                           desc,
	                                           len,
	                                           handles,
	                                           ARRAY_SIZE(handles),
	                                           &num_handles,
	                                           ALP_COMPANION_BLE_MS);
	k_mutex_unlock(&companion_bus_lock);
	if (s != ALP_OK) {
		shell_error(sh, "ble gatt register failed (%d)", (int)s);
		return -EIO;
	}
	shell_fprintf(
	    sh, SHELL_NORMAL, "GATT table registered (%u bytes), handles:", (unsigned int)len);
	for (size_t i = 0; i < num_handles; i++) {
		shell_fprintf(sh, SHELL_NORMAL, " 0x%04x", (unsigned int)handles[i]);
	}
	shell_fprintf(sh, SHELL_NORMAL, "\n");
	return 0;
}

static int cmd_companion_ble_gatt_read(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	unsigned long handle;
	if (alp_console_parse_ulong(argv[1], &handle) != 0 || handle > 0xFFFFu) {
		shell_error(sh, "usage: alp companion ble gatt read <handle>");
		return -EINVAL;
	}
	uint8_t val[ALP_COMPANION_BLE_GATT_MAX];
	size_t  got = 0;
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_ble_gatt_read(
	    companion_cc3501e, (uint16_t)handle, val, sizeof(val), &got, ALP_COMPANION_BLE_MS);
	k_mutex_unlock(&companion_bus_lock);
	if (s != ALP_OK) {
		shell_error(sh, "ble gatt read failed (%d)", (int)s);
		return -EIO;
	}
	shell_fprintf(sh, SHELL_NORMAL, "handle 0x%04x =", (unsigned int)handle);
	for (size_t i = 0; i < got; i++) {
		shell_fprintf(sh, SHELL_NORMAL, " %02x", val[i]);
	}
	shell_fprintf(sh, SHELL_NORMAL, " (%u bytes)\n", (unsigned int)got);
	return 0;
}

/* Shared body for `gatt write` (BLE_GATT_WRITE) and `gatt notify`
 * (BLE_GATT_NOTIFY): both take <handle> <hexbytes> and differ only in the
 * driver call. */
static int companion_ble_gatt_send(const struct shell *sh, char **argv, bool notify)
{
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	unsigned long handle;
	if (alp_console_parse_ulong(argv[1], &handle) != 0 || handle > 0xFFFFu) {
		shell_error(sh,
		            "usage: alp companion ble gatt %s <handle> <hexbytes>",
		            notify ? "notify" : "write");
		return -EINVAL;
	}
	uint8_t val[ALP_COMPANION_BLE_GATT_MAX];
	size_t  len = 0;
	if (companion_parse_hex(argv[2], val, sizeof(val), &len) != 0) {
		shell_error(sh,
		            "usage: alp companion ble gatt %s <handle> <hexbytes>",
		            notify ? "notify" : "write");
		return -EINVAL;
	}
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = notify
	                     ? cc3501e_ble_gatt_notify(
	                           companion_cc3501e, (uint16_t)handle, val, len, ALP_COMPANION_BLE_MS)
	                     : cc3501e_ble_gatt_write(
	                           companion_cc3501e, (uint16_t)handle, val, len, ALP_COMPANION_BLE_MS);
	k_mutex_unlock(&companion_bus_lock);
	if (s != ALP_OK) {
		shell_error(sh, "ble gatt %s failed (%d)", notify ? "notify" : "write", (int)s);
		return -EIO;
	}
	shell_print(sh,
	            "GATT %s handle 0x%04x (%u bytes)",
	            notify ? "notify" : "write",
	            (unsigned int)handle,
	            (unsigned int)len);
	return 0;
}

static int cmd_companion_ble_gatt_write(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	return companion_ble_gatt_send(sh, argv, false);
}

static int cmd_companion_ble_gatt_notify(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	return companion_ble_gatt_send(sh, argv, true);
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    alp_companion_ble_gatt_subcmds,
    /* "register" is a shell command name here, not the C storage-class keyword;
     * guard it from clang-format's keyword handling like the hyphenated names. */
    /* clang-format off */
    SHELL_CMD_ARG(register,
                  NULL,
                  "register <hexbytes>  -- register an opaque GATT attribute table",
                  cmd_companion_ble_gatt_register,
                  2,
                  0),
    /* clang-format on */
    SHELL_CMD_ARG(read,
                  NULL,
                  "read <handle>  -- read a GATT attribute value",
                  cmd_companion_ble_gatt_read,
                  2,
                  0),
    SHELL_CMD_ARG(write,
                  NULL,
                  "write <handle> <hexbytes>  -- write a GATT attribute value",
                  cmd_companion_ble_gatt_write,
                  3,
                  0),
    SHELL_CMD_ARG(notify,
                  NULL,
                  "notify <handle> <hexbytes>  -- send a GATT notification",
                  cmd_companion_ble_gatt_notify,
                  3,
                  0),
    SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
    alp_companion_ble_subcmds,
    SHELL_CMD_ARG(enable,
                  NULL,
                  "enable the BLE controller + NimBLE host",
                  cmd_companion_ble_enable,
                  1,
                  0),
    SHELL_CMD_ARG(disable,
                  NULL,
                  "disable the BLE controller + NimBLE host",
                  cmd_companion_ble_disable,
                  1,
                  0),
    SHELL_CMD_ARG(scan,
                  NULL,
                  "scan for BLE advertisers (needs `ble enable` first)",
                  cmd_companion_ble_scan,
                  1,
                  0),
    /* "scan-stop" / "adv-stop" are shell command names, not subtraction. */
    /* clang-format off */
    SHELL_CMD_ARG(scan-stop, NULL, "stop an in-progress scan", cmd_companion_ble_scan_stop, 1, 0),
    /* clang-format on */
    SHELL_CMD_ARG(adv,
                  NULL,
                  "start connectable advertising (fixed interval)",
                  cmd_companion_ble_adv,
                  1,
                  0),
    /* clang-format off */
    SHELL_CMD_ARG(adv-stop, NULL, "stop advertising", cmd_companion_ble_adv_stop, 1, 0),
    /* clang-format on */
    SHELL_CMD_ARG(connect,
                  NULL,
                  "connect <aa:bb:cc:dd:ee:ff> [random]  -- central connect to a peer",
                  cmd_companion_ble_connect,
                  2,
                  1),
    SHELL_CMD_ARG(disconnect,
                  NULL,
                  "drop the active BLE connection",
                  cmd_companion_ble_disconnect,
                  1,
                  0),
    SHELL_CMD(gatt,
              &alp_companion_ble_gatt_subcmds,
              "GATT (register / read / write / notify)",
              NULL),
    SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((alp, companion),
                 ble,
                 &alp_companion_ble_subcmds,
                 "CC3501E BLE (enable / disable / scan / scan-stop / adv / adv-stop / connect / "
                 "disconnect / gatt)",
                 NULL,
                 1,
                 0);
#endif /* !CONFIG_ALP_SDK_V2N_SUPERVISOR */
