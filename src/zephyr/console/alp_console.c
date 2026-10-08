/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alp SoM console -- the `alp` Zephyr-shell command root, the shared
 * argument parser, and the `alp board` identity command.  Mirrors the
 * boot banner (src/zephyr/alp_banner.c): same SDK version + EEPROM
 * identity source, printed on demand.
 */
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "alp_console.h"

#include <alp/peripheral.h>
#include <alp/console.h>
#include <alp/ext/cc3501e/console.h>
#include <alp/version.h> /* ALP_VERSION_STRING -- the single SDK-version source */

#if defined(CONFIG_ALP_SDK_HW_INFO)
#include <alp/hw_info.h>
#endif

int alp_console_parse_ulong(const char *s, unsigned long *out)
{
	if (s == NULL || *s == '\0') {
		return -EINVAL;
	}

	char         *end = NULL;
	unsigned long v   = strtoul(s, &end, 0); /* base 0: 0x.. hex, else dec */

	if (end == s || *end != '\0') {
		return -EINVAL;
	}
	*out = v;
	return 0;
}

/*
 * Shared by every console group that takes an opaque byte blob on the command
 * line (BLE GATT values, SPI1 passthrough TX).
 */
int alp_console_parse_hex(const char *s, uint8_t *out, size_t cap, size_t *out_len)
{
	if (s == NULL || out == NULL || out_len == NULL) {
		return -EINVAL;
	}

	size_t n = strlen(s);

	if (n == 0u || (n % 2u) != 0u || (n / 2u) > cap) {
		return -EINVAL;
	}
	for (size_t i = 0; i < n; i += 2u) {
		char oct[3] = { s[i], s[i + 1u], '\0' };

		/* isxdigit BEFORE strtoul, not instead of it: strtoul happily accepts
		 * "+1" and " 1" as a full 2-char window, so a typo would silently
		 * decode to 0x01.  On a console verb that clocks bytes into a flash
		 * part that is the wrong kind of forgiving. */
		if (!isxdigit((unsigned char)oct[0]) || !isxdigit((unsigned char)oct[1])) {
			return -EINVAL;
		}
		out[i / 2u] = (uint8_t)strtoul(oct, NULL, 16);
	}
	*out_len = n / 2u;
	return 0;
}

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_CMD_BOARD)
static int cmd_board(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

#if defined(CONFIG_ALP_SDK_HW_INFO)
	alp_hw_info_t info;

	if (alp_hw_info_read(&info) == ALP_OK && info.som_sku[0] != '\0') {
		shell_print(sh,
		            "Alp SDK %s  |  %s %s  |  (c) Alp Lab AB",
		            ALP_VERSION_STRING,
		            info.som_sku,
		            info.som_hw_rev);
		shell_print(sh, "  family : %s", info.som_family);
		shell_print(sh, "  serial : %s", info.som_serial);
		shell_print(sh,
		            "  mfg    : %04u-%02u-%02u",
		            info.som_mfg_year,
		            info.som_mfg_month,
		            info.som_mfg_day);
	} else
#endif
	{
		shell_print(sh, "Alp SDK %s  |  %s  |  (c) Alp Lab AB", ALP_VERSION_STRING, CONFIG_BOARD);
	}

	shell_print(sh, "  uptime : %llu ms", (unsigned long long)k_uptime_get());
	return 0;
}
#endif /* CONFIG_ALP_SDK_CONSOLE_CMD_BOARD */

SHELL_SUBCMD_SET_CREATE(alp_subcmds, (alp));

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_CMD_BOARD)
SHELL_SUBCMD_ADD((alp), board, NULL, "SoM identity, SDK version, uptime", cmd_board, 1, 0);
#endif

SHELL_CMD_REGISTER(alp, &alp_subcmds, "Alp SoM diagnostics console", NULL);

#if !IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_CMD_COMPANION)
/*
 * No-op fallback: when no companion is configured, binding one is a no-op so
 * the public <alp/console.h> symbol always links on a console-enabled build.
 * The real definition lives in alp_console_companion.c under
 * CONFIG_ALP_SDK_CONSOLE_CMD_COMPANION.
 */
void alp_console_companion_set(cc3501e_t *ctx)
{
	ARG_UNUSED(ctx);
}
#endif
