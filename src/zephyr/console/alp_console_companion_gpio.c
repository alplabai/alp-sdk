/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp companion gpio` -- GD32 supervisor GPIO (read / write), V2N only.
 * Command-group TU of the alp_console_companion.c split (#673 Phase 2):
 * registers onto the (alp, companion) dynamic subcommand set the core TU
 * declares.  Talks to the GD32 supervisor singleton directly (V2N has no
 * app-registered companion, no bridge-bus mutex to share -- see
 * "../v2n_supervisor.h"), so it needs nothing from
 * alp_console_companion_internal.h.
 */
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <alp/peripheral.h>

#include "alp_console.h"
#include "../v2n_supervisor.h"

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
    SHELL_CMD_ARG(read,
                  NULL,
                  "read <0..31>  -- sample a GD32 GPIO pin",
                  cmd_companion_gpio_read,
                  2,
                  0),
#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)
    SHELL_CMD_ARG(write,
                  NULL,
                  "write <0..31> <0|1>  -- drive a GD32 GPIO pin (UNSAFE)",
                  cmd_companion_gpio_write,
                  3,
                  0),
#endif
    SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((alp, companion),
                 gpio,
                 &alp_companion_gpio_subcmds,
                 "companion GPIO (V2N only)",
                 NULL,
                 1,
                 0);
#endif /* CONFIG_ALP_SDK_V2N_SUPERVISOR */
