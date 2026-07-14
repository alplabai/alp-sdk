/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Alp SDK boot-identity banner.
 *
 * Prints the SDK + SoM + SoC identity and a system summary at APPLICATION
 * init:
 *
 *   Alp SDK 0.10.0  |  E1M-AEN801  |  Alif Ensemble E8  |  (c) Alp Lab AB
 *     CPU: M55-HE @160MHz (active) + M55-HP @400MHz + 2x Cortex-A32 @800MHz
 *     NPU: Ethos-U85 + 2x Ethos-U55   |   SRAM 9984 KB | MRAM 5.5 MB
 *
 * Identity field (the SoM column), in priority order:
 *   1. LIVE EEPROM manifest (CONFIG_ALP_SDK_HW_INFO): the SoM's true SKU +
 *      hardware revision, read from the on-module identity EEPROM.
 *   2. CONFIG_ALP_SDK_SOM_SKU: the build-time SoM SKU (board.yaml `som.sku`).
 *   3. CONFIG_BOARD: the raw Zephyr board target -- last-resort fallback.
 *
 * The SoC column + the system summary come from the SoC spec JSON (cores /
 * npus / total SRAM+MRAM), pre-formatted into CONFIG_ALP_SDK_SOC_* by
 * scripts/alp_orchestrate.py.  Builds without those (e.g. apps not built
 * through alp_orchestrate.py, or native_sim) fall back to the devicetree
 * (running-core clock + the chosen sram/flash region sizes).  No value here
 * is invented -- every number is data-driven from the SoC JSON or the DT.
 *
 * Compiled only when CONFIG_ALP_SDK_BANNER=y (the whole TU is gated in CMake).
 * Uses printk so it lands on whatever console backend the app wired.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>

#include <alp/version.h> /* ALP_VERSION_STRING -- the single SDK-version source */

#if defined(CONFIG_ALP_SDK_HW_INFO)
#include <alp/hw_info.h> /* alp_hw_info_read(), alp_hw_info_t, ALP_OK */
#endif

/* Append the SoC display name (when known) + the manufacturer, then end the
 * identity line.  Shared by both identity paths (live EEPROM / build-time). */
static void alp_print_soc_and_eol(void)
{
#if defined(CONFIG_ALP_SDK_SOC_NAME)
	if (CONFIG_ALP_SDK_SOC_NAME[0] != '\0') {
		printk("  |  %s", CONFIG_ALP_SDK_SOC_NAME);
	}
#endif
	printk("  |  (c) Alp Lab AB\n");
}

/*
 * System summary.  Preferred path: the SoC spec complement (every CPU core +
 * NPU + the SoC's total on-chip SRAM/MRAM, emitted from the SoC JSON by
 * alp_orchestrate.py -- the active core is marked and listed first).  Fallback
 * for builds without the SoC config: the devicetree (running-core clock + the
 * chosen sram/flash region sizes), each field guarded for boards that lack it.
 */
static void alp_print_sysinfo(void)
{
#if defined(CONFIG_ALP_SDK_SOC_CPUS)
	if (CONFIG_ALP_SDK_SOC_CPUS[0] != '\0') {
		printk("  CPU: %s\n", CONFIG_ALP_SDK_SOC_CPUS);
		printk("  NPU: %s   |   SRAM %u KB | MRAM %u.%u MB\n",
		       CONFIG_ALP_SDK_SOC_NPUS,
		       (unsigned int)CONFIG_ALP_SDK_SOC_SRAM_KB,
		       (unsigned int)(CONFIG_ALP_SDK_SOC_MRAM_KB / 1024),
		       (unsigned int)((CONFIG_ALP_SDK_SOC_MRAM_KB % 1024) * 10 / 1024));
		return;
	}
#endif

	unsigned int cpu_mhz =
#if DT_NODE_HAS_PROP(DT_PATH(cpus, cpu_0), clock_frequency)
	    (unsigned int)(DT_PROP(DT_PATH(cpus, cpu_0), clock_frequency) / 1000000U);
#else
	    (unsigned int)(sys_clock_hw_cycles_per_sec() / 1000000U);
#endif

	printk("  CPU %u MHz", cpu_mhz);
#if DT_HAS_CHOSEN(zephyr_sram)
	printk("  |  RAM %u KB", (unsigned int)(DT_REG_SIZE(DT_CHOSEN(zephyr_sram)) / 1024U));
#endif
#if DT_HAS_CHOSEN(zephyr_flash)
	printk("  |  ROM %u KB", (unsigned int)(DT_REG_SIZE(DT_CHOSEN(zephyr_flash)) / 1024U));
#endif
	printk("\n");
}

static int alp_sdk_banner(void)
{
#if defined(CONFIG_ALP_SDK_HW_INFO)
	alp_hw_info_t info;

	/*
	 * Best-effort live identity: a missing/unprovisioned/unreadable EEPROM
	 * just falls through to the build-time name -- the banner never fails
	 * a boot.
	 */
	if (alp_hw_info_read(&info) == ALP_OK && info.som_sku[0] != '\0') {
		printk("Alp SDK %s  |  %s %s", ALP_VERSION_STRING, info.som_sku, info.som_hw_rev);
		alp_print_soc_and_eol();
		alp_print_sysinfo();
		return 0;
	}
#endif

	/* Build-time SoM SKU (board.yaml som.sku) before the raw board target. */
	const char *board_name = CONFIG_BOARD;
#if defined(CONFIG_ALP_SDK_SOM_SKU)
	if (CONFIG_ALP_SDK_SOM_SKU[0] != '\0') {
		board_name = CONFIG_ALP_SDK_SOM_SKU;
	}
#endif
	printk("Alp SDK %s  |  %s", ALP_VERSION_STRING, board_name);
	alp_print_soc_and_eol();
	alp_print_sysinfo();
	return 0;
}

/*
 * APPLICATION level so the console (UART or RAM console) is already up;
 * default priority so it prints after device init but before the app's main().
 */
SYS_INIT(alp_sdk_banner, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
