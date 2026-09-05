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
 *   Alp SDK 0.16.0  |  E1M-AEN803 2626-r2  |  Alif Ensemble E8  |  (c) Alp Lab AB
 *     CPU: M55-HE @160MHz (active) + 2x Cortex-A32 @800MHz + M55-HP @400MHz
 *     NPU: Ethos-U85 + 2x Ethos-U55   |   SRAM 9984 KB | MRAM 5.5 MB
 *     EXT: 512 Mbit (64 MiB) RAM + 256 Mbit (32 MiB) NOR (OSPI)
 *     RTC: present, time not set (no backup supply on this batch)
 *     Temp: 23456 milli-degC
 *
 * The EXT line is OMITTED entirely on a SKU whose BOM leaves both external
 * memories DNI (CONFIG_ALP_SDK_SOM_DRAM_MBIT == CONFIG_ALP_SDK_SOM_FLASH_MBIT
 * == 0), and prints only the populated half on a SKU that populates just one.
 * E1M-AEN803 is the SKU that fits both; E1M-AEN801 fits NEITHER and shows no
 * EXT line at all -- it runs from the SoC's on-die MRAM.
 *
 * That distinction was wrong until 2026-09-05: the SoM preset's `hyperram`
 * block had no `assembled` key, the schema defaults it to TRUE, and so an
 * E1M-AEN801 banner advertised "EXT: 256 Mbit RAM (OSPI)" for a part the SKU
 * does not populate.  Both memories are now explicitly `assembled: false` there
 * with `memory: dram_mbit/flash_mbit: 0`, and the schema gained
 * hyperram.assembled so absence can be stated rather than implied.
 *
 * The last two lines (CONFIG_ALP_SDK_BANNER_HOUSEKEEPING, on by default) are
 * REPORT ONLY: an on-module RTC (compatible "microcrystal,rv3028") and/or
 * ambient-temperature sensor (compatible "ti,tmp112"), whichever the
 * devicetree enables -- absent on a SoM without one, e.g. native_sim.  A
 * missing/failing device prints one line and never fails the boot; see
 * alp_print_housekeeping() below.
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
 * The SoC column + the CPU/NPU/SRAM/MRAM system summary come from the SoC
 * spec JSON (cores / npus / total on-chip SRAM+MRAM -- silicon facts, true of
 * every SKU on that silicon), pre-formatted into CONFIG_ALP_SDK_SOC_* by
 * scripts/alp_orchestrate.py (`alp_orchestrate.kconfig._emit_soc_summary`).
 * The EXT line is a separate, MODULE-level fact from the same emitter: the
 * SoM preset's `memory:` block (off-SoC OSPI RAM/NOR the SKU's BOM actually
 * populates -- CONFIG_ALP_SDK_SOM_DRAM_MBIT / _FLASH_MBIT), which can differ
 * between SKUs sharing the identical PCB/silicon.  Builds without a resolved
 * SoC spec (e.g. apps not built through alp_orchestrate.py, or native_sim)
 * fall back to the devicetree (running-core clock + the chosen sram/flash
 * region sizes) and never print an EXT line.  No value here is invented --
 * every number is data-driven from the SoC JSON, the SoM preset, or the DT.
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

/*
 * On-module housekeeping devices (RTC, ambient temperature).  Bound by
 * DEVICETREE COMPATIBLE, never by node label or alias -- the board layer
 * that owns these nodes is free to name/relabel them, and "microcrystal,
 * rv3028" / "ti,tmp112" are the stable contracts (same choice
 * examples/aen/aen-temp-sensor already makes for TMP112).
 *
 * A node's DT status alone is NOT enough to gate DEVICE_DT_GET(): the
 * AEN801 board layer enables both nodes UNCONDITIONALLY, but upstream only
 * compiles rtc_rv3028.c / the TMP112 sensor driver in when the app itself
 * also turns on the driver subsystem (CONFIG_RTC / CONFIG_SENSOR) --
 * RTC_RV3028 and TMP112 both live inside an `if RTC` / `if SENSOR` Kconfig
 * block upstream, `default y` only once that parent is on.  Gating on the
 * DT status alone linked clean but failed at the FINAL link step with
 * "undefined reference to __device_dts_ord_*" on any AEN801 app that
 * enables ALP_SDK without also enabling RTC/SENSOR (e.g.
 * examples/aen/aen-can-regcheck) -- caught by this file's own build
 * verification, not by inspection.  So: each block additionally requires
 * its driver's own Kconfig symbol, and quietly compiles to nothing (no
 * link reference at all) on a build that has the DT node but never opted
 * into the driver -- same "absent" reporting as a SoM with neither part.
 */
#if defined(CONFIG_ALP_SDK_BANNER_HOUSEKEEPING)
#include <errno.h>
#if DT_HAS_COMPAT_STATUS_OKAY(microcrystal_rv3028) && defined(CONFIG_RTC_RV3028)
#include <zephyr/drivers/rtc.h>
#define ALP_BANNER_HAS_RTC 1
#endif
#if DT_HAS_COMPAT_STATUS_OKAY(ti_tmp112) && defined(CONFIG_TMP112)
#include <zephyr/drivers/sensor.h>
#define ALP_BANNER_HAS_TEMP 1
#endif
#endif /* CONFIG_ALP_SDK_BANNER_HOUSEKEEPING */

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
 * alp_orchestrate.py -- the active core is marked and listed first) plus, when
 * the SoM SKU's BOM populates any, the external OSPI RAM/NOR the SoC itself
 * does not carry (CONFIG_ALP_SDK_SOM_{DRAM,FLASH}_MBIT, emitted from the SoM
 * preset's `memory:` block -- a MODULE fact: two SKUs on the identical
 * PCB/silicon can differ here, e.g. E1M-AEN803 populates both external
 * memories, E1M-AEN801 only the RAM).  Fallback for builds without the SoC
 * config: the devicetree (running-core clock + the chosen sram/flash region
 * sizes), each field guarded for boards that lack it.
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
#if defined(CONFIG_ALP_SDK_SOM_DRAM_MBIT) && defined(CONFIG_ALP_SDK_SOM_FLASH_MBIT)
		if (CONFIG_ALP_SDK_SOM_DRAM_MBIT > 0 || CONFIG_ALP_SDK_SOM_FLASH_MBIT > 0) {
			/* Off-SoC OSPI memory the SoM SKU's BOM actually populates --
			 * NOT a SoC fact, so it stays a separate line from CPU/NPU/
			 * SRAM/MRAM above.  Either half is omitted when the SKU
			 * leaves that part DNI/optional (e.g. E1M-AEN801's NOR). */
			/* Both units, deliberately.  These parts are specified in
			 * Mbit but everything else on this banner (SRAM KB, MRAM MB)
			 * is in bytes, and "512 Mbit" next to "MRAM 5.5 MB" reads as
			 * 512 MB at a glance -- it was misread exactly that way on
			 * first sight.  Mbit / 8 = MiB, and both are exact for every
			 * capacity these parts come in. */
			printk("  EXT:");
			if (CONFIG_ALP_SDK_SOM_DRAM_MBIT > 0) {
				printk(" %u Mbit (%u MiB) RAM",
				       (unsigned int)CONFIG_ALP_SDK_SOM_DRAM_MBIT,
				       (unsigned int)(CONFIG_ALP_SDK_SOM_DRAM_MBIT / 8));
			}
			if (CONFIG_ALP_SDK_SOM_FLASH_MBIT > 0) {
				printk("%s %u Mbit (%u MiB) NOR",
				       CONFIG_ALP_SDK_SOM_DRAM_MBIT > 0 ? " +" : "",
				       (unsigned int)CONFIG_ALP_SDK_SOM_FLASH_MBIT,
				       (unsigned int)(CONFIG_ALP_SDK_SOM_FLASH_MBIT / 8));
			}
			printk(" (OSPI)\n");
		}
#endif
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
 * Per-unit identity: the factory serial and the manufacturing date, both from
 * the SAME manifest read that already produced som_sku / som_hw_rev above -- so
 * this costs no extra I2C traffic at boot.
 *
 * Printed on its own line rather than appended to the SDK/SKU line: with SKU +
 * hw_rev + serial + date + the SoC display name, one line runs well past 80
 * columns on a real module (E1M-AEN803 / 2626-r2 / 2026W36-0001 / 2026-09-04 /
 * Alif Ensemble E8).
 *
 * Both fields are guarded, and deliberately so: this is the LIVE-EEPROM path,
 * and a module provisioned before these fields carried data -- or one whose
 * manifest region is zeroed -- must degrade quietly rather than print an empty
 * serial or a nonsense "0000-00-00".  The build-time fallback identity path
 * below has no manifest at all and therefore prints neither.
 */
static void alp_print_unit_identity(const alp_hw_info_t *info)
{
	bool have_serial = (info->som_serial[0] != '\0');
	/* Plausibility, not validation: the point is to reject a zeroed or
	 * never-written region, not to police the calendar. */
	bool have_date =
	    (info->som_mfg_year >= 2000U && info->som_mfg_year <= 2199U && info->som_mfg_month >= 1U &&
	     info->som_mfg_month <= 12U && info->som_mfg_day >= 1U && info->som_mfg_day <= 31U);

	if (!have_serial && !have_date) {
		return;
	}

	printk("  Unit:");
	if (have_serial) {
		printk(" %s", info->som_serial);
	}
	if (have_date) {
		printk("%s%04u-%02u-%02u",
		       have_serial ? "  |  mfg " : " mfg ",
		       (unsigned int)info->som_mfg_year,
		       (unsigned int)info->som_mfg_month,
		       (unsigned int)info->som_mfg_day);
	}
	printk("\n");
}

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

#if defined(CONFIG_ALP_SDK_BANNER_HOUSEKEEPING)
/*
 * REPORT ONLY -- never gate.  These are non-critical on-module parts and
 * this code runs in every customer product: a missing, absent or failing
 * device prints one short line and falls through, never fails the boot, and
 * never returns an error to SYS_INIT.  Bounded throughout -- no retry loop,
 * no wait for an RTC tick; the oscillator-runs proof belongs to
 * examples/aen/aen-rtc-alarm, not this banner.
 */
static void alp_print_housekeeping(void)
{
#if defined(ALP_BANNER_HAS_RTC)
	const struct device *const rtc =
	    DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(microcrystal_rv3028));

	if (!device_is_ready(rtc)) {
		printk("  RTC: present, not ready\n");
	} else {
		struct rtc_time tm;
		int             rc = rtc_get_time(rtc, &tm);

		if (rc == 0) {
			printk("  RTC: present, time set\n");
		} else if (rc == -ENODATA) {
			/* Expected on a cold boot on this batch: VBACKUP has no
			 * supply fitted (R4/R68 both 0-ohm DNP), so the RV-3028
			 * never retains time across a power cycle.  Not a fault. */
			printk("  RTC: present, time not set (no backup supply on this batch)\n");
		} else {
			printk("  RTC: present, read failed (rc=%d)\n", rc);
		}
	}
#endif

#if defined(ALP_BANNER_HAS_TEMP)
	const struct device *const temp = DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(ti_tmp112));

	if (!device_is_ready(temp)) {
		printk("  Temp: present, not ready\n");
	} else {
		struct sensor_value val;
		int                 rc = sensor_sample_fetch_chan(temp, SENSOR_CHAN_AMBIENT_TEMP);

		if (rc == 0) {
			rc = sensor_channel_get(temp, SENSOR_CHAN_AMBIENT_TEMP, &val);
		}
		if (rc == 0) {
			/* Integer milli-degrees C, no float printf -- same
			 * conversion + format as examples/aen/aen-temp-sensor. */
			printk("  Temp: %d milli-degC\n", (int)sensor_value_to_milli(&val));
		} else {
			printk("  Temp: read failed (rc=%d)\n", rc);
		}
	}
#endif
}
#endif /* CONFIG_ALP_SDK_BANNER_HOUSEKEEPING */

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
		alp_print_unit_identity(&info);
		alp_print_sysinfo();
		alp_check_hw_rev_match(&info);
#if defined(CONFIG_ALP_SDK_BANNER_HOUSEKEEPING)
		alp_print_housekeeping();
#endif
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
#if defined(CONFIG_ALP_SDK_BANNER_HOUSEKEEPING)
	alp_print_housekeeping();
#endif
	return 0;
}

/*
 * APPLICATION level so the console (UART or RAM console) is already up;
 * default priority so it prints after device init but before the app's main().
 */
SYS_INIT(alp_sdk_banner, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
