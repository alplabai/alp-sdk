/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp clk` (system-clock facts) and `alp reboot` (warm reset, UNSAFE).
 */
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_CMD_REBOOT) && IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE) && \
    IS_ENABLED(CONFIG_REBOOT)
#include <zephyr/sys/reboot.h>
#endif

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_CMD_CLK)
static int cmd_clk(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "sys clock : %u Hz", (unsigned int)sys_clock_hw_cycles_per_sec());
	shell_print(sh, "tick rate : %d Hz", (int)CONFIG_SYS_CLOCK_TICKS_PER_SEC);
	return 0;
}

SHELL_SUBCMD_ADD((alp), clk, NULL, "System clock facts", cmd_clk, 1, 0);
#endif /* CONFIG_ALP_SDK_CONSOLE_CMD_CLK */

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_CMD_REBOOT) && IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)
static int cmd_reboot(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

#if IS_ENABLED(CONFIG_REBOOT)
	shell_print(sh, "rebooting...");
	sys_reboot(SYS_REBOOT_WARM);
	return 0; /* not reached */
#else
	shell_error(sh, "CONFIG_REBOOT not enabled in this build");
	return -ENOTSUP;
#endif
}

SHELL_SUBCMD_ADD((alp), reboot, NULL, "Warm reset (UNSAFE)", cmd_reboot, 1, 0);
#endif /* CONFIG_ALP_SDK_CONSOLE_CMD_REBOOT && CONFIG_ALP_SDK_CONSOLE_UNSAFE */
