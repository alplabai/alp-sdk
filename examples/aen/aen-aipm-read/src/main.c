/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-aipm-read -- read the SE's live aiPM (Autonomous Intelligent Power
 * Management) RUN and OFF power/clock profiles on the E1M-AEN801 (Ensemble E8,
 * M55-HE) and DECODE each numeric selector into a named legend, via the bench
 * RAM-run + RAM-console flow.
 *
 * WHAT aiPM IS.  On the Ensemble E8 the Secure Enclave (SE) owns the power and
 * clock tree.  A core does not poke PLL / DC-DC / power-domain registers
 * directly; instead the SE keeps two profiles per core and the M55 reads/writes
 * them over the SE-service mailbox:
 *   - the RUN profile  (run_profile_t) -- the active operating point: which HF
 *     clock source feeds the CPU (HFRC / HFXO / PLL), the CPU frequency
 *     selector (60..800 MHz), the AON LF clock (LFRC / LFXO), the DC-DC output
 *     voltage, and which power_domains / memory_blocks stay powered.
 *   - the OFF profile  (off_profile_t) -- the standby/off operating point plus
 *     the wake configuration (wakeup_events, EWIC mask, the VTOR the core
 *     resumes at).
 *
 * WHAT THIS APP DOES.  It is the aiPM-focused, READ-ONLY companion to
 * aen-se-service-query.  It opens the SAME proven SE mailbox path, calls the two
 * aiPM GETTERS, and turns the raw enum ordinals they return into human-readable
 * text (clock-source name, frequency in MHz, the set power-domain / memory-block
 * names, DC-DC mV).  Nothing here changes the operating point.
 *
 * SAFETY -- ONLY READ-ONLY / NON-MUTATING SE SERVICES ARE CALLED HERE:
 *   se_service_heartbeat        -- liveness ping (proves the SE answers)
 *   se_service_get_run_cfg      -- read the current RUN power/clock profile
 *   se_service_get_off_cfg      -- read the OFF/standby power/wake profile
 *
 * DELIBERATELY NOT CALLED (mutating -- a wrong clock/voltage value can brown out
 * the rail, stall the core, or change the wake posture; these need a recovery
 * plan + explicit sign-off before any bench run):
 *   se_service_set_run_cfg / se_service_set_off_cfg   -- CHANGE the operating point
 *   se_service_clock_set_divider / *_set_enable        -- CHANGE the clock tree
 *   se_service_update_stoc / boot_* / *_sleep_req      -- secure boot / lifecycle / sleep
 *
 * Both getters bound their wait inside se_service.c (return 0 / -EAGAIN /
 * -EBUSY / a positive SE error), so the app never hangs.  PASS gate: the SE
 * answers heartbeat + get_run_cfg + get_off_cfg, all rc=0.
 *
 * REFERENCES (transcribed, not vendored -- the Alif DFP is proprietary):
 *   - aiPM types + enums:  Alif DFP se_services/include/aipm.h
 *       run_profile_t / off_profile_t   (aipm.h:482 / aipm.h:500)
 *       lfclock_t  CLK_SRC_LFRC/LFXO     (aipm.h:78)
 *       hfclock_t  CLK_SRC_HFRC/HFXO/PLL (aipm.h:87)
 *       clock_frequency_t 800..60..disabled ordinals (aipm.h:97)
 *       power_domain_t + PD_* aliases    (aipm.h:25 / aipm.h:50)
 *       memory_block_t MB_* (E8/default)  (aipm.h:287)
 *       dcdc_mode_t / ioflex_mode_t       (aipm.h:409 / aipm.h:420)
 *   - SE-service request semantics: Alif DFP se_services/source/services_host_power.c
 *       SERVICES_get_run_cfg -> SERVICE_POWER_GET_RUN_REQ_ID (services_host_power.c:58)
 *       SERVICES_get_off_cfg -> SERVICE_POWER_GET_OFF_REQ_ID (services_host_power.c:133)
 *   - Zephyr client wrappers (Apache-2.0, consumed as a module -- the proven path):
 *       hal_alif se_services/zephyr/include/se_service.h
 *       int se_service_get_run_cfg(run_profile_t *pp)  (se_service.h:186)
 *       int se_service_get_off_cfg(off_profile_t *wp)  (se_service.h:233)
 *     se_service.h pulls aipm.h in via services_lib_api.h:31, so run_profile_t /
 *     off_profile_t and every CLK_SRC_* / PD_* / MB_* / CLOCK_FREQUENCY_* token
 *     below are the vendor enums -- this app does not re-declare them.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>

/* hal_alif SE service client (Apache-2.0).  Brings in se_service_get_run_cfg /
 * se_service_get_off_cfg and, transitively (services_lib_api.h -> aipm.h), the
 * run_profile_t / off_profile_t structs + all the aiPM enums decoded below. */
#include <se_service.h>

/* --------------------------------------------------------------------------
 * Decode helpers -- map the raw aiPM enum ordinals the SE returns to names.
 * Every legend value is transcribed from Alif DFP se_services/include/aipm.h
 * with the line cited inline; this app never invents an ordinal.
 * -------------------------------------------------------------------------- */

/* hfclock_t -- the HF (system) clock source feeding the CPU. aipm.h:87 */
static const char *hfclk_name(int src)
{
	switch (src) {
	case CLK_SRC_HFRC: /* aipm.h:88  = 0 */
		return "HFRC (HF RC oscillator)";
	case CLK_SRC_HFXO: /* aipm.h:89  = 1 */
		return "HFXO (HF crystal oscillator)";
	case CLK_SRC_PLL: /* aipm.h:90  = 2 */
		return "PLL";
	default:
		return "<unknown hfclk>";
	}
}

/* lfclock_t -- the always-on (AON) LF clock source. aipm.h:78 */
static const char *lfclk_name(int src)
{
	switch (src) {
	case CLK_SRC_LFRC: /* aipm.h:79  = 0 */
		return "LFRC (LF RC oscillator)";
	case CLK_SRC_LFXO: /* aipm.h:80  = 1 */
		return "LFXO (LF crystal oscillator)";
	default:
		return "<unknown lfclk>";
	}
}

/*
 * clock_frequency_t -- the CPU frequency SELECTOR is an ordinal, NOT a value in
 * MHz, and its order is non-monotonic (60 MHz is ordinal 7, 100 MHz is ordinal
 * 8).  Transcribed verbatim from aipm.h:97-115.  Returns the MHz-equivalent
 * string; the caller also prints the raw ordinal so an unmapped value is
 * visible.
 */
static const char *cpu_freq_name(int freq)
{
	switch (freq) {
	case CLOCK_FREQUENCY_800MHZ: /* aipm.h:98  = 0 */
		return "800 MHz";
	case CLOCK_FREQUENCY_400MHZ: /* aipm.h:99  = 1 */
		return "400 MHz";
	case CLOCK_FREQUENCY_300MHZ: /* aipm.h:100 = 2 */
		return "300 MHz";
	case CLOCK_FREQUENCY_200MHZ: /* aipm.h:101 = 3 */
		return "200 MHz";
	case CLOCK_FREQUENCY_160MHZ: /* aipm.h:102 = 4 */
		return "160 MHz";
	case CLOCK_FREQUENCY_120MHZ: /* aipm.h:103 = 5 */
		return "120 MHz";
	case CLOCK_FREQUENCY_80MHZ: /* aipm.h:104 = 6 */
		return "80 MHz";
	case CLOCK_FREQUENCY_60MHZ: /* aipm.h:105 = 7 */
		return "60 MHz";
	case CLOCK_FREQUENCY_100MHZ: /* aipm.h:106 = 8  (peripheral-clock range) */
		return "100 MHz";
	case CLOCK_FREQUENCY_50MHZ: /* aipm.h:107 = 9 */
		return "50 MHz";
	case CLOCK_FREQUENCY_20MHZ: /* aipm.h:108 = 10 */
		return "20 MHz";
	case CLOCK_FREQUENCY_10MHZ: /* aipm.h:109 = 11 */
		return "10 MHz";
	case CLOCK_FREQUENCY_76_8_RC_MHZ: /* aipm.h:110 = 12 */
		return "76.8 MHz (RC)";
	case CLOCK_FREQUENCY_38_4_RC_MHZ: /* aipm.h:111 = 13 */
		return "38.4 MHz (RC)";
	case CLOCK_FREQUENCY_76_8_XO_MHZ: /* aipm.h:112 = 14 */
		return "76.8 MHz (XO)";
	case CLOCK_FREQUENCY_38_4_XO_MHZ: /* aipm.h:113 = 15 */
		return "38.4 MHz (XO)";
	case CLOCK_FREQUENCY_DISABLED: /* aipm.h:114 = 16 */
		return "DISABLED";
	default:
		return "<unmapped freq ordinal>";
	}
}

/* dcdc_mode_t -- DC-DC converter operating mode. aipm.h:409 */
static const char *dcdc_mode_name(int mode)
{
	switch (mode) {
	case DCDC_MODE_OFF: /* aipm.h:410 = 0 */
		return "OFF";
	case DCDC_MODE_PFM_AUTO: /* aipm.h:411 = 1 */
		return "PFM_AUTO";
	case DCDC_MODE_PFM_FORCED: /* aipm.h:412 = 2 */
		return "PFM_FORCED";
	case DCDC_MODE_PWM: /* aipm.h:413 = 3 */
		return "PWM";
	default:
		return "<unknown dcdc mode>";
	}
}

/* ioflex_mode_t -- whether the flex GPIO bank runs at 3.3 V or 1.8 V. aipm.h:420 */
static const char *ioflex_name(int io)
{
	switch (io) {
	case IOFLEX_LEVEL_3V3: /* aipm.h:421 = 0 */
		return "3.3V";
	case IOFLEX_LEVEL_1V8: /* aipm.h:422 = 1 */
		return "1.8V";
	default:
		return "<unknown ioflex>";
	}
}

/*
 * Decode the power_domains bitmask into the set domain-alias names.  The bit
 * positions are the power_domain_t ordinals, named via the PD_* aliases.
 * Transcribed from aipm.h:25 (power_domain_t) + aipm.h:50 (PD_* aliases).
 */
static void print_power_domains(uint32_t mask)
{
	static const struct {
		uint32_t    bit;
		const char *name;
	} pd[] = {
		{ PD_VBAT_AON_MASK, "VBAT_AON" },           /* aipm.h:63  bit0 */
		{ PD_SRAM_CTRL_AON_MASK, "SRAM_CTRL_AON" }, /* aipm.h:64  bit1 */
		{ PD_SSE700_AON_MASK, "SSE700_AON" },       /* aipm.h:65  bit2 */
		{ PD_RTSS_HE_MASK, "RTSS_HE" },             /* aipm.h:66  bit3 */
		{ PD_SRAMS_MASK, "SRAMS" },                 /* aipm.h:67  bit4 */
		{ PD_SESS_MASK, "SESS" },                   /* aipm.h:68  bit5 */
		{ PD_SYST_MASK, "SYSTOP" },                 /* aipm.h:69  bit6 */
		{ PD_RTSS_HP_MASK, "RTSS_HP" },             /* aipm.h:70  bit7 */
		{ PD_DBSS_MASK, "DBSS" },                   /* aipm.h:71  bit8 */
		{ PD2_APPS_MASK, "PD2_APPS" },              /* aipm.h:72  bit9 */
	};

	printk("        power_domains  = 0x%08x  [", mask);
	bool first = true;
	for (size_t i = 0U; i < ARRAY_SIZE(pd); i++) {
		if (mask & pd[i].bit) {
			printk("%s%s", first ? "" : " ", pd[i].name);
			first = false;
		}
	}
	if (first) {
		printk("none");
	}
	printk("]\n");
}

/*
 * Decode the memory_blocks bitmask into the retained/powered MB_* names.  The
 * actually-compiled header is the hal_alif module copy
 * (modules/hal/alif/se_services/include/aipm.h); for the E8 (which is NEITHER
 * CONFIG_SOC_SERIES_E1C NOR CONFIG_SOC_SERIES_B1) the #else / DEFAULT
 * memory_block_t variant applies -- bit0=SRAM0 .. bit20=BACKUP4K
 * (module aipm.h:263 enum + aipm.h:288 masks; the same ordinals appear in the
 * DFP reference aipm.h:287/aipm.h:312).  The raw mask is always printed so any
 * SoC-variant divergence is visible.  NOTE: the chosen variant is a build-time
 * #define keyed off CONFIG_SOC_SERIES_* -- see the tbd note in the README.
 */
static void print_memory_blocks(uint32_t mask)
{
	static const struct {
		uint32_t    bit;
		const char *name;
	} mb[] = {
		{ SRAM0_MASK, "SRAM0" },           /* aipm.h:312 bit0 */
		{ SRAM1_MASK, "SRAM1" },           /* aipm.h:313 bit1 */
		{ SRAM2_MASK, "SRAM2" },           /* aipm.h:314 bit2 */
		{ SRAM3_MASK, "SRAM3" },           /* aipm.h:315 bit3 */
		{ SRAM4_1_MASK, "SRAM4_1(ITCM)" }, /* aipm.h:316 bit4 */
		{ SRAM4_2_MASK, "SRAM4_2(ITCM)" }, /* aipm.h:317 bit5 */
		{ SRAM5_1_MASK, "SRAM5_1(DTCM)" }, /* aipm.h:318 bit6 */
		{ SRAM5_2_MASK, "SRAM5_2(DTCM)" }, /* aipm.h:319 bit7 */
		{ SRAM6A_MASK, "SRAM6A" },         /* aipm.h:320 bit8 */
		{ SRAM6B_MASK, "SRAM6B" },         /* aipm.h:321 bit9 */
		{ SRAM7_1_MASK, "SRAM7_1" },       /* aipm.h:322 bit10 */
		{ SRAM7_2_MASK, "SRAM7_2" },       /* aipm.h:323 bit11 */
		{ SRAM7_3_MASK, "SRAM7_3" },       /* aipm.h:324 bit12 */
		{ SRAM8_MASK, "SRAM8" },           /* aipm.h:325 bit13 */
		{ SRAM9_MASK, "SRAM9" },           /* aipm.h:326 bit14 */
		{ MRAM_MASK, "MRAM" },             /* aipm.h:327 bit15 */
		{ OSPI0_MASK, "OSPI0" },           /* aipm.h:328 bit16 */
		{ OSPI1_MASK, "OSPI1" },           /* aipm.h:329 bit17 */
		{ SERAM_MASK, "SERAM" },           /* aipm.h:330 bit18 */
		{ FWRAM_MASK, "FWRAM" },           /* aipm.h:331 bit19 */
		{ BACKUP4K_MASK, "BACKUP4K" },     /* aipm.h:332 bit20 */
	};

	printk("        memory_blocks  = 0x%08x  [", mask);
	bool first = true;
	for (size_t i = 0U; i < ARRAY_SIZE(mb); i++) {
		if (mask & mb[i].bit) {
			printk("%s%s", first ? "" : " ", mb[i].name);
			first = false;
		}
	}
	if (first) {
		printk("none");
	}
	printk("]\n");
}

int main(void)
{
	int  rc;
	bool all_ok = true;

	printk("\n=== aen-aipm-read (read-only aiPM RUN/OFF profile) ===\n");

	/*
	 * 0. Liveness: a heartbeat round-trip proves the SE answers the mailboxes
	 *    before we trust the profile reads.  (se_service.h liveness ping.)
	 */
	rc = se_service_heartbeat();
	printk("heartbeat            : rc=%d\n", rc);
	all_ok &= (rc == 0);

	/*
	 * 1. RUN profile -- the live operating point.  se_service_get_run_cfg
	 *    issues SERVICE_POWER_GET_RUN_REQ_ID under the hood (DFP
	 *    services_host_power.c:58) and fills run_profile_t; nothing is written.
	 */
	run_profile_t run = { 0 };

	rc = se_service_get_run_cfg(&run);
	printk("get_run_cfg          : rc=%d\n", rc);
	all_ok &= (rc == 0);
	if (rc == 0) {
		/* Clock tree: HF source feeds the CPU; LF source is the AON domain. */
		printk("        run_clk_src    = %d  (%s)\n",
		       (int)run.run_clk_src,
		       hfclk_name((int)run.run_clk_src));
		printk("        cpu_clk_freq   = %d  (%s)\n",
		       (int)run.cpu_clk_freq,
		       cpu_freq_name((int)run.cpu_clk_freq));
		printk("        aon_clk_src    = %d  (%s)\n",
		       (int)run.aon_clk_src,
		       lfclk_name((int)run.aon_clk_src));
		printk("        scaled_clk_freq= %d\n", (int)run.scaled_clk_freq);

		/* Rail: DC-DC output voltage (mV) + converter mode. */
		printk("        dcdc_voltage   = %u mV  (mode %d=%s)\n",
		       run.dcdc_voltage,
		       (int)run.dcdc_mode,
		       dcdc_mode_name((int)run.dcdc_mode));

		/* Domains + memory kept powered in this operating point. */
		print_power_domains(run.power_domains);
		print_memory_blocks(run.memory_blocks);

		/* Clock/PHY gating + flex-GPIO rail. */
		printk("        ip_clock_gating= 0x%08x  phy_pwr_gating=0x%08x\n",
		       run.ip_clock_gating,
		       run.phy_pwr_gating);
		printk("        vdd_ioflex_3V3 = %d  (%s)\n",
		       (int)run.vdd_ioflex_3V3,
		       ioflex_name((int)run.vdd_ioflex_3V3));
	}

	/*
	 * 2. OFF profile -- the standby/off operating point + wake configuration.
	 *    se_service_get_off_cfg issues SERVICE_POWER_GET_OFF_REQ_ID (DFP
	 *    services_host_power.c:133) and fills off_profile_t; nothing is written.
	 */
	off_profile_t off = { 0 };

	rc = se_service_get_off_cfg(&off);
	printk("get_off_cfg          : rc=%d\n", rc);
	all_ok &= (rc == 0);
	if (rc == 0) {
		/* Standby clock tree: stby HF source + AON LF source. */
		printk("        stby_clk_src   = %d  (%s)\n",
		       (int)off.stby_clk_src,
		       hfclk_name((int)off.stby_clk_src));
		printk("        aon_clk_src    = %d  (%s)\n",
		       (int)off.aon_clk_src,
		       lfclk_name((int)off.aon_clk_src));
		printk("        stby_clk_freq  = %d\n", (int)off.stby_clk_freq);

		/* Rail in standby. */
		printk("        dcdc_voltage   = %u mV  (mode %d=%s)\n",
		       off.dcdc_voltage,
		       (int)off.dcdc_mode,
		       dcdc_mode_name((int)off.dcdc_mode));

		/* Domains + memory retained across standby. */
		print_power_domains(off.power_domains);
		print_memory_blocks(off.memory_blocks);

		/* Wake configuration: which events wake the SoC + where the core resumes.
		 * (wakeup_events / ewic_cfg bit legends are in aipm.h:335 / aipm.h:357;
		 * we print the raw masks -- the per-bit decode is left to the wake-source
		 * example to keep this app focused on the clock/power view.) */
		printk(
		    "        wakeup_events  = 0x%08x  ewic_cfg=0x%08x\n", off.wakeup_events, off.ewic_cfg);
		printk("        vtor_address   = 0x%08x  vtor_address_ns=0x%08x\n",
		       off.vtor_address,
		       off.vtor_address_ns);
		printk("        vdd_ioflex_3V3 = %d  (%s)\n",
		       (int)off.vdd_ioflex_3V3,
		       ioflex_name((int)off.vdd_ioflex_3V3));
	}

	if (all_ok) {
		printk("RESULT PASS: SE returned the aiPM RUN+OFF profiles read-only "
		       "(RUN clk=%s @ %s, %u mV; OFF clk=%s, %u mV) -- no set_run_cfg / "
		       "set_off_cfg called\n",
		       hfclk_name((int)run.run_clk_src),
		       cpu_freq_name((int)run.cpu_clk_freq),
		       run.dcdc_voltage,
		       hfclk_name((int)off.stby_clk_src),
		       off.dcdc_voltage);
	} else {
		printk("RESULT FAIL: the SE did not answer one of heartbeat / get_run_cfg "
		       "/ get_off_cfg with rc=0 (SE asleep/unreachable on this boot, or a "
		       "service unsupported) -- see the per-call rc above\n");
	}

	return 0;
}
