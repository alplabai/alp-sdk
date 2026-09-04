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
 *   Alp SDK 0.16.0  |  E1M-AEN801  |  Alif Ensemble E8  |  (c) Alp Lab AB
 *     CPU: M55-HE @160MHz (active) + M55-HP @400MHz + 2x Cortex-A32 @800MHz
 *     NPU: Ethos-U85 + 2x Ethos-U55   |   SRAM 9984 KB | MRAM 5.5 MB
 *
 * Identity field (the SoM column), in priority order:
 *   1. LIVE EEPROM manifest (CONFIG_ALP_SDK_HW_INFO): the SoM's true SKU +
 *      hardware revision, read from the on-module identity EEPROM.
 *   2. CONFIG_ALP_SDK_SOM_SKU: the build-time SoM SKU (board.yaml `som.sku`).
 *   3. CONFIG_BOARD: the raw Zephyr board target -- last-resort fallback.
 *
 * When the live manifest read (path 1) succeeds, this file also compares
 * its hw_rev against CONFIG_ALP_SDK_SOM_HW_REV -- the hw_rev this
 * firmware BUILD resolved -- and warns loudly on a disagreement; see
 * alp_check_hw_rev_match() below for why that is a warning, not a
 * refused boot, by default (issue #1853).
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
#include <alp/hw_info.h>      /* alp_hw_info_read(), alp_hw_info_t, ALP_OK */
#include "hw_info_manifest.h" /* alp_hw_info_build_hw_rev_mismatch() -- internal, issue #1853 */
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

#if defined(CONFIG_ALP_SDK_HW_INFO)
/*
 * Boot-time hw_rev mismatch check (issue #1853).  CONFIG_ALP_SDK_SOM_HW_REV
 * is the hw_rev this firmware BUILD resolved (board.yaml `som.hw_rev`,
 * falling back to the SKU preset's `default_hw_rev`); the EEPROM manifest
 * just read above is the module's ACTUAL revision.  Nothing in this
 * firmware image derives a pad-routing table from that build-time value --
 * the SoM preset's `pad_routes`/`pad_route_overrides` data is read only by
 * scripts/alp_project_emit/bom_netlist.py, for the debug/BOM
 * `--emit composed-route-table` / `--emit carrier-netlist` surfaces, not
 * by any header/C table/DT overlay this build produces.
 *
 * The real-world risk this check warns about is downstream of that gap:
 * on the AEN family, three E1M pads (IO8/IO10/IO21) physically sit on a
 * DIFFERENT chip depending on hw_rev, and application code that hardcodes
 * a pin-to-chip map for one revision (see #1859 --
 * examples/aen/aen-cc3501e-gpio/src/cc3501e_gpio_routes.c hardcodes the r2
 * map with no IO21 entry, same table duplicated in aen-cc3501e-bringup and
 * aen-cc3501e-companion-tour) silently targets the wrong chip on the other
 * revision, with no diagnostic anywhere.  This check cannot fix that
 * hardcoded table; it can only tell the developer their firmware and their
 * board disagree.
 *
 * Severity, chosen deliberately:
 *   - A loud warning is the FLOOR, always on: this is real -- silently
 *     driving the wrong chip is the exact defect class that has already
 *     produced multiple wrong bench conclusions (see the issue).
 *   - Refusing to boot is NOT the default: alp_hw_info_read() already
 *     routed a NOT_PROVISIONED / unreadable manifest away from this
 *     function entirely (this only runs on ALP_OK), so a factory-fresh
 *     module never trips it -- but a mismatch can also be entirely
 *     benign (a dev board relabelled, a board.yaml that hasn't caught up
 *     yet), and bricking a developer's board over a revision string is a
 *     worse failure mode than a possibly-wrong pin.  A production build
 *     that wants to fail closed instead opts in via
 *     CONFIG_ALP_SDK_HW_REV_MISMATCH_FATAL.
 *   - This check lives entirely inside the boot banner (compiled only
 *     under CONFIG_ALP_SDK_BANNER); a build that turns the banner off for
 *     footprint gets neither the warning nor CONFIG_ALP_SDK_HW_REV_
 *     MISMATCH_FATAL.  Known limitation, not fixed here -- see the
 *     Kconfig help.
 *   - What this does NOT do: refuse to DISPATCH only the specific pads
 *     whose route actually differs between hw_revs (the issue's
 *     "stronger guard").  No dispatcher consults any pad-route table
 *     today, so there is nothing to retrofit -- the real missing piece
 *     is #1859: generate a per-hw_rev `cc3501e_gpio_routes[]` from the
 *     composed route table (replacing the three hand-written, r2-only
 *     copies above) plus one hw_rev guard in the GPIO proxy.  GPIO-only,
 *     much smaller than a dispatch-layer change, and out of scope for
 *     this boot-banner fix.  CONFIG_ALP_SDK_HW_REV_MISMATCH_FATAL is the
 *     coarse mitigation available today: it halts before any pad is
 *     ever dispatched, covering the whole app rather than just the
 *     ambiguous pads.
 */
static void alp_check_hw_rev_match(const alp_hw_info_t *info)
{
	if (!alp_hw_info_build_hw_rev_mismatch(info, CONFIG_ALP_SDK_SOM_HW_REV)) {
		return;
	}
	printk("Alp SDK: WARNING hw_rev mismatch -- built for '%s', module reports '%s'.\n",
	       CONFIG_ALP_SDK_SOM_HW_REV,
	       info->som_hw_rev);
	printk("  Pad routing can differ by hw_rev (docs/board-config-hardware.md);"
	       " a pin may be driven on the wrong chip.\n");
#if defined(CONFIG_ALP_SDK_HW_REV_MISMATCH_FATAL)
	printk("  CONFIG_ALP_SDK_HW_REV_MISMATCH_FATAL=y -- halting.\n");
	k_panic();
#endif
}
#endif /* CONFIG_ALP_SDK_HW_INFO */

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
		alp_check_hw_rev_match(&info);
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
