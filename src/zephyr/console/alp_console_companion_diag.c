/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp companion diag` -- CC3501E diagnostics (info / stats / loglevel),
 * Alif companion only.  Command-group TU of the alp_console_companion.c
 * split (#673 Phase 2): registers onto the (alp, companion) dynamic
 * subcommand set the core TU declares.  Shared companion context +
 * bridge-bus mutex come from alp_console_companion_internal.h.
 */
#include <errno.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <alp/ext/cc3501e/console.h>
#include <alp/peripheral.h>

#include "alp_console.h"
#include "alp_console_companion_internal.h"

#if !IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
/* ---- CC3501E diagnostics (Alif companion) -------------------------------- */

/* Map the reset-cause / role codes to short labels for `diag info`. */
static const char *companion_reset_cause_name(uint8_t cause)
{
	switch (cause) {
	case ALP_CC3501E_RESET_POWER_ON:
		return "power-on";
	case ALP_CC3501E_RESET_NRST_PIN:
		return "nrst-pin";
	case ALP_CC3501E_RESET_SOFT:
		return "soft";
	case ALP_CC3501E_RESET_WATCHDOG:
		return "watchdog";
	case ALP_CC3501E_RESET_BROWNOUT:
		return "brownout";
	case ALP_CC3501E_RESET_BLE_STACK:
		return "ble-panic";
	case ALP_CC3501E_RESET_WIFI_STACK:
		return "wifi-panic";
	default:
		return "unknown";
	}
}

static const char *companion_role_name(uint8_t role)
{
	switch (role) {
	case ALP_CC3501E_ROLE_OFF:
		return "off";
	case ALP_CC3501E_ROLE_WIFI_STA:
		return "wifi-sta";
	case ALP_CC3501E_ROLE_WIFI_AP:
		return "wifi-ap";
	case ALP_CC3501E_ROLE_BLE_PERIPHERAL:
		return "ble-peripheral";
	case ALP_CC3501E_ROLE_BLE_CENTRAL:
		return "ble-central";
	case ALP_CC3501E_ROLE_DUAL_WIFI_BLE:
		return "wifi+ble";
	default:
		return "unknown";
	}
}

static int cmd_companion_diag_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}

	alp_cc3501e_diag_info_t di = { 0 };
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_diag_info(companion_cc3501e, &di);
	k_mutex_unlock(&companion_bus_lock);

	if (s != ALP_OK) {
		shell_error(sh, "diag info failed (%d)", (int)s);
		return -EIO;
	}
	/* fw_version is the firmware RELEASE version (u16); print it both raw (the
	 * bench uses this to confirm which image is flashed) and as maj.minor -- the
	 * high byte is the major, the low byte the minor. */
	shell_print(sh,
	            "fw:     0x%04x (v%u.%u)",
	            (unsigned int)di.fw_version,
	            (unsigned int)(di.fw_version >> 8),
	            (unsigned int)(di.fw_version & 0xFFu));
	shell_print(sh, "reset:  %s (%u)", companion_reset_cause_name(di.reset_cause), di.reset_cause);
	shell_print(sh, "role:   %s (%u)", companion_role_name(di.role), di.role);
	shell_print(sh, "uptime: %u ms", (unsigned int)di.uptime_ms);
	shell_print(sh, "heap:   %u B free", (unsigned int)di.free_heap_bytes);
	shell_print(sh, "lasterr:%u", di.last_error);
	return 0;
}

static int cmd_companion_diag_stats(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}

	uint32_t frames_ok = 0, frames_err = 0;
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_diag_stats(companion_cc3501e, &frames_ok, &frames_err);
	k_mutex_unlock(&companion_bus_lock);

	if (s != ALP_OK) {
		shell_error(sh, "diag stats failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "frames ok=%u err=%u", (unsigned int)frames_ok, (unsigned int)frames_err);
	return 0;
}

static int cmd_companion_diag_loglevel(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	unsigned long level;
	if (alp_console_parse_ulong(argv[1], &level) != 0 || level > 0xFFu) {
		shell_error(sh, "usage: alp companion diag loglevel <0..255>");
		return -EINVAL;
	}
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_diag_log_level(companion_cc3501e, (uint8_t)level);
	k_mutex_unlock(&companion_bus_lock);

	if (s != ALP_OK) {
		shell_error(sh, "diag loglevel failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "log level <- %lu", level);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    alp_companion_diag_subcmds,
    SHELL_CMD_ARG(info,
                  NULL,
                  "fw version / reset cause / uptime / role",
                  cmd_companion_diag_info,
                  1,
                  0),
    SHELL_CMD_ARG(stats, NULL, "frame counters (ok / err)", cmd_companion_diag_stats, 1, 0),
    SHELL_CMD_ARG(loglevel,
                  NULL,
                  "loglevel <0..255>  -- set firmware log verbosity",
                  cmd_companion_diag_loglevel,
                  2,
                  0),
    SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((alp, companion),
                 diag,
                 &alp_companion_diag_subcmds,
                 "CC3501E diagnostics (info / stats / loglevel)",
                 NULL,
                 1,
                 0);
#endif /* !CONFIG_ALP_SDK_V2N_SUPERVISOR */
