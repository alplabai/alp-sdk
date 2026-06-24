/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp clk` (system-clock facts), `alp res` (live resource view), and
 * `alp reboot` (warm reset, UNSAFE).
 */
#include <errno.h>

#include <zephyr/devicetree.h>
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

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_CMD_RES)
/*
 * `alp res` -- a live resource snapshot: uptime, the SoC core complement (the
 * active core marked), this build's RAM region + the SoC's total SRAM/MRAM,
 * and a per-thread stack used/free table.  Read-only, safe.  The thread table
 * is runtime (k_thread_stack_space_get); the static SoC totals come from the
 * SoC spec JSON via CONFIG_ALP_SDK_SOC_* (the same source as alp_banner.c).
 */
#if IS_ENABLED(CONFIG_THREAD_MONITOR) && IS_ENABLED(CONFIG_THREAD_STACK_INFO)
struct res_stack_acc {
	const struct shell *sh;
	size_t             total_size;
	size_t             total_free;
};

static void res_thread_cb(const struct k_thread *thread, void *user_data)
{
	struct res_stack_acc *acc = (struct res_stack_acc *)user_data;
	size_t                size = thread->stack_info.size;
	size_t                unused = 0U;
	const char           *name;

	(void)k_thread_stack_space_get(thread, &unused);
	name = k_thread_name_get((k_tid_t)thread);
	acc->total_size += size;
	acc->total_free += unused;
	shell_print(acc->sh, "    %-16s %6u B  used %6u  free %6u",
	            (name != NULL && name[0] != '\0') ? name : "?",
	            (unsigned int)size, (unsigned int)(size - unused),
	            (unsigned int)unused);
}
#endif /* THREAD_MONITOR && THREAD_STACK_INFO */

static int cmd_res(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "uptime  : %lld ms", k_uptime_get());

#if defined(CONFIG_ALP_SDK_SOC_CPUS)
	if (CONFIG_ALP_SDK_SOC_CPUS[0] != '\0') {
		shell_print(sh, "cores   : %s", CONFIG_ALP_SDK_SOC_CPUS);
		shell_print(sh, "          (this image runs the active core; other "
		                "cores are not started by it)");
	}
#endif

#if DT_HAS_CHOSEN(zephyr_sram)
	shell_print(sh, "RAM     : %u KB (this build's region)",
	            (unsigned int)(DT_REG_SIZE(DT_CHOSEN(zephyr_sram)) / 1024U));
#endif
#if defined(CONFIG_ALP_SDK_SOC_SRAM_KB) && (CONFIG_ALP_SDK_SOC_SRAM_KB > 0)
	shell_print(sh, "SoC mem : SRAM %u KB | MRAM %u KB (total on-chip)",
	            (unsigned int)CONFIG_ALP_SDK_SOC_SRAM_KB,
	            (unsigned int)CONFIG_ALP_SDK_SOC_MRAM_KB);
#endif

#if IS_ENABLED(CONFIG_THREAD_MONITOR) && IS_ENABLED(CONFIG_THREAD_STACK_INFO)
	struct res_stack_acc acc = {.sh = sh, .total_size = 0U, .total_free = 0U};

	shell_print(sh, "threads : stack used/free per thread");
	k_thread_foreach_unlocked(res_thread_cb, &acc);
	shell_print(sh, "    -- total stack %u B, free %u B",
	            (unsigned int)acc.total_size, (unsigned int)acc.total_free);
#else
	shell_print(sh, "threads : (enable CONFIG_THREAD_MONITOR for the table)");
#endif
	return 0;
}

SHELL_SUBCMD_ADD((alp), res, NULL, "Live resources: cores, memory, thread stacks",
		 cmd_res, 1, 0);
#endif /* CONFIG_ALP_SDK_CONSOLE_CMD_RES */

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
