/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp gpio` -- read / configure / write a portable GPIO by pin_id
 * (the E1M_* / BOARD_* indices from <alp/e1m_pinout.h>).
 *
 * `read` is always available; `write` is gated on
 * CONFIG_ALP_SDK_CONSOLE_UNSAFE because it can drive live hardware
 * signals without any safeguard.
 */
#include <errno.h>

#include <zephyr/shell/shell.h>

#include <alp/peripheral.h>

#include "alp_console.h"

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_CMD_GPIO)

static int cmd_gpio_read(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	unsigned long pin;

	if (alp_console_parse_ulong(argv[1], &pin) != 0) {
		shell_error(sh, "bad pin id");
		return -EINVAL;
	}

	alp_gpio_t *g = alp_gpio_open((uint32_t)pin);

	if (g == NULL) {
		shell_error(sh, "open pin %lu failed (err %d)", pin, (int)alp_last_error());
		return -EIO;
	}

	(void)alp_gpio_configure(g, ALP_GPIO_INPUT, ALP_GPIO_PULL_NONE);

	bool         level = false;
	alp_status_t s     = alp_gpio_read(g, &level);

	alp_gpio_close(g);
	if (s != ALP_OK) {
		shell_error(sh, "read failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "pin %lu = %d", pin, (int)level);
	return 0;
}

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)
static int cmd_gpio_write(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	unsigned long pin;
	unsigned long val;

	if (alp_console_parse_ulong(argv[1], &pin) != 0 ||
	    alp_console_parse_ulong(argv[2], &val) != 0 || val > 1) {
		shell_error(sh, "usage: alp gpio write <pin> <0|1>");
		return -EINVAL;
	}

	alp_gpio_t *g = alp_gpio_open((uint32_t)pin);

	if (g == NULL) {
		shell_error(sh, "open pin %lu failed (err %d)", pin, (int)alp_last_error());
		return -EIO;
	}
	(void)alp_gpio_configure(g, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);

	alp_status_t s = alp_gpio_write(g, val != 0);

	alp_gpio_close(g);
	if (s != ALP_OK) {
		shell_error(sh, "write failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "pin %lu <- %lu", pin, val);
	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
    alp_gpio_subcmds,
    SHELL_CMD_ARG(read, NULL, "read <pin> -- sample a pin as input", cmd_gpio_read, 2, 0),
#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)
    SHELL_CMD_ARG(write, NULL, "write <pin> <0|1> -- drive a pin (UNSAFE)", cmd_gpio_write, 3, 0),
#endif
    SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((alp), gpio, &alp_gpio_subcmds, "GPIO read / write by pin_id", NULL, 1, 0);

#endif /* CONFIG_ALP_SDK_CONSOLE_CMD_GPIO */
