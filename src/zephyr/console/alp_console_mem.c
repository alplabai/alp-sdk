/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp mem` -- width-aware volatile memory peek (rd) and poke (wr).
 * rd is the always-available register-debug primitive; wr is gated on
 * CONFIG_ALP_SDK_CONSOLE_UNSAFE.
 */
#include <errno.h>
#include <stdint.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include "alp_console.h"

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_CMD_MEM)

static int cmd_mem_rd(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long addr;
	unsigned long count = 1;

	if (alp_console_parse_ulong(argv[1], &addr) != 0) {
		shell_error(sh, "bad address");
		return -EINVAL;
	}
	if (argc == 3 && alp_console_parse_ulong(argv[2], &count) != 0) {
		shell_error(sh, "bad count");
		return -EINVAL;
	}

	for (unsigned long i = 0; i < count; i++) {
		uintptr_t          a = (uintptr_t)addr + i * sizeof(uint32_t);
		volatile uint32_t *p = (volatile uint32_t *)a;

		shell_print(sh, "[%08lx] = %08x", (unsigned long)a, *p);
	}
	return 0;
}

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)
static int cmd_mem_wr(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	unsigned long addr;
	unsigned long val;

	if (alp_console_parse_ulong(argv[1], &addr) != 0 ||
	    alp_console_parse_ulong(argv[2], &val) != 0) {
		shell_error(sh, "usage: alp mem wr <addr> <u32>");
		return -EINVAL;
	}

	*(volatile uint32_t *)(uintptr_t)addr = (uint32_t)val;
	shell_print(sh, "[%08lx] <- %08lx", addr, val);
	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
    alp_mem_subcmds,
    SHELL_CMD_ARG(rd, NULL, "rd <addr> [words] -- read u32(s)", cmd_mem_rd, 2, 1),
#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)
    SHELL_CMD_ARG(wr, NULL, "wr <addr> <u32> -- write u32 (UNSAFE)", cmd_mem_wr, 3, 0),
#endif
    SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((alp), mem, &alp_mem_subcmds, "Memory / register peek-poke", NULL, 1, 0);

#endif /* CONFIG_ALP_SDK_CONSOLE_CMD_MEM */
