/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp companion ota` -- CC3501E OTA firmware update (status / begin /
 * abort), Alif companion only.  Command-group TU of the
 * alp_console_companion.c split (#673 Phase 2): registers onto the
 * (alp, companion) dynamic subcommand set the core TU declares.  Shared
 * companion context comes from
 * alp_console_companion_internal.h.
 *
 * Inspect + drive the over-the-bridge PSA-FWU session (host wrappers
 * cc3501e_ota_*).  `status` is read-only (safe to poll any time); `begin`
 * and `abort` open / cancel a session so the state machine can be walked on
 * the bench.  Streaming a full image is an app job (cc3501e_ota_update) --
 * see examples/aen/aen-companion-ota -- not a shell command.
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
#define ALP_COMPANION_OTA_MS 30000u

/* Map the OTA session state to a short label for `ota status`. */
static const char *companion_ota_state_name(uint8_t state)
{
	switch (state) {
	case ALP_CC3501E_OTA_STATE_IDLE:
		return "idle";
	case ALP_CC3501E_OTA_STATE_WRITING:
		return "writing";
	case ALP_CC3501E_OTA_STATE_STAGED:
		return "staged";
	case ALP_CC3501E_OTA_STATE_ERROR:
		return "error";
	default:
		return "unknown";
	}
}

static int cmd_companion_ota_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}

	alp_cc3501e_ota_status_t st = { 0 };
	alp_status_t             s  = cc3501e_ota_status(companion_cc3501e, &st, ALP_COMPANION_OTA_MS);

	if (s == ALP_ERR_NOT_READY) {
		shell_warn(sh, "OTA not available (no PSA-FWU in this CC3501E image)");
		return -ENOTSUP;
	}
	if (s != ALP_OK) {
		shell_error(sh, "ota status failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "state:   %s (%u)", companion_ota_state_name(st.state), st.state);
	shell_print(sh, "written: %u B", (unsigned int)st.bytes_written);
	shell_print(sh, "total:   %u B", (unsigned int)st.total_len);
	/* #1610 bench triage.  reserved[2] carries TWO encodings, and treating it as
	 * a bare fault code printed `fault: stage 64` on every healthy session:
	 *   1..0x3F  the psa_fwu_* call that failed the last window flush
	 *            (cc3501e_hw_ota_fault's fail_stage).
	 *   0x40|p   no psa fault -- the low bits are the transport PHASE, and bit
	 *   0xC0|p   0x80 says the bridge is in POLLED mode.  The firmware emits
	 *            this shape whenever fail_stage is 0, i.e. on every good run
	 *            (cc3501e-bridge-firmware:src/protocol_ota.c).
	 * 0 means the device published neither. */
	if (st.reserved[2] != 0u && st.reserved[2] < 0x40u) {
		shell_print(sh, "fault:   stage %u (see cc3501e_hw_ota_fault)", st.reserved[2]);
	} else if (st.reserved[2] != 0u) {
		shell_print(sh,
		            "bridge:  %s, phase %u",
		            (st.reserved[2] & 0x80u) ? "polled" : "DMA",
		            (unsigned int)(st.reserved[2] & 0x3Fu));
	}
	return 0;
}

static int cmd_companion_ota_begin(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	unsigned long total_len;
	if (alp_console_parse_ulong(argv[1], &total_len) != 0 || total_len == 0u ||
	    total_len > 0xFFFFFFFFu) {
		shell_error(sh, "usage: alp companion ota begin <total_len_bytes>");
		return -EINVAL;
	}
	/* Enter update mode FIRST.  Opening a session runs psa_fwu_query and, on a
	 * dirty slot, a full slot erase -- and those never return while the bridge is
	 * open in callback/DMA mode, so this command used to WEDGE the device until a
	 * WIFI_EN/nRESET cold cycle.  cc3501e_ota_begin now refuses that outright, so
	 * without this the command would simply always fail. */
	alp_status_t s = cc3501e_ota_update_mode(companion_cc3501e, true, ALP_COMPANION_OTA_MS);
	if (s != ALP_OK) {
		shell_error(sh,
		            "could not enter OTA update mode (%d) -- the device reboots into a "
		            "polled bridge for OTA; retry, or reset the companion",
		            (int)s);
		return -EIO;
	}
	s = cc3501e_ota_begin(companion_cc3501e, (uint32_t)total_len, ALP_COMPANION_OTA_MS);

	if (s == ALP_ERR_NOT_READY) {
		shell_warn(sh, "OTA not available (no PSA-FWU in this CC3501E image)");
		return -ENOTSUP;
	}
	if (s != ALP_OK) {
		shell_error(sh, "ota begin failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh,
	            "OTA session open for %lu B -- stream the image from the app "
	            "(cc3501e_ota_update); `ota status` to watch progress",
	            total_len);
	return 0;
}

static int cmd_companion_ota_abort(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	alp_status_t s = cc3501e_ota_abort(companion_cc3501e, ALP_COMPANION_OTA_MS);

	if (s != ALP_OK) {
		shell_error(sh, "ota abort failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "OTA session aborted -- back to idle");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    alp_companion_ota_subcmds,
    SHELL_CMD_ARG(status,
                  NULL,
                  "session state + bytes written / total (read-only)",
                  cmd_companion_ota_status,
                  1,
                  0),
    SHELL_CMD_ARG(begin,
                  NULL,
                  "begin <total_len>  -- open an OTA session for a total_len-byte image",
                  cmd_companion_ota_begin,
                  2,
                  0),
    SHELL_CMD_ARG(abort,
                  NULL,
                  "cancel the OTA session (back to idle)",
                  cmd_companion_ota_abort,
                  1,
                  0),
    SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((alp, companion),
                 ota,
                 &alp_companion_ota_subcmds,
                 "CC3501E OTA firmware update (status / begin / abort)",
                 NULL,
                 1,
                 0);
#endif /* !CONFIG_ALP_SDK_V2N_SUPERVISOR */
