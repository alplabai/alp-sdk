/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-power-stop -- bench proof of the Alif SE STOP backend (#2784, unit U7) on
 * the E1M-AEN803 (Alif Ensemble E8, M55-HE), built as an MRAM image.
 *
 * UNTESTED ON SILICON.  Back up the MRAM image before flashing this (Flow D
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
 * A cold power cycle (not a reset) starts the sequence over: the counter lives in
 * SRAM that a reset keeps and a power cycle loses.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#include <alp/chips/rv3028c7.h>
#include <alp/peripheral.h>
#include <alp/power.h>

#include "som_power.h"       /* bench counter (SDK-internal) */
#include "som_power_chips.h" /* alp_som_power_bind_rv3028() (SDK-internal) */

#define N_CYCLES 3
#define AWAKE_MS 10000u

/* BRD_I2C, the on-module housekeeping bus the RV-3028 sits on (portable bus 2). */
#define BRD_I2C_BUS 2u

typedef struct {
	const char *what;          /* printed in the evidence line */
	uint32_t    wake_bitmap;   /* alp_power_configure_wake_source() */
	uint32_t    wake_after_ms; /* alp_power_request_sleep() */
	uint32_t    expect;        /* the wake_source bit that must come back */
	bool        arm_alarm;     /* arm the RV-3028 alarm for the next minute first */
} cycle_t;

static const cycle_t cycles[N_CYCLES] = {
	{ "LPTIMER 500 ms", ALP_POWER_WAKE_TIMER, 500u, ALP_POWER_WAKE_TIMER, false },
	{ "RV-3028 countdown 3 s", ALP_POWER_WAKE_RTC, 3000u, ALP_POWER_WAKE_RTC, false },
	{ "RV-3028 alarm", ALP_POWER_WAKE_RTC, 0u, ALP_POWER_WAKE_RTC, true },
};

/* Must outlive main(): the SDK keeps this pointer for the RTC power-domain hook and
 * the countdown. */
static rv3028c7_t g_rtc;

static unsigned g_pass, g_fail;

static void verdict(unsigned cycle, const char *check, bool pass)
{
	printk("POWER_STOP: cycle%u %s %s\n", cycle, check, pass ? "PASS" : "FAIL");
	if (pass) {
		g_pass++;
	} else {
		g_fail++;
	}
}

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
	verdict(done, "mode_stop", bi->valid && bi->realised_mode == ALP_POWER_MODE_STOP);
	verdict(done, "wake_source", bi->valid && bi->wake_source == c->expect);
	verdict(done,
	        "restored_all",
	        bi->valid && bi->quiesced_domains == bi->restored_domains &&
	            bi->restore_failed_domains == 0u);
	/* Reaching this line with the counter at @p done is the retention proof: it was
	 * written before the sleep and nothing else writes it. */
	verdict(done, "bkram_counter", alp_som_pd_bench_count() == done);
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

	/* No application RAM is kept: the SDK-reserved BKRAM is the floor. */
	alp_power_retain_t keep = { .level = ALP_POWER_RETAIN_NONE };
	alp_status_t       rc   = alp_power_configure_retention(p, &keep);

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
	alp_som_pd_bench_set(n);
	printk("POWER_STOP: cycle%u enter STOP (%s)\n", n, c->what);
	k_msleep(100); /* let the UART FIFO drain before the clocks go away */

	alp_power_wake_info_t wi = { 0 };

	rc = alp_power_request_sleep(p, ALP_POWER_MODE_STOP, c->wake_after_ms, &wi);

	/* Reaching here means the sleep did not happen (a refusal) or was cut short. */
	printk("POWER_STOP: cycle%u request FAIL rc=%d realised=%d wake_source=0x%x\n",
	       n,
	       (int)rc,
	       (int)wi.realised_mode,
	       (unsigned)wi.wake_source);
	g_fail++;
	alp_som_pd_bench_set(n - 1u); /* the cycle did not run: do not count it */
	alp_power_close(p);
}

int main(void)
{
	printk("\n=== aen-power-stop: Alif SE STOP backend bench (UNTESTED ON SILICON) ===\n");

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
	unsigned done = alp_som_pd_bench_count();

	printk("POWER_STOP: boot valid=%d counter=%u\n", (int)bi.valid, done);
	print_regs();
	print_stop_mode(); /* baseline on the first boot, the wake witness after one */

	if (done > N_CYCLES) {
		done = 0u; /* stale or corrupt: start over */
		alp_som_pd_bench_set(0u);
	}
	if (done != 0u && !bi.valid) {
		/* A counter with no wake record behind it: a reset kept the SRAM, or the
		 * record did not survive.  Either way this is not a clean wake. */
		verdict(done, "record_valid", false);
		done = 0u;
		alp_som_pd_bench_set(0u);
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
