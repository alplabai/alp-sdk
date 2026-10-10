/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-power-stop -- bench proof of the Alif SE STOP backend (#2784, unit U7) on
 * the E1M-AEN803 (Alif Ensemble E8, M55-HE), built as an MRAM image.
 *
 * STOP passed on silicon (U8h); STANDBY is untested.  Back up the MRAM image before flashing this (Flow D
 * backup / restore): if the entry sequence is wrong the module can sit with the
 * Secure Enclave until it is power-cycled.
 *
 *
 * ==== WHAT STOP IS ON THIS PART ======================================
 *
 * The M55-HE subsystem is powered OFF and only the always-on island stays alive.
 * The wake is a COLD BOOT through the Secure Enclave (SES -> ATOC -> this image),
 * so alp_power_request_sleep(STOP) never returns: this program is run again from
 * main() on every wake, and the only things that survive are
 *
 *   - the SDK's wake record in the 4 KB Utility SRAM ("BKRAM"), which the SDK
 *     decodes before main() runs and hands back through alp_power_boot_wake_info();
 *   - a bench counter in the same SRAM (CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH).
 *     BKRAM is SDK-reserved: the counter exists only so this bench can prove the
 *     SRAM was retained, and it is absent from a product build.
 *
 * Because the program restarts, the "loop" below is not a loop at all.  Each boot
 * reads the counter to learn which cycle it is, judges the cycle that just ended,
 * and starts the next one.
 *
 *
 * ==== PRODUCT CONFIGURATION (variants/product-noscratch.conf) ========
 *
 * Without CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH there is no bench cell and no diag: the
 * cycle number is kept in the RV-3028's User RAM 1, every verdict comes from
 * alp_power_boot_wake_info(), and each boot prints the UART's configured baud, the CGU PLL
 * registers and a tick-rate check against two RV-3028 seconds (verdict "tick_rate").
 *
 *
 * ==== THE THREE CYCLES ==============================================
 *
 *   cycle  wake source armed                    expected wake_source
 *   -----  -----------------------------------  --------------------
 *     1    LPTIMER, 500 ms                      ALP_POWER_WAKE_TIMER
 *     2    RV-3028 countdown, 3 s               ALP_POWER_WAKE_RTC
 *     3    RV-3028 alarm (next minute change)   ALP_POWER_WAKE_RTC
 *
 * A timed wake under 1 s uses the LPTIMER; from 1 s up it uses the RV-3028
 * countdown, because the internal low-frequency clock is not trustworthy (the SES
 * leaves it on the ~4.5 % fast ring oscillator) while the RV-3028 is +-1 ppm.  The
 * alarm in cycle 3 is armed by this app through the chip driver; the backend only
 * checks that it really is armed before it lets the SoC sleep on it.
 *
 * Each cycle starts with 10 s awake, so a probe or a console can attach before the
 * module goes down.  A probe that is still attached makes the backend refuse the
 * sleep with ALP_ERR_BUSY (the subsystem would not power down), and the probe leaves
 * DHCSR.C_DEBUGEN set after it detaches: clear it from the debugger first (README).
 *
 *
 * ==== BENCH CONTRACT ================================================
 *
 * One stable line per check, grep-able:
 *
 *     POWER_STOP: cycle<n> <check> <PASS|FAIL>
 *     POWER_STOP: SUMMARY cycles=<n> pass=<n> fail=<n>
 *
 * checks per cycle, printed after the wake that ended it:
 *     record_valid  the SDK's wake record survived and decoded
 *     mode_stop     the record says STOP
 *     wake_source   the source that fired is the one armed
 *     restored_all  every domain quiesced before the sleep was restored, none failed
 *     bkram_counter the bench counter read back what was written before the sleep
 *
 * Evidence lines (no verdict): POWER_STOP: boot ..., POWER_STOP: wake ...,
 * POWER_STOP: regs ....  Keep these strings byte-for-byte.
 *
 *
 * ==== THREE VARIANTS NOT YET RUN ON SILICON (#2784) ===================
 *
 * Each is a config fragment (variants/<name>.conf); the default image is unchanged.
 *
 *   CONFIG_AEN_STOP_MODE_STANDBY   every cycle sleeps in ALP_POWER_MODE_STANDBY; the
 *                                  check "mode_stop" becomes "mode_standby" (realised_mode
 *                                  must read STANDBY).
 *   CONFIG_AEN_STOP_RETAIN_TCM_KB  ALP_POWER_RETAIN_TCM with that retain_kb; a CRC-checked
 *                                  pattern in the DTCM is written before the sleep and
 *                                  checked after the wake: check "tcm_retained", evidence
 *                                  "POWER_STOP: tcm ...".
 *   CONFIG_AEN_STOP_WAKE_TIMING    "POWER_STOP: timing ..." lines: the LPRTC counter at
 *                                  sleep entry, at PRE_KERNEL_1 and at main(), and the
 *                                  wake-to-main() figure derived from them.
 *
 * All three are code-complete and unverified until a bench run reads those lines.
 *
 * A cold power cycle (not a reset) starts the sequence over: the counter lives in
 * SRAM that a reset keeps and a power cycle loses.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

/* Deliberate for a bench app: the SE profile dump below calls hal_alif's read-only
 * se_service_get_run_cfg() / get_off_cfg() directly.  Application code uses <alp/power.h>. */
#include <se_service.h>

#include <alp/chips/rv3028c7.h>
#include <alp/peripheral.h>
#include <alp/power.h>

#include "alif_se_power_hw.h" /* LPRTC counter read (SDK-internal) */
#include "som_power.h"        /* bench counter (SDK-internal) */
#include "som_power_chips.h"  /* alp_som_power_bind_rv3028() (SDK-internal) */

#define N_CYCLES 3
#define AWAKE_MS 10000u

/* The sleep mode every cycle requests, and the name of the check that judges it.  STOP is the
 * default and bench-proven; the STANDBY variant swaps both (verdict "mode_standby"). */
#ifdef CONFIG_AEN_STOP_MODE_STANDBY
#define SLEEP_MODE      ALP_POWER_MODE_STANDBY
#define SLEEP_MODE_NAME "STANDBY"
#define MODE_CHECK_NAME "mode_standby"
#else
#define SLEEP_MODE      ALP_POWER_MODE_STOP
#define SLEEP_MODE_NAME "STOP"
#define MODE_CHECK_NAME "mode_stop"
#endif

/* BRD_I2C, the on-module housekeeping bus the RV-3028 sits on (portable bus 2). */
#define BRD_I2C_BUS 2u

typedef struct {
	const char *what;          /* printed in the evidence line */
	uint32_t    wake_bitmap;   /* alp_power_configure_wake_source() */
	uint32_t    wake_after_ms; /* alp_power_request_sleep() */
	uint32_t    expect;        /* the wake_source bit that must come back */
	bool        arm_alarm;     /* arm the RV-3028 alarm for the next minute first */
} cycle_t;

/* The three cycles.  The bench variants (variants/<name>.conf, one variable each) reorder or
 * retime them: CONFIG_AEN_STOP_FIRST_RTC runs the RV-3028 countdown first,
 * CONFIG_AEN_STOP_LPTIMER_MS sets the LPTIMER interval. */
#define LPT_CYCLE \
	{ "LPTIMER " STRINGIFY(CONFIG_AEN_STOP_LPTIMER_MS) " ms", \
	  ALP_POWER_WAKE_TIMER, \
	  CONFIG_AEN_STOP_LPTIMER_MS, \
	  ALP_POWER_WAKE_TIMER, \
	  false }
#define RTC_CYCLE \
	{ "RV-3028 countdown " STRINGIFY(CONFIG_AEN_STOP_RTC_MS) " ms", \
	  ALP_POWER_WAKE_RTC, \
	  CONFIG_AEN_STOP_RTC_MS, \
	  ALP_POWER_WAKE_RTC, \
	  false }

/* The countdown is the RV-3028's only if the backend does not route it to the LPTIMER. */
BUILD_ASSERT(CONFIG_AEN_STOP_RTC_MS >= 1000, "the countdown cycle must be at least 1 s");
#define ALARM_CYCLE { "RV-3028 alarm", ALP_POWER_WAKE_RTC, 0u, ALP_POWER_WAKE_RTC, true }

static const cycle_t cycles[N_CYCLES] = {
#ifdef CONFIG_AEN_STOP_FIRST_RTC
	RTC_CYCLE,
	LPT_CYCLE,
	ALARM_CYCLE,
#else
	LPT_CYCLE,
	RTC_CYCLE,
	ALARM_CYCLE,
#endif
};

/* Must outlive main(): the SDK keeps this pointer for the RTC power-domain hook and
 * the countdown. */
static rv3028c7_t g_rtc;

static unsigned g_pass, g_fail;

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
/* Bench build: the cycle number lives in the bench cell in BKRAM. */
static unsigned cycle_get(void)
{
	return alp_som_pd_bench_count();
}

static void cycle_set(unsigned n)
{
	alp_som_pd_bench_set(n);
}
#else
/* Product build (no bench cell, no diag): the cycle number lives in the RV-3028's User RAM 1
 * (register 1Fh, kept on the backup supply), tagged 0xC0 | n so a stale or never-written value
 * reads as 0.  The SDK's own wake record is what proves each wake. */
#define RTC_USER_RAM1 0x1Fu
#define CYCLE_TAG     0xC0u

static unsigned cycle_get(void)
{
	uint8_t reg = RTC_USER_RAM1;
	uint8_t v   = 0u;

	if (alp_i2c_write_read(g_rtc.bus, RV3028C7_I2C_ADDR, &reg, 1, &v, 1) != ALP_OK ||
	    (v & 0xF0u) != CYCLE_TAG) {
		return 0u;
	}
	return v & 0x0Fu;
}

static void cycle_set(unsigned n)
{
	uint8_t buf[2] = { RTC_USER_RAM1, (uint8_t)(CYCLE_TAG | (n & 0x0Fu)) };

	(void)alp_i2c_write(g_rtc.bus, RV3028C7_I2C_ADDR, buf, sizeof(buf));
}
#endif

static void verdict(unsigned cycle, const char *check, bool pass)
{
	printk("POWER_STOP: cycle%u %s %s\n", cycle, check, pass ? "PASS" : "FAIL");
	if (pass) {
		g_pass++;
	} else {
		g_fail++;
	}
}

/* The Secure Enclave firmware identity (bench U8e: which SES the wake path was measured on). */
static void print_ses_version(void)
{
	uint8_t  rev[80] = { 0 }; /* VERSION_RESPONSE_LENGTH */
	uint32_t toc     = 0u;
	int      rrc     = se_service_get_se_revision(rev);
	int      trc     = se_service_get_toc_version(&toc);

	rev[sizeof(rev) - 1u] = '\0';
	printk("POWER_STOP: ses revision rc=%d \"%s\" toc_version rc=%d 0x%08x\n",
	       rrc,
	       rrc == 0 ? (const char *)rev : "",
	       trc,
	       (unsigned)toc);
}

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
static void print_knobs(void)
{
	printk("POWER_STOP: knobs vtor_self=%d mram_seram=%d lfxo=%d stby_76_8=%d\n",
	       (int)alp_som_bench_knobs.vtor_self,
	       (int)alp_som_bench_knobs.mram_seram,
	       (int)alp_som_bench_knobs.lfxo,
	       (int)alp_som_bench_knobs.stby_76_8);
}

#endif

/* Raw always-on registers, as evidence for the bench record.  The first boot after
 * the SE cold start is the baseline; the ones after a wake show what the SE left. */
static void print_regs(void)
{
	printk("POWER_STOP: regs RET_CTRL=0x%08x VBAT_ANA_REG1=0x%08x ANA_MISC=0x%08x "
	       "STOP_MODE=0x%08x RTSS_HE_CTRL=0x%08x\n",
	       sys_read32(DT_REG_ADDR(DT_NODELABEL(vbat)) + 0x0Cu),
	       sys_read32(DT_REG_ADDR(DT_NODELABEL(ana)) + 0x38u),
	       sys_read32(DT_REG_ADDR(DT_NODELABEL(ana)) + 0x00u),
	       sys_read32(DT_REG_ADDR(DT_NODELABEL(stop_mode))),
	       sys_read32(0x1A604010u)); /* AON.RTSS_HE_CTRL: bit0 COLD_WAKEUP, [9:8] WIC */
}

/* VBAT_STOP_MODE_REG (0x1A60F000), decoded: the hardware's own witness, independent of
 * the SDK's record.  STOP_MODE_STAT (bit 4) says the last reset was a STOP wake;
 * DC_DC_STAT (bit 8) the DC-DC state the SE left (SVD: 1 = off, 0 = on); STOP_MODE_CTRL
 * (bit 0).  Printed on every boot, so the first line (before cycle 1) is the baseline
 * and the ones after a wake are the evidence. */
static void print_stop_mode(void)
{
	uint32_t v = sys_read32(DT_REG_ADDR(DT_NODELABEL(stop_mode)));

	printk("POWER_STOP: stop_mode_reg=0x%08x STOP_MODE_STAT=%u DC_DC_STAT=%u STOP_MODE_CTRL=%u\n",
	       v,
	       (unsigned)((v >> 4) & 1u),
	       (unsigned)((v >> 8) & 1u),
	       (unsigned)(v & 1u));
}

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
/* The two BKRAM register snapshots (alif_se_power_hw.c lists what each word is):
 *   PRE  = written with interrupts off immediately before the WFI of the last sleep;
 *   BOOT = written by the earliest init hook of THIS boot (PRE_KERNEL_1), before the SoM
 *          restore, any driver init and the console.
 * Raw words, no interpretation: the bench reads them against the register list. */
static void print_diag(const char *name, unsigned slot)
{
	alp_som_pd_diag_t d;

	if (!alp_som_pd_diag_load(slot, &d)) {
		printk("POWER_STOP: diag %s empty\n", name);
		return;
	}
	printk("POWER_STOP: diag %s seq=%u cycle=%u\n", name, (unsigned)d.seq, (unsigned)d.cycle);
	for (unsigned i = 0; i < ALP_SOM_PD_DIAG_WORDS; i += 4u) {
		printk("POWER_STOP: diag %s w[%02u..%02u]=%08x %08x %08x %08x\n",
		       name,
		       i,
		       i + 3u,
		       (unsigned)d.w[i],
		       (unsigned)d.w[i + 1u],
		       (unsigned)d.w[i + 2u],
		       (unsigned)d.w[i + 3u]);
	}
}
#endif

/* Every field of the SE's RUN and OFF profiles as it stands now (read-only getters), so
 * the clock tree the SE left after the wake can be compared with the cold-boot profile
 * (power_domains 0x16d, dcdc 825 PWM, LFRC, PLL, 160 MHz, memory_blocks 0x108000). */
static void print_se_profiles(void)
{
	run_profile_t run;
	off_profile_t off;

	memset(&run, 0, sizeof(run));
	memset(&off, 0, sizeof(off));
	int rrc = se_service_get_run_cfg(&run);
	int orc = se_service_get_off_cfg(&off);

	printk("POWER_STOP: se run rc=%d power_domains=0x%x dcdc_mv=%u dcdc_mode=%d aon_clk=%d "
	       "run_clk=%d cpu_clk=%d scaled=%d mem=0x%x ipclk=0x%x phy=0x%x ioflex=%d\n",
	       rrc,
	       (unsigned)run.power_domains,
	       (unsigned)run.dcdc_voltage,
	       (int)run.dcdc_mode,
	       (int)run.aon_clk_src,
	       (int)run.run_clk_src,
	       (int)run.cpu_clk_freq,
	       (int)run.scaled_clk_freq,
	       (unsigned)run.memory_blocks,
	       (unsigned)run.ip_clock_gating,
	       (unsigned)run.phy_pwr_gating,
	       (int)run.vdd_ioflex_3V3);
	/* dcdc_mode is not carried by hal_alif's OFF getter: it reads 0 here, not the SE's. */
	printk("POWER_STOP: se off rc=%d power_domains=0x%x dcdc_mv=%u aon_clk=%d stby_clk=%d "
	       "stby_freq=%d mem=0x%x ipclk=0x%x phy=0x%x ioflex=%d wake=0x%x ewic=0x%x "
	       "vtor=0x%x vtor_ns=0x%x\n",
	       orc,
	       (unsigned)off.power_domains,
	       (unsigned)off.dcdc_voltage,
	       (int)off.aon_clk_src,
	       (int)off.stby_clk_src,
	       (int)off.stby_clk_freq,
	       (unsigned)off.memory_blocks,
	       (unsigned)off.ip_clock_gating,
	       (unsigned)off.phy_pwr_gating,
	       (int)off.vdd_ioflex_3V3,
	       (unsigned)off.wakeup_events,
	       (unsigned)off.ewic_cfg,
	       (unsigned)off.vtor_address,
	       (unsigned)off.vtor_address_ns);
}

#if CONFIG_AEN_STOP_RETAIN_TCM_KB > 0
/* ==== TCM retention probe (variant T) =================================================
 *
 * Zephyr's whole RAM is the M55-HE DTCM (zephyr,sram = &dtcm), 256 KiB at CPU-local
 * 0x20000000 (DFP AE822FA0E5597 core_defines.h DTCM_BASE, soc_features.h:59
 * SOC_FEAT_HE_DTCM_SIZE 0x00040000).  The SE's memory-retention bitmap splits it into two
 * 128 KiB banks, SRAM5_1 and SRAM5_2 (hal_alif se_services/include/aipm.h:207-208, "M55-HE
 * DTCM RET1 / RET2 dtcm 128kb"); the backend claims them in that order.  Which half of the
 * address range each bit covers, and that SRAM5_x maps onto RET_CTRL.HETCM_RET1/2, are NOT
 * proven: this probe is how the bench finds out.
 *
 * The probe is a .noinit array, so the C runtime neither copies nor zeroes it on the cold boot
 * the wake goes through: whatever the DTCM kept is what the check reads.  It is larger than one
 * bank so it straddles the bank boundary wherever the linker puts it: part A is the bytes
 * below base + 128 KiB (bank 0), part B the bytes above (bank 1).
 */
#define TCM_BANK_BYTES  (128u * 1024u)
#define TCM_PROBE_BYTES (160u * 1024u)
#define TCM_BANKS       2u
#define TCM_CHUNK_WORDS 64u

BUILD_ASSERT(CONFIG_SRAM_SIZE * 1024 >= TCM_BANKS * TCM_BANK_BYTES,
             "the probe assumes the 256 KiB M55-HE DTCM is the whole of RAM");
BUILD_ASSERT(CONFIG_AEN_STOP_RETAIN_TCM_KB <= TCM_BANKS * 128,
             "the DTCM is two 128 KiB banks; the ITCM is not probed by this app");

static uint32_t tcm_probe[TCM_PROBE_BYTES / 4u] __noinit;

/* The pattern word at index @p i for cycle @p seed: a different pattern every cycle, so memory
 * left over from the previous cycle (or the previous run) can never read as retained. */
static uint32_t tcm_word(unsigned seed, uint32_t i)
{
	return ((seed + 1u) * 0x9E3779B1u) ^ (i * 0x85EBCA6Bu + 0x5BD1E995u);
}

/* Part A / B of the probe as word counts, from where the linker put it.  False when the array
 * does not straddle the bank boundary (cannot happen with 160 KiB in 256 KiB, but a silent
 * wrong answer is worse than a printed one). */
static bool tcm_parts(uint32_t *a_words, uint32_t *b_words)
{
	uintptr_t first = (uintptr_t)tcm_probe;
	uintptr_t split = (uintptr_t)CONFIG_SRAM_BASE_ADDRESS + TCM_BANK_BYTES;

	if (first >= split || first + TCM_PROBE_BYTES <= split) {
		return false;
	}
	*a_words = (uint32_t)((split - first) / 4u);
	*b_words = (uint32_t)(TCM_PROBE_BYTES / 4u) - *a_words;
	return true;
}

/* CRC-32 of pattern words [first, first + n), generated in chunks rather than stored: the
 * expected value must not live in memory that the test is about to lose. */
static uint32_t tcm_expected_crc(unsigned seed, uint32_t first, uint32_t n)
{
	uint32_t buf[TCM_CHUNK_WORDS];
	uint32_t crc = 0u;

	for (uint32_t done = 0u; done < n;) {
		uint32_t k = MIN(n - done, TCM_CHUNK_WORDS);

		for (uint32_t j = 0u; j < k; ++j) {
			buf[j] = tcm_word(seed, first + done + j);
		}
		crc = crc32_ieee_update(crc, (const uint8_t *)buf, k * 4u);
		done += k;
	}
	return crc;
}

static uint32_t tcm_memory_crc(uint32_t first, uint32_t n)
{
	return crc32_ieee_update(0u, (const uint8_t *)&tcm_probe[first], n * 4u);
}

/* Write cycle @p seed's pattern over the whole probe, before the sleep. */
static void tcm_fill(unsigned seed)
{
	uint32_t a, b;

	for (uint32_t i = 0u; i < TCM_PROBE_BYTES / 4u; ++i) {
		tcm_probe[i] = tcm_word(seed, i);
	}
	if (!tcm_parts(&a, &b)) {
		printk("POWER_STOP: tcm cycle%u layout FAIL addr=0x%08x\n",
		       seed,
		       (unsigned)(uintptr_t)tcm_probe);
		return;
	}
	printk("POWER_STOP: tcm cycle%u wrote retain_kb=%u addr=0x%08x bank0_bytes=%u bank1_bytes=%u "
	       "crc_a=0x%08x crc_b=0x%08x\n",
	       seed,
	       (unsigned)CONFIG_AEN_STOP_RETAIN_TCM_KB,
	       (unsigned)(uintptr_t)tcm_probe,
	       (unsigned)(a * 4u),
	       (unsigned)(b * 4u),
	       (unsigned)tcm_expected_crc(seed, 0u, a),
	       (unsigned)tcm_expected_crc(seed, a, b));
}

/* After the wake: did each bank's part of the pattern survive?  The verdict covers the banks
 * the request asked for (retain_kb rounded UP to 128 KiB banks, DTCM RET1 first); a bank that
 * was not asked for is evidence only -- "retained" there means it kept its content anyway. */
static void tcm_check(unsigned cycle)
{
	uint32_t a = 0u, b = 0u;
	bool     layout = tcm_parts(&a, &b);
	bool     ok0    = layout && tcm_memory_crc(0u, a) == tcm_expected_crc(cycle, 0u, a);
	bool     ok1    = layout && tcm_memory_crc(a, b) == tcm_expected_crc(cycle, a, b);
	unsigned asked  = (CONFIG_AEN_STOP_RETAIN_TCM_KB + 127u) / 128u;

	printk("POWER_STOP: tcm cycle%u read asked_banks=%u bank0=%s bank1=%s\n",
	       cycle,
	       asked,
	       ok0 ? "retained" : "lost",
	       ok1 ? "retained" : "lost");
	verdict(cycle, "tcm_retained", layout && ok0 && (asked < 2u || ok1));
}
#endif /* CONFIG_AEN_STOP_RETAIN_TCM_KB */

#ifdef CONFIG_AEN_STOP_WAKE_TIMING
/* ==== Wake-to-main() timing evidence (variant W) =======================================
 *
 * The counter that keeps running through STOP and the SE cold boot is the LPRTC (VBAT domain,
 * base 0x42000000: DFP AE822FA0E5597 rtss_he/soc.h LPRTC_Type; its clock is gated by VBAT
 * RTC_CLK_EN, +0x10, which the SVD says "must be set before any programming to LPRTC").  Its
 * prescaler CPSR (+0x20) resets to 0x8000 and "by default, the counter increments at a 1 Hz
 * rate when the prescaler is enabled and precise 32.768 kHz clock source is used" (SVD
 * LPRTC_CPSR).  Here the source is the SES-left LFRC, not a precise 32.768 kHz, and the bench
 * counted ~2 Hz (155 ticks in 76 s), so the tick period is only known nominally; CCR and CPSR
 * are printed raw so the bench can settle it.
 *
 * Three samples of the same counter:
 *   pre   BKRAM diag PRE word 8,  taken with interrupts off right before the WFI;
 *   boot  BKRAM diag BOOT word 8, taken at PRE_KERNEL_1 of the wake boot;
 *   main  the first thing main() does.
 * The wake event itself is not timestamped.  It happens at entry + armed_ms (the LPTIMER
 * interval or the RV-3028 countdown), so wake-to-main = (main - pre) - armed_ms, good to a
 * couple of ticks.  The RV-3028 alarm cycle has no armed length and prints n/a.  This is a
 * bound with one-tick resolution at each end, not a measurement.
 */
#define LPRTC_BASE       0x42000000u
#define LPRTC_CCR        (LPRTC_BASE + 0x0Cu)
#define LPRTC_CPSR       (LPRTC_BASE + 0x20u)
#define LPRTC_CPCVR      (LPRTC_BASE + 0x24u)
#define LPRTC_NOMINAL_HZ 32768u /* the clock CPSR is specified against (SVD LPRTC_CPSR) */

static uint32_t g_main_ccvr;
static int64_t  g_main_uptime_ms;

static void timing_sample_main(void)
{
	g_main_ccvr      = alif_se_hw_lprtc_ccvr(); /* also switches the LPRTC clock on */
	g_main_uptime_ms = k_uptime_get();
}

static void print_wake_timing(unsigned cycle, const alp_power_boot_info_t *bi)
{
	alp_som_pd_diag_t pre, boot;

	if (!alp_som_pd_diag_load(ALP_SOM_PD_DIAG_PRE, &pre) ||
	    !alp_som_pd_diag_load(ALP_SOM_PD_DIAG_BOOT, &boot)) {
		printk("POWER_STOP: timing cycle%u unavailable (no PRE or BOOT snapshot)\n", cycle);
		return;
	}

	const cycle_t *c         = &cycles[cycle - 1u];
	uint32_t       cpsr      = sys_read32(LPRTC_CPSR);
	uint32_t       ticks_all = g_main_ccvr - pre.w[8];
	uint32_t       ticks_pb  = boot.w[8] - pre.w[8];
	uint32_t       ticks_bm  = g_main_ccvr - boot.w[8];
	uint64_t       nom_ms    = (uint64_t)ticks_all * cpsr * 1000u / LPRTC_NOMINAL_HZ;

	printk("POWER_STOP: timing cycle%u lprtc pre=%u boot=%u main=%u ccr=0x%x cpsr=%u cpcvr=%u "
	       "armed_ms=%u main_uptime_ms=%d\n",
	       cycle,
	       (unsigned)pre.w[8],
	       (unsigned)boot.w[8],
	       (unsigned)g_main_ccvr,
	       (unsigned)sys_read32(LPRTC_CCR),
	       (unsigned)cpsr,
	       (unsigned)sys_read32(LPRTC_CPCVR),
	       (unsigned)c->wake_after_ms,
	       (int)g_main_uptime_ms);
	printk("POWER_STOP: timing cycle%u ticks entry_to_boot=%u boot_to_main=%u entry_to_main=%u "
	       "tick_ms_nominal=%u\n",
	       cycle,
	       (unsigned)ticks_pb,
	       (unsigned)ticks_bm,
	       (unsigned)ticks_all,
	       (unsigned)((uint64_t)cpsr * 1000u / LPRTC_NOMINAL_HZ));
	if (c->wake_after_ms == 0u) {
		printk("POWER_STOP: timing cycle%u wake_to_main_nominal_ms=n/a (no armed length)\n", cycle);
		return;
	}
	printk("POWER_STOP: timing cycle%u entry_to_main_nominal_ms=%u wake_to_main_nominal_ms=%d "
	       "rtc_slept_ms=%u\n",
	       cycle,
	       (unsigned)nom_ms,
	       (int)((int64_t)nom_ms - (int64_t)c->wake_after_ms),
	       (unsigned)bi->slept_ms);
}
#endif /* CONFIG_AEN_STOP_WAKE_TIMING */

/* Judge the cycle that ended with this boot.  @p done is how many STOPs were
 * started before it (the bench counter), so the cycle is cycles[done - 1]. */
static void judge(unsigned done, const alp_power_boot_info_t *bi)
{
	const cycle_t *c = &cycles[done - 1u];

	printk("POWER_STOP: wake cycle%u (%s) valid=%d mode=%d wake_source=0x%x slept_ms=%u "
	       "quiesced=0x%x restored=0x%x failed=0x%x\n",
	       done,
	       c->what,
	       (int)bi->valid,
	       (int)bi->realised_mode,
	       (unsigned)bi->wake_source,
	       (unsigned)bi->slept_ms,
	       (unsigned)bi->quiesced_domains,
	       (unsigned)bi->restored_domains,
	       (unsigned)bi->restore_failed_domains);

	verdict(done, "record_valid", bi->valid);
	verdict(done, MODE_CHECK_NAME, bi->valid && bi->realised_mode == SLEEP_MODE);
	verdict(done, "wake_source", bi->valid && bi->wake_source == c->expect);
	verdict(done,
	        "restored_all",
	        bi->valid && bi->quiesced_domains == bi->restored_domains &&
	            bi->restore_failed_domains == 0u);
	/* Reaching this line with the counter at @p done is the retention proof: it was
	 * written before the sleep and nothing else writes it. */
#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
	verdict(done, "bkram_counter", cycle_get() == done);
#else
	verdict(done, "cycle_counter", cycle_get() == done);
#endif
#if CONFIG_AEN_STOP_RETAIN_TCM_KB > 0
	tcm_check(done);
#endif
}

/* Arm the RV-3028 alarm for the next minute change and leave INT -> P15_0 enabled.
 * The backend refuses an RTC-only wake whose alarm is not armed, so a failure here
 * is reported rather than slept on. */
static bool arm_next_minute_alarm(void)
{
	rv3028c7_time_t now;

	if (rv3028c7_get_time(&g_rtc, &now) != ALP_OK) {
		return false;
	}
	rv3028c7_time_t when = now;

	when.minute                  = (uint8_t)((now.minute + 1u) % 60u);
	rv3028c7_alarm_match_t match = { .match_minute = true };

	printk("POWER_STOP: alarm armed for minute %u (now %02u:%02u:%02u)\n",
	       (unsigned)when.minute,
	       (unsigned)now.hour,
	       (unsigned)now.minute,
	       (unsigned)now.second);
	return rv3028c7_alarm_arm(&g_rtc, &when, &match) == ALP_OK;
}

/* Start cycle number @p n (1-based).  Returns only if the sleep did not happen. */
static void start_cycle(unsigned n)
{
	const cycle_t *c = &cycles[n - 1u];

	printk("POWER_STOP: awake %u ms before cycle%u (%s)\n", (unsigned)AWAKE_MS, n, c->what);
	for (unsigned s = 0; s < AWAKE_MS / 1000u; ++s) {
		k_sleep(K_SECONDS(1));
		printk("POWER_STOP: awake %u/%u\n", s + 1u, (unsigned)(AWAKE_MS / 1000u));
	}

	if (c->arm_alarm && !arm_next_minute_alarm()) {
		printk("POWER_STOP: cycle%u arm_alarm FAIL\n", n);
		g_fail++;
		return;
	}

	alp_power_t *p = alp_power_open();

	if (p == NULL) {
		printk("POWER_STOP: cycle%u open FAIL err=%d\n", n, (int)alp_last_error());
		g_fail++;
		return;
	}
	printk("POWER_STOP: wake_capabilities=0x%x\n", (unsigned)alp_power_wake_capabilities(p));

	/* No application RAM is kept: the SDK-reserved BKRAM is the floor.  Variant T asks for
	 * the DTCM banks instead (retain_kb rounds UP to whole 128 KiB banks). */
	alp_power_retain_t keep = { .level = ALP_POWER_RETAIN_NONE };
#if CONFIG_AEN_STOP_RETAIN_TCM_KB > 0
	keep.level     = ALP_POWER_RETAIN_TCM;
	keep.retain_kb = CONFIG_AEN_STOP_RETAIN_TCM_KB;
#endif
	alp_status_t rc = alp_power_configure_retention(p, &keep);

	if (rc == ALP_OK) {
		rc = alp_power_configure_wake_source(p, c->wake_bitmap);
	}
	if (rc != ALP_OK) {
		printk("POWER_STOP: cycle%u configure FAIL rc=%d\n", n, (int)rc);
		g_fail++;
		alp_power_close(p);
		return;
	}

	/* The counter is written BEFORE the sleep: this boot cannot know whether the
	 * next one happens. */
	cycle_set(n);
#if CONFIG_AEN_STOP_RETAIN_TCM_KB > 0
	tcm_fill(n); /* last, so nothing the sleep entry needs is written after the pattern */
#endif
	printk("POWER_STOP: cycle%u enter %s (%s)\n", n, SLEEP_MODE_NAME, c->what);
	k_msleep(100); /* let the UART FIFO drain before the clocks go away */

	alp_power_wake_info_t wi = { 0 };

	rc = alp_power_request_sleep(p, SLEEP_MODE, c->wake_after_ms, &wi);

	/* Reaching here means the sleep did not happen (a refusal) or was cut short. */
	printk("POWER_STOP: cycle%u request FAIL rc=%d realised=%d wake_source=0x%x\n",
	       n,
	       (int)rc,
	       (int)wi.realised_mode,
	       (unsigned)wi.wake_source);
	g_fail++;
	cycle_set(n - 1u); /* the cycle did not run: do not count it */
	alp_power_close(p);
}

#ifndef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
/* Product build: is the post-wake clock tree the running one?  The UART's configured rate is
 * printed (the line is only readable on the host if the real rate matches), the CGU PLL
 * registers, and the kernel tick is measured against two RV-3028 seconds (a tick running on
 * the SE-left RC clocks reads about twice as long). */
static void check_clocks(unsigned cycle)
{
	const struct device *con = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	struct uart_config   cfg = { 0 };
	uint32_t             s0 = 0u, s = 0u;
	int64_t              t0, t1;
	bool                 ok = false;
	int                  ms = -1;

	(void)uart_config_get(con, &cfg);
	printk("POWER_STOP: clocks uart_baud=%u pll_lock=0x%08x pll_clk_sel=0x%08x\n",
	       (unsigned)cfg.baudrate,
	       (unsigned)sys_read32(0x1A602004u),
	       (unsigned)sys_read32(0x1A602008u));

	if (alp_som_power_rtc_seconds(&s0) == ALP_OK) {
		for (int i = 0; i < 300 && alp_som_power_rtc_seconds(&s) == ALP_OK && s == s0; ++i) {
			k_msleep(10);
		}
		s0 = s;
		t0 = k_uptime_get();
		for (int i = 0; i < 700 && alp_som_power_rtc_seconds(&s) == ALP_OK && s < s0 + 2u; ++i) {
			k_msleep(10);
		}
		t1 = k_uptime_get();
		if (s == s0 + 2u) {
			ms = (int)(t1 - t0);
			ok = ms >= 1800 && ms <= 2200;
		}
	}
	printk("POWER_STOP: clocks tick_ms_per_2_rtc_s=%d\n", ms);
	verdict(cycle, "tick_rate", ok);
}
#endif

int main(void)
{
#ifdef CONFIG_AEN_STOP_WAKE_TIMING
	timing_sample_main(); /* before anything that takes time: this is the "main()" instant */
#endif
	printk("\n=== aen-power-stop: Alif SE STOP backend bench (STOP bench-proven) ===\n");

	(void)alp_init();

	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = BRD_I2C_BUS, .bitrate_hz = 100000u });

	if (bus == NULL || rv3028c7_init(&g_rtc, bus) != ALP_OK ||
	    alp_som_power_bind_rv3028(&g_rtc) != ALP_OK) {
		printk("POWER_STOP: rtc_init FAIL err=%d\n", (int)alp_last_error());
		return 0;
	}

	/* What the SDK decoded before main() ran. */
	alp_power_boot_info_t bi = { 0 };

	(void)alp_power_boot_wake_info(&bi);
	unsigned done = cycle_get();

	printk("POWER_STOP: boot valid=%d counter=%u\n", (int)bi.valid, done);
	print_ses_version();
	print_regs();
	/* BKRAM after the boot-time clock restore: served from the RAM shadow (live=0) means the
	 * block failed its write/readback and a sleep will be refused (bkram_unusable). */
	printk("POWER_STOP: bkram live=%d selftest=%d\n",
	       (int)alp_som_pd_bkram_live(),
	       (int)alp_som_pd_bkram_selftest());
#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
	print_knobs();
#endif
	print_stop_mode(); /* baseline on the first boot, the wake witness after one */
#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
	print_diag("pre", ALP_SOM_PD_DIAG_PRE);
#ifdef CONFIG_AEN_STOP_WAKE_TIMING
	if (done != 0u && done <= N_CYCLES && bi.valid) {
		print_wake_timing(done, &bi); /* needs PRE: before it is invalidated below */
	}
#endif
	alp_som_pd_diag_invalidate(ALP_SOM_PD_DIAG_PRE); /* printed once; never read as stale later */
	print_diag("boot", ALP_SOM_PD_DIAG_BOOT);
	{
		/* BOOT word 16 is VBAT_STOP_MODE_REG as the SDK found it, BEFORE the wake path
		 * acknowledged STOP_MODE_STAT -- the raw witness; print_stop_mode() above reads it
		 * after that acknowledge. */
		alp_som_pd_diag_t b;

		if (alp_som_pd_diag_load(ALP_SOM_PD_DIAG_BOOT, &b)) {
			printk("POWER_STOP: BOOT w16 (raw pre-clear STOP_MODE)=0x%08x\n", (unsigned)b.w[16]);
		}
	}
#else
	check_clocks(done);
#endif
	print_se_profiles();

	if (done > N_CYCLES) {
		done = 0u; /* stale or corrupt: start over */
		cycle_set(0u);
	}
	if (done != 0u && !bi.valid) {
		/* A counter with no wake record behind it: a reset kept the SRAM, or the
		 * record did not survive.  Either way this is not a clean wake. */
		verdict(done, "record_valid", false);
		done = 0u;
		cycle_set(0u);
	} else if (done != 0u) {
		judge(done, &bi);
	}

	if (done < N_CYCLES) {
		start_cycle(done + 1u);
	}

	printk("POWER_STOP: SUMMARY cycles=%u pass=%u fail=%u\n", done, g_pass, g_fail);
	for (;;) {
		k_sleep(K_SECONDS(10));
		printk("POWER_STOP: awake (done)\n");
	}
	return 0;
}
