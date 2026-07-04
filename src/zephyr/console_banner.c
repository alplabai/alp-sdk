/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alp SDK improved console boot banner.
 *
 * Replaces Zephyr's one-line "*** Booting Zephyr OS ***" banner with an
 * informative, Linux-console-style header printed on the app console at boot:
 * SDK version, board + SoC + core, the SoM MPN, the on-module memory map, and
 * the console the header itself is coming out of.  It then READS THE HW MANIFEST
 * from the module EEPROM at boot (alp_hw_info_read) and prints the MPN / hw-rev
 * / serial it finds -- or a clear "unavailable" line when the board has no
 * EEPROM bus configured or the read fails (e.g. a bench where the manifest I2C
 * is not reachable).
 *
 * Everything printed is REAL: compile-time identity comes from CONFIG_BOARD /
 * CONFIG_SOC / <alp/version.h>, the memory sizes from the devicetree, the
 * console from the `zephyr,console` chosen node, and the manifest from the live
 * EEPROM read.  Nothing here is invented -- an absent field is printed as such.
 *
 * Enabled by CONFIG_ALP_SDK_CONSOLE_BANNER (default y under CONFIG_ALP_SDK),
 * which also turns Zephyr's basic BOOT_BANNER off so there is one canonical
 * boot header everywhere.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/version.h>

#include <alp/version.h>
#include <alp/hw_info.h>
#include <alp/peripheral.h> /* alp_status_t / ALP_OK / ALP_ERR_* */

/* Human-readable size: whole KiB/MiB when exact, else raw bytes.
 * __maybe_unused: on a target with none of the named memory nodes (e.g.
 * native_sim) every caller is #if'd out, and -Werror=unused-function would
 * otherwise fail the build. */
static __maybe_unused void print_size(const char *label, unsigned long bytes)
{
	if (bytes == 0U) {
		return; /* region absent on this SoC -- skip it */
	}
	if ((bytes % (1024UL * 1024UL)) == 0U) {
		printk(" %s %luM", label, bytes / (1024UL * 1024UL));
	} else if ((bytes % 1024UL) == 0U) {
		printk(" %s %luK", label, bytes / 1024UL);
	} else {
		printk(" %s %luB", label, bytes);
	}
}

/*
 * Emit `print_size("LABEL", DT_REG_SIZE(...))` ONLY when the node exists with a
 * `reg`.  A plain ternary won't do: DT_REG_SIZE expands to an undeclared token
 * on a target that lacks the node (e.g. native_sim has no itcm/dtcm/mram), so
 * the guard has to be a preprocessor `#if`, not a runtime `?:`.
 */
#define ALP_PRINT_REGION(name, label)                                          \
	IF_ENABLED(UTIL_AND(DT_NODE_EXISTS(DT_NODELABEL(label)),                \
			    DT_NODE_HAS_PROP(DT_NODELABEL(label), reg)),        \
		   (print_size(name, (unsigned long)DT_REG_SIZE(DT_NODELABEL(label)));))

static void print_memory_line(void)
{
	printk("  memory  :");
	ALP_PRINT_REGION("ITCM", itcm);
	ALP_PRINT_REGION("DTCM", dtcm);
#if DT_HAS_CHOSEN(zephyr_sram)
	print_size("SRAM", (unsigned long)DT_REG_SIZE(DT_CHOSEN(zephyr_sram)));
#endif
	ALP_PRINT_REGION("MRAM", mram);
	printk("\n");
}

static void print_console_line(void)
{
#if DT_HAS_CHOSEN(zephyr_console)
	printk("  console : %s", DT_NODE_FULL_NAME(DT_CHOSEN(zephyr_console)));
#if DT_NODE_HAS_PROP(DT_CHOSEN(zephyr_console), current_speed)
	printk(" @ %d 8N1", DT_PROP(DT_CHOSEN(zephyr_console), current_speed));
#endif
	printk("  (Alp UART console)\n");
#else
	printk("  console : (RAM console -- read over SWD)\n");
#endif
}

/* Read the on-module HW manifest (EEPROM) at boot and print it, or say why not. */
static void print_manifest_line(void)
{
	alp_hw_info_t info;
	alp_status_t  s = alp_hw_info_read(&info);

	if (s == ALP_OK) {
		printk("  manifest: MPN %s  rev %s  sn %s  (EEPROM)\n",
		       info.som_sku[0] ? info.som_sku : "?",
		       info.som_hw_rev[0] ? info.som_hw_rev : "?",
		       info.som_serial[0] ? info.som_serial : "?");
		if (info.board_name[0]) {
			printk("  board   : %s %s\n", info.board_name,
			       info.board_hw_rev[0] ? info.board_hw_rev : "");
		}
	} else if (s == ALP_ERR_NOSUPPORT) {
		printk("  manifest: unavailable (no EEPROM bus configured)\n");
	} else if (s == ALP_ERR_NOT_PROVISIONED) {
		printk("  manifest: unprovisioned (EEPROM blank -- run program_eeprom.py)\n");
	} else if (s == ALP_ERR_NOT_READY) {
		printk("  manifest: unavailable (EEPROM not ready / bus not wired)\n");
	} else {
		printk("  manifest: unavailable (read failed: %d)\n", (int)s);
	}
}

static int alp_console_banner(void)
{
	printk("\n"
	       "==========================================================\n"
	       "  Alp SDK v%s   |   %s\n"
	       "==========================================================\n",
	       ALP_VERSION_STRING, CONFIG_BOARD_TARGET);
	printk("  soc     : %s   core %s\n", CONFIG_SOC,
#if defined(CONFIG_CPU_CORTEX_M55)
	       "Cortex-M55"
#elif defined(CONFIG_CPU_CORTEX_M33)
	       "Cortex-M33"
#elif defined(CONFIG_CPU_CORTEX_A55)
	       "Cortex-A55"
#else
	       "-"
#endif
	);
	print_memory_line();
	print_console_line();
	printk("  os      : zephyr %s\n", KERNEL_VERSION_STRING);
	print_manifest_line();
	printk("==========================================================\n");
	return 0;
}

/*
 * APPLICATION init level: runs after all drivers (UART console + the I2C bus
 * the EEPROM lives on are up), so both the print and the manifest read work.
 */
SYS_INIT(alp_console_banner, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
