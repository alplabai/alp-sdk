/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * Alif Secure Enclave STOP / STANDBY power backend for the Ensemble E8 M55-HE
 * (#2784, unit U7).  alp_power_request_sleep(STOP | STANDBY) on E1M-AEN801 /
 * E1M-AEN803.
 *
 * STATUS: UNTESTED ON SILICON.  CONFIG_ALP_SDK_POWER_ALIF_SE is off by default.
 * Every item marked [BENCH] below is a hypothesis the first bench run must settle.
 *
 * What a STOP is on this part
 * ---------------------------
 * The M55-HE subsystem is powered OFF.  The Secure Enclave (SE) owns the power
 * tree: before the core sleeps, an OFF profile (off_profile_t) is handed to the SE
 * over its mailbox, then the core enters the EWIC subsystem-off sleep.  When a
 * wake event fires the SE powers the subsystem back up and boots it through the
 * normal SES -> ATOC path -- a COLD BOOT, so alp_power_request_sleep() does not
 * return.  The 4 KB Utility SRAM (BKRAM) is retained and carries the SDK's wake
 * record (som_power_record.c); the wake decode below turns that record plus the
 * wake hardware's latched status into alp_power_boot_wake_info().
 *
 * The OFF profile is built COMPLETELY and EXPLICITLY
 * --------------------------------------------------
 * se_service_set_run_cfg() / set_off_cfg() are not side-effect-free (bench,
 * E1M-AEN803, 2026-10-08): a read-modify-write of one field dropped memory_blocks
 * bit 20, cleared the VBAT retention-LDO enables in VBAT_ANA_REG1 and the CVM
 * retention masks in RET_CTRL, and those persist across a SYSRESETREQ.  So this
 * backend never read-modify-writes a profile.  build_off_profile() assigns each of
 * the 14 off_profile_t members by name (a test pre-fills the struct with a poison
 * pattern and proves none survives).  Only the three members the SE itself owns
 * are taken from the live profile: dcdc_voltage (range-checked), vtor_address and
 * vtor_address_ns (preserved so the wake still goes through the SES -> ATOC path).
 * After the SE has written the profile the backend reads back ALL 14 members,
 * re-asserts the retention LDO / RET_CTRL bits it needs and refuses to sleep if any
 * of it did not stick.  Before that call it snapshots RET_CTRL and VBAT_ANA_REG1 and
 * keeps the live profile: every later exit (a failure, or a sleep that did not power
 * down) writes the live profile and the snapshots back and verifies them, so a failed
 * request leaves the SE and the retention registers as it found them.
 *
 * Wake sources (advertised only when real)
 * ----------------------------------------
 *   ALP_POWER_WAKE_RTC    the on-module RV-3028 on /INT -> P15_0 -> LPGPIO0
 *                         (WE_LPGPIO0).  Primary RTC: ER001 / ER002 and the
 *                         LFRC-only boot make the internal LPRTC unfit for timed
 *                         wake.  A countdown is started through the chip driver
 *                         (rv3028c7_timer_start); with wake_after_ms == 0 the
 *                         caller's own alarm / countdown must already be armed.
 *   ALP_POWER_WAKE_TIMER  the LPTIMER (the `alp,power-wake-timer` chosen node,
 *                         WE_LPTIMER0) for wake_after_ms < 1000.  Its 32 kHz source
 *                         is the AON low-frequency clock: LFRC runs ~4.5 % fast
 *                         (34251.7 Hz measured vs the 32768 Hz the devicetree
 *                         states).  wake_after_ms is a MINIMUM (the sleep is never
 *                         shorter), so the tick count is rounded UP against the
 *                         fastest the clock can run; a short LFRC wake can be late.
 *                         wake_after_ms >= 1000 uses the RV-3028 countdown instead
 *                         (whole seconds, rounded up; > 4095 s in whole minutes).
 *   Nothing else is advertised.  GPIO, UART RX, comparator, brown-out and USB wake
 *   are real hardware features of the part but are not wired by this backend.
 *
 * Refusals (steps 1-3 below change nothing)
 * ----------------------------------------
 *   ALP_ERR_BUSY       a debugger is attached (DHCSR.C_DEBUGEN; bench override
 *                      CONFIG_ALP_SDK_POWER_ALIF_SE_ALLOW_DEBUGGER), or an armed
 *                      wake source is already pending (the sleep would end at once).
 *   ALP_ERR_NOSUPPORT  the D-cache is enabled, or a wake bit / duration this
 *                      backend cannot arm.
 *   ALP_ERR_NOT_READY  the core's low-power-state requests are not all OFF.
 *   ALP_ERR_INVAL      bad mode, bad retention, a timed wake out of range, a
 *                      policy that conflicts with an armed source.
 *   ALP_ERR_IO         the SE refused or altered the profile, the retention bits
 *                      did not stick, or the subsystem stayed up with no wake
 *                      source to explain it.
 *
 * Sequence of a STOP / STANDBY request
 * ------------------------------------
 *   1 validate wake bits, retention, duration       (no side effect)
 *   2 refuse: debugger, D-cache, LPSTATE, pending   (no side effect)
 *   3 read the live OFF profile, build the new one  (no side effect)
 *   4 quiesce the SoM power domains, write the BKRAM record
 *   5 arm the wake sources (RV countdown / LPTIMER)
 *   6 snapshot RET_CTRL / VBAT_ANA_REG1, se_service_set_off_cfg, read back all 14
 *     members, re-assert RET_CTRL / ANA_REG1, verify
 *   7 EWIC entry (interrupts off, INT pad armed inside); does not return on success
 * A failure after 3 disarms the sources, restores the domains in reverse, and, once
 * step 6 has started, writes the live profile and the register snapshots back and
 * verifies them (undo_se).  A failure of that undo itself is reported as ALP_ERR_IO.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <alp/backend.h>
#include <alp/chips/rv3028c7.h>
#include <alp/peripheral.h>
#include <alp/power.h>

#if defined(CONFIG_ALP_SDK_POWER_ALIF_SE)

/* hal_alif SE service client (Apache-2.0): off_profile_t / run_profile_t and the
 * aiPM enums.  The memory-block and wake-event masks are NOT taken from its
 * headers (gen1 layout, see alif_aipm_gen2.h); only power-domain masks, clock
 * enums and the profile structs are. */
#include <se_service.h>

#include "alif_aipm_gen2.h"
#include "alif_se_power_hw.h"
#include "power_ops.h"
#include "som_power.h"

/* ---- Register facts (E8 SVD) ----------------------------------------------- */

/* ANA.VBAT_ANA_REG1 fields (SVD VBAT_ANA_REG1 @ ANA + 0x38).  Checked against the
 * SVD by tests/scripts/test_alif_se_power_svd.py. */
#define ALP_ALIF_SE_SVD_ANA_REG1_RET_LDO_VBAT_EN_BIT    8u  /* LDO-0: Utility SRAM retention */
#define ALP_ALIF_SE_SVD_ANA_REG1_RET_LDO_VDDMAIN_EN_BIT 10u /* LDO-2: SRAM0/1, SE SRAM, HE TCM */
#define ALP_ALIF_SE_SVD_ANA_REG1_XTAL32K_EN_BIT         12u
#define ALP_ALIF_SE_SVD_ANA_REG1_XTAL32K_CAP_CONT_LSB   19u
#define ALP_ALIF_SE_SVD_ANA_REG1_XTAL32K_CAP_CONT_WIDTH 6u
/* ANA.MISC_CTRL.SEL_32K: 0 = LFRC, 1 = LFXO. */
#define ALP_ALIF_SE_SVD_ANA_MISC_SEL_32K_BIT 0u

#define ANA_REG1_RET_LDO_VBAT_EN    (UINT32_C(1) << ALP_ALIF_SE_SVD_ANA_REG1_RET_LDO_VBAT_EN_BIT)
#define ANA_REG1_RET_LDO_VDDMAIN_EN (UINT32_C(1) << ALP_ALIF_SE_SVD_ANA_REG1_RET_LDO_VDDMAIN_EN_BIT)
#define ANA_REG1_XTAL32K_EN         (UINT32_C(1) << ALP_ALIF_SE_SVD_ANA_REG1_XTAL32K_EN_BIT)
#define ANA_REG1_CAP_CONT_MASK \
	(((UINT32_C(1) << ALP_ALIF_SE_SVD_ANA_REG1_XTAL32K_CAP_CONT_WIDTH) - 1u) \
	 << ALP_ALIF_SE_SVD_ANA_REG1_XTAL32K_CAP_CONT_LSB)
#define ANA_MISC_SEL_32K (UINT32_C(1) << ALP_ALIF_SE_SVD_ANA_MISC_SEL_32K_BIT)

/* Y1 (ECS-.327-12.5, CL 12.5 pF) has no external load caps, so the SoC trim is its
 * only load: code 63 (maximum) measured +61 ppm against the RV-3028, the SE's
 * default code 8 measured +451 ppm (bench, E1M-AEN803, 2026-10-08). */
#define XTAL32K_CAP_CONT_MAX 63u

/* RET_CTRL masks this backend needs (SVD VBAT.RET_CTRL, via alif_aipm_gen2.h). */
#define RET_CTRL_BKRAM  (UINT32_C(1) << ALP_AIPM_SVD_RET_CTRL_BKRAM_RET_MASK_BIT)
#define RET_CTRL_HETCM1 (UINT32_C(1) << ALP_AIPM_SVD_RET_CTRL_HETCM_RET1_MASK_BIT)
#define RET_CTRL_HETCM2 (UINT32_C(1) << ALP_AIPM_SVD_RET_CTRL_HETCM_RET2_MASK_BIT)

/* ---- Constants -------------------------------------------------------------- */

/* wake_after_ms is a MINIMUM: the sleep is never shorter.  The LPTIMER therefore
 * counts against the FASTEST its clock can run, so it can only be late:
 *   LFRC  36045 Hz = 32768 +10 %, the top of the OSC_RC_32K_FREQ_CONT trim range
 *         (SVD VBAT_ANA_REG1: -5 % .. +10 %); the SES leaves it at 34251.7 Hz
 *         (+4.53 %, bench, E1M-AEN803, 2026-10-08), inside that bound.
 *   LFXO  32775 Hz = 32768 + 200 ppm; Y1 measured +61 ppm (RV-3028 reference) /
 *         +141 ppm (HE reference) at XTAL32K_CAP_CONT = 63.
 * Cost: counting at 36045 Hz, a clock at the BOTTOM of the trim range (-5 %, about
 * 31130 Hz) sleeps up to ~16 % longer than asked; the 34251.7 Hz this module
 * measured gives ~5 %.  That is the price of never being early on a clock this
 * poor; use the RV-3028 for an accurate wake. */
#define ALIF_SE_LFRC_HZ_MAX 36045u
#define ALIF_SE_LFXO_HZ_MAX 32775u

/* The LPTIMER carries wake_after_ms below this; at or above it the RV-3028 does. */
#ifdef CONFIG_ALP_SDK_POWER_ALIF_SE_BENCH_LPTIMER_MAX_MS
#define ALIF_SE_LPTIMER_MAX_MS ((uint32_t)CONFIG_ALP_SDK_POWER_ALIF_SE_BENCH_LPTIMER_MAX_MS)
#else
#define ALIF_SE_LPTIMER_MAX_MS 1000u
#endif

/* DC-DC window the SE accepts (alif_se_profile.c, docs/aen-se-services.md). */
#define ALIF_SE_DCDC_MV_MIN 750u
#define ALIF_SE_DCDC_MV_MAX 850u

/* M55-HE TCM: ITCM 256 KiB + DTCM 256 KiB, two retention banks each.  [BENCH] The
 * 128 KiB bank size and the SRAM4_x = ITCM / SRAM5_x = DTCM split are the
 * unverified part of the gen2 header (alif_aipm_gen2.h); DTCM banks are claimed
 * first because data is what an application keeps. */
#define ALIF_SE_TCM_BANK_KB  128u
#define ALIF_SE_TCM_BANKS    4u
#define ALIF_SE_TCM_TOTAL_KB (ALIF_SE_TCM_BANK_KB * ALIF_SE_TCM_BANKS)

static const uint32_t _tcm_bank_mask[ALIF_SE_TCM_BANKS] = {
	ALP_AIPM_GEN2_SRAM5_1_MASK,
	ALP_AIPM_GEN2_SRAM5_2_MASK,
	ALP_AIPM_GEN2_SRAM4_1_MASK,
	ALP_AIPM_GEN2_SRAM4_2_MASK,
};

/* ---- Small helpers ---------------------------------------------------------- */

static alp_status_t se_rc_to_alp(int rc)
{
	switch (rc) {
	case 0:
		return ALP_OK;
	case -EINVAL:
		return ALP_ERR_INVAL;
	case -EAGAIN:
	case -EBUSY:
		return ALP_ERR_NOT_READY;
	default:
		return ALP_ERR_IO;
	}
}

/* Every refusal says which check, at which step, with what raw value: a STOP that
 * does not start must never be silent (bench U8: ALP_ERR_NOT_READY with no clue). */
static alp_status_t refuse(int step, const char *reason, int raw, alp_status_t rc)
{
	printk(
	    "alif_se_power: refuse step=%d reason=%s rc=%d (status %d)\n", step, reason, raw, (int)rc);
	return rc;
}

static bool rtc_int_usable(void)
{
	return alif_se_hw_rtc_int_present();
}

/* The wake bits STOP / STANDBY can arm right now. */
static uint32_t stop_wake_caps(void)
{
	uint32_t caps = 0u;

	if (rtc_int_usable()) {
		caps |= ALP_POWER_WAKE_RTC;
	}
	if (alif_se_hw_wake_timer_present() ||
	    (rtc_int_usable() && alp_som_power_rtc_countdown_ready())) {
		caps |= ALP_POWER_WAKE_TIMER;
	}
	return caps;
}

static bool is_deep_mode(alp_power_mode_t mode)
{
	return mode == ALP_POWER_MODE_STOP || mode == ALP_POWER_MODE_STANDBY;
}

/* ---- Retention -------------------------------------------------------------- */

/* memory_blocks for @p r: the Utility SRAM (BKRAM, always, gen2 bit 21) plus the
 * TCM banks the request needs, rounded UP to whole banks. */
static uint32_t retained_blocks(const alp_power_retain_t *r)
{
	uint32_t blocks = ALP_AIPM_GEN2_BACKUP4K_MASK;
	uint32_t banks  = 0u;

	if (r->level == ALP_POWER_RETAIN_FULL) {
		banks = ALIF_SE_TCM_BANKS;
	} else if (r->level == ALP_POWER_RETAIN_TCM) {
		banks = (r->retain_kb + ALIF_SE_TCM_BANK_KB - 1u) / ALIF_SE_TCM_BANK_KB;
	}
	for (uint32_t i = 0; i < banks && i < ALIF_SE_TCM_BANKS; ++i) {
		blocks |= _tcm_bank_mask[i];
	}
	return blocks;
}

static bool retention_valid(const alp_power_retain_t *r, alp_status_t *why)
{
	switch (r->level) {
	case ALP_POWER_RETAIN_NONE:
	case ALP_POWER_RETAIN_UTILITY: /* equivalent to NONE: BKRAM is always kept */
	case ALP_POWER_RETAIN_FULL:
		return true;
	case ALP_POWER_RETAIN_TCM:
		if (r->retain_kb == 0u) {
			*why = ALP_ERR_INVAL;
			return false;
		}
		if (r->retain_kb > ALIF_SE_TCM_TOTAL_KB) {
			*why = ALP_ERR_NOSUPPORT;
			return false;
		}
		return true;
	default:
		*why = ALP_ERR_INVAL;
		return false;
	}
}

static bool tcm_requested(uint32_t memory_blocks)
{
	for (uint32_t i = 0; i < ALIF_SE_TCM_BANKS; ++i) {
		if ((memory_blocks & _tcm_bank_mask[i]) != 0u) {
			return true;
		}
	}
	return false;
}

/* ---- The sleep plan ---------------------------------------------------------- */

typedef struct {
	alp_power_mode_t mode;
	uint32_t         wake;          /* ALP_POWER_WAKE_* the cycle arms */
	uint32_t         hw;            /* ALP_SOM_ARM_* wake paths */
	uint32_t         lptimer_ticks; /* LPTIMER_ARM: down-count */
	uint32_t         rtc_seconds;   /* RTC_TIMER: requested countdown */
	uint32_t         armed_ms;      /* nominal timed-wake length */
	uint32_t         timed_bit;     /* ALP_POWER_WAKE_* a timed wake reports */
	bool             lfxo;          /* AON clock source for the OFF profile */
	uint32_t         memory_blocks;
} sleep_plan_t;

/* Decide what arms the wake, with no side effect.  Errors are final. */
static alp_status_t
plan_wake(const alp_power_backend_state_t *state, uint32_t wake_after_ms, sleep_plan_t *plan)
{
	const uint32_t bitmap = state->wake_bitmap;

	if ((bitmap & ~stop_wake_caps()) != 0u) {
		return ALP_ERR_NOSUPPORT;
	}
	if (bitmap == 0u && wake_after_ms == 0u) {
		return ALP_ERR_INVAL; /* nothing would ever wake the SoC */
	}
	if ((bitmap & ALP_POWER_WAKE_TIMER) != 0u && wake_after_ms == 0u) {
		return ALP_ERR_INVAL; /* a timer wake with no length */
	}

	if (wake_after_ms != 0u) {
		if (wake_after_ms < ALIF_SE_LPTIMER_MAX_MS) {
			uint32_t hz = alif_se_hw_wake_timer_hz();

			if (!alif_se_hw_wake_timer_present() || hz == 0u) {
				return ALP_ERR_NOSUPPORT;
			}
			/* Round UP against the fastest clock: never early (see the constants). */
			uint64_t rate  = plan->lfxo ? ALIF_SE_LFXO_HZ_MAX : ALIF_SE_LFRC_HZ_MAX;
			uint64_t ticks = ((uint64_t)wake_after_ms * rate + 999u) / 1000u;

			plan->lptimer_ticks = (uint32_t)ticks;
			plan->hw |= ALP_SOM_ARM_LPTIMER;
			plan->armed_ms = wake_after_ms;
		} else {
			if (!rtc_int_usable() || !alp_som_power_rtc_countdown_ready()) {
				return ALP_ERR_NOSUPPORT;
			}
			uint32_t seconds = (wake_after_ms + 999u) / 1000u;

			if (seconds > RV3028C7_TIMER_MAX_SECONDS) {
				return ALP_ERR_INVAL;
			}
			plan->rtc_seconds = seconds;
			plan->hw |= ALP_SOM_ARM_RTC_TIMER;
			plan->armed_ms = seconds * 1000u;
		}
	}

	/* A timed wake reports the source the caller asked for, whichever hardware serves
	 * it: TIMER (also when nothing was asked), or RTC when only RTC was configured. */
	if (wake_after_ms != 0u) {
		plan->timed_bit =
		    ((bitmap & ALP_POWER_WAKE_RTC) != 0u && (bitmap & ALP_POWER_WAKE_TIMER) == 0u)
		        ? ALP_POWER_WAKE_RTC
		        : ALP_POWER_WAKE_TIMER;
		plan->wake |= plan->timed_bit;
	}

	if ((bitmap & ALP_POWER_WAKE_RTC) != 0u) {
		if ((plan->hw & ALP_SOM_ARM_RTC_TIMER) == 0u) {
			/* The caller's own RV-3028 alarm / countdown: it must really be armed, or
			 * the SoC sleeps with nothing to wake it. */
			bool armed = false;

			if (!rtc_int_usable() || alp_som_power_rtc_int_armed(&armed) != ALP_OK || !armed) {
				return ALP_ERR_INVAL;
			}
			plan->hw |= ALP_SOM_ARM_RTC_INT;
		}
		plan->wake |= ALP_POWER_WAKE_RTC;
	}

	/* A policy that takes the RTC away conflicts with an RTC wake. */
	if ((plan->hw & (ALP_SOM_ARM_RTC_TIMER | ALP_SOM_ARM_RTC_INT)) != 0u &&
	    alp_som_power_policy(ALP_POWER_DOMAIN_RTC) == ALP_POWER_DOMAIN_POLICY_RAIL_OFF) {
		return ALP_ERR_INVAL;
	}
	return ALP_OK;
}

/* Every armed path must be idle before arming: a latched status would end the sleep
 * at once, and clearing it here would swallow an event the application wants. */
static alp_status_t refuse_if_pending(const sleep_plan_t *plan)
{
	if ((plan->hw & ALP_SOM_ARM_LPTIMER) != 0u && alif_se_hw_wake_timer_pending()) {
		return refuse(2, "lptimer_pending", 1, ALP_ERR_BUSY);
	}
	if ((plan->hw & (ALP_SOM_ARM_RTC_TIMER | ALP_SOM_ARM_RTC_INT)) != 0u &&
	    alif_se_hw_rtc_int_asserted() > 0) {
		return refuse(2, "rtc_int_asserted", 1, ALP_ERR_BUSY);
	}
	return ALP_OK;
}

static alp_status_t refuse_if_unfit(void)
{
#ifndef CONFIG_ALP_SDK_POWER_ALIF_SE_ALLOW_DEBUGGER
	if (alif_se_hw_debugger_attached()) {
		return refuse(2, "debugger_attached", 1, ALP_ERR_BUSY);
	}
#endif
	if (alif_se_hw_dcache_active()) {
		return refuse(2, "dcache_on", 1, ALP_ERR_NOSUPPORT);
	}
	if (!alif_se_hw_lpstate_off()) {
		/* a core power-state request keeps the subsystem up; raw = PWRMODCTL.CPDLPSTATE */
		return refuse(2, "lpstate_not_off", (int)alif_se_hw_lpstate_read(), ALP_ERR_NOT_READY);
	}
	return ALP_OK;
}

/* ---- The OFF profile ---------------------------------------------------------- */

/* off_profile_t must end at vtor_address_ns, or a member added by a hal_alif bump
 * would go unassigned below. */
_Static_assert(offsetof(off_profile_t, vtor_address_ns) + sizeof(uint32_t) == sizeof(off_profile_t),
               "off_profile_t changed: assign the new member in build_off_profile()");

/* LFXO is selected only when the live clock tree already runs on it (SEL_32K and
 * XTAL32K_EN), i.e. when it is confirmed to oscillate; the SES boot leaves LFRC. */
static bool lfxo_confirmed(void)
{
	return (alif_se_hw_reg_read(ALIF_SE_REG_ANA_MISC) & ANA_MISC_SEL_32K) != 0u &&
	       (alif_se_hw_reg_read(ALIF_SE_REG_ANA_REG1) & ANA_REG1_XTAL32K_EN) != 0u;
}

static uint32_t wake_events_for(const sleep_plan_t *plan)
{
	uint32_t we = 0u;

	if ((plan->hw & ALP_SOM_ARM_LPTIMER) != 0u) {
		we |= ALP_AIPM_GEN2_WE_LPTIMER0; /* lptimer0 is the `alp,power-wake-timer` */
	}
	if ((plan->hw & (ALP_SOM_ARM_RTC_TIMER | ALP_SOM_ARM_RTC_INT)) != 0u) {
		we |= ALP_AIPM_GEN2_WE_LPGPIO0; /* RV-3028 /INT = P15_0 = LPGPIO bit 0 */
	}
	return we;
}

/* EWIC lines are enabled as whole groups (EWIC_VBAT_TIMER = bits 10:7, EWIC_VBAT_GPIO
 * = bits 18:11), the way the vendor does (sdk-alif power_mgr.c enables EWIC_VBAT_GPIO
 * with the single WE_LPGPIO1).  Narrowing it to LPGPIO line 0 (bit 11) needs the
 * line-to-bit order, which no Alif document in the tree states, so it is not
 * guessed: the other lines stay disarmed anyway, because only the single
 * WE_LPGPIO0 wake event is requested and no other LPGPIO / LPTIMER interrupt is
 * enabled in the NVIC.  [BENCH] */
static uint32_t ewic_for(const sleep_plan_t *plan)
{
	uint32_t ewic = 0u;

	if ((plan->hw & ALP_SOM_ARM_LPTIMER) != 0u) {
		ewic |= ALP_AIPM_GEN2_EWIC_VBAT_TIMER;
	}
	if ((plan->hw & (ALP_SOM_ARM_RTC_TIMER | ALP_SOM_ARM_RTC_INT)) != 0u) {
		ewic |= ALP_AIPM_GEN2_EWIC_VBAT_GPIO;
	}
	return ewic;
}

/**
 * Build the complete OFF profile for @p plan.  Every member of @p out is assigned
 * here by name; @p out is not read first.  @p live is the SE's current OFF profile,
 * consulted for exactly three things: dcdc_voltage (range-checked, not trusted),
 * vtor_address and vtor_address_ns (preserved, so the wake still goes through the
 * SES -> ATOC path).
 */
static alp_status_t
build_off_profile(off_profile_t *out, const off_profile_t *live, const sleep_plan_t *plan)
{
	if (live->dcdc_voltage < ALIF_SE_DCDC_MV_MIN || live->dcdc_voltage > ALIF_SE_DCDC_MV_MAX) {
		return ALP_ERR_IO; /* a garbled read, not a profile to carry over */
	}

	const bool stop = (plan->mode == ALP_POWER_MODE_STOP);

	/* STOP keeps only the VBAT always-on domain (PD0) powered.  STANDBY adds the SE's
	 * own always-on domain (PD2).  PD0 stays in both: it holds the BKRAM, the LPTIMER
	 * and the LPGPIO this backend arms and the wake record depends on.  The vendor
	 * OFF_STATE_STANDBY profile (sdk-alif subsys/bluetooth/common/power_mgr.c) writes
	 * PD_SSE700_AON_MASK alone, but that file shows no PD0 exclusion being required
	 * for STANDBY, only that PD2 is added, so the superset is used.  [BENCH] */
	out->power_domains = stop ? PD_VBAT_AON_MASK : (PD_VBAT_AON_MASK | PD_SSE700_AON_MASK);
	out->dcdc_voltage  = live->dcdc_voltage;
	out->dcdc_mode     = DCDC_MODE_OFF;
	out->aon_clk_src   = plan->lfxo ? CLK_SRC_LFXO : CLK_SRC_LFRC;
	out->stby_clk_src  = CLK_SRC_HFRC;
	out->stby_clk_freq = stop ? SCALED_FREQ_RC_STDBY_0_075_MHZ : SCALED_FREQ_RC_STDBY_76_8_MHZ;
	out->memory_blocks = plan->memory_blocks;
	if (IS_ENABLED(CONFIG_ALP_SDK_POWER_ALIF_SE_BENCH_MRAM_SERAM)) {
		/* Bench variant (vi): the vendor sample's MRAM-boot profile (sdk-alif
		 * samples/drivers/pm/system_off) keeps MRAM and the SE RAM powered. */
		out->memory_blocks |= ALP_AIPM_GEN2_MRAM_MASK | ALP_AIPM_GEN2_SERAM_MASK;
	}
	/* The cold-boot RUN profile carries no IP clock gating and no PHY power gating
	 * (E1M-AEN803, 2026-10-08); nothing here changes that. */
	out->ip_clock_gating = 0u;
	out->phy_pwr_gating  = 0u;
	out->vdd_ioflex_3V3  = IOFLEX_LEVEL_1V8; /* this SoM's I/O flex rail is 1.8 V */
	out->wakeup_events   = wake_events_for(plan);
	out->ewic_cfg        = ewic_for(plan);
	out->vtor_address    = live->vtor_address;
	out->vtor_address_ns = live->vtor_address_ns;
	if (IS_ENABLED(CONFIG_ALP_SDK_POWER_ALIF_SE_BENCH_VTOR_SELF)) {
		/* Bench variant (v): resume at this image's own vector table, as the vendor
		 * sample does (offp.vtor_address = SCB->VTOR), instead of through SES -> ATOC. */
		out->vtor_address    = alif_se_hw_vtor_read();
		out->vtor_address_ns = out->vtor_address;
	}
	return ALP_OK;
}

/* Every member must read back as written.  The SE can drop or rewrite bits silently
 * (it refused to restore memory_blocks bit 20 on the bench), and a wrong dcdc_voltage
 * or vdd_ioflex_3V3 is a hardware hazard, not a power-saving miss: any difference
 * abandons the sleep. */
/* Probe value for the OFF profile's dcdc_mode (see off_profile_matches()). */
#define OFF_DCDC_MODE_SENTINEL ((dcdc_mode_t)0xA5A5A5A5u)

/* Name every member that differs, expected vs got, so the first bench run can tell an
 * SE that rewrites only what it owns (stby_clk_src / stby_clk_freq are "selected
 * automatically") from one that drops a retention or wake bit.  Failure path only. */
#define OFF_DIFF(field) \
	do { \
		if ((uint32_t)want->field != (uint32_t)got->field) { \
			printk("alif_se_power: OFF profile %s: wrote 0x%08x, SE reports 0x%08x\n", \
			       #field, \
			       (unsigned)want->field, \
			       (unsigned)got->field); \
		} \
	} while (0)

static void off_profile_log_diff(const off_profile_t *want, const off_profile_t *got)
{
	OFF_DIFF(power_domains);
	OFF_DIFF(dcdc_voltage);
	if (got->dcdc_mode != OFF_DCDC_MODE_SENTINEL) {
		OFF_DIFF(dcdc_mode);
	}
	OFF_DIFF(aon_clk_src);
	OFF_DIFF(stby_clk_src);
	OFF_DIFF(stby_clk_freq);
	OFF_DIFF(memory_blocks);
	OFF_DIFF(ip_clock_gating);
	OFF_DIFF(phy_pwr_gating);
	OFF_DIFF(vdd_ioflex_3V3);
	OFF_DIFF(wakeup_events);
	OFF_DIFF(ewic_cfg);
	OFF_DIFF(vtor_address);
	OFF_DIFF(vtor_address_ns);
}

/* hal_alif's OFF-profile client does not carry dcdc_mode (v2.3.0 se_service.c:
 * se_service_set_off_cfg() zeroes the packet, so the SE receives 0 = DCDC_MODE_OFF;
 * se_service_get_off_cfg() never fills it, unlike the RUN pair), so a read-back
 * leaves the member untouched.  It is probed with a sentinel: still the sentinel
 * after the call means "not transported", and the member is skipped.  A value that
 * changed (a future hal_alif) is compared strictly.  Nothing special-cases a value. */

/* Read the SE's OFF profile into @p out, which is fully initialised first: the
 * members the client does not fill must not be stack garbage. */
static int off_cfg_read(off_profile_t *out)
{
	memset(out, 0, sizeof(*out));
	out->dcdc_mode = OFF_DCDC_MODE_SENTINEL;
	return se_service_get_off_cfg(out);
}

static bool dcdc_mode_transported(const off_profile_t *got)
{
	static bool logged;

	if (got->dcdc_mode == OFF_DCDC_MODE_SENTINEL) {
		if (!logged) {
			logged = true;
			printk("alif_se_power: OFF dcdc_mode not transported by the hal_alif client; the "
			       "SE receives 0 (DCDC_MODE_OFF)\n");
		}
		return false;
	}
	return true;
}

static bool off_profile_matches(const off_profile_t *want, const off_profile_t *got)
{
	if (dcdc_mode_transported(got) && got->dcdc_mode != want->dcdc_mode) {
		return false;
	}
	return got->power_domains == want->power_domains && got->dcdc_voltage == want->dcdc_voltage &&
	       got->aon_clk_src == want->aon_clk_src && got->stby_clk_src == want->stby_clk_src &&
	       got->stby_clk_freq == want->stby_clk_freq && got->memory_blocks == want->memory_blocks &&
	       got->ip_clock_gating == want->ip_clock_gating &&
	       got->phy_pwr_gating == want->phy_pwr_gating &&
	       got->vdd_ioflex_3V3 == want->vdd_ioflex_3V3 &&
	       got->wakeup_events == want->wakeup_events && got->ewic_cfg == want->ewic_cfg &&
	       got->vtor_address == want->vtor_address && got->vtor_address_ns == want->vtor_address_ns;
}

/* After the SE call: re-assert what its write cleared and check it took.  Only bits
 * this cycle needs are OR-ed in; nothing is cleared. */
static alp_status_t reassert_retention(const sleep_plan_t *plan)
{
	uint32_t ret_need = RET_CTRL_BKRAM;
	uint32_t ana_need = ANA_REG1_RET_LDO_VBAT_EN;

	if (tcm_requested(plan->memory_blocks)) {
		ret_need |= RET_CTRL_HETCM1 | RET_CTRL_HETCM2;
		ana_need |= ANA_REG1_RET_LDO_VDDMAIN_EN;
	}
	if (plan->lfxo) {
		ana_need |= ANA_REG1_XTAL32K_EN;
	}

	uint32_t ret = alif_se_hw_reg_read(ALIF_SE_REG_RET_CTRL);

	if ((ret & ret_need) != ret_need) {
		alif_se_hw_reg_write(ALIF_SE_REG_RET_CTRL, ret | ret_need);
	}

	uint32_t ana  = alif_se_hw_reg_read(ALIF_SE_REG_ANA_REG1);
	uint32_t want = ana | ana_need;

	if (plan->lfxo) {
		want = (want & ~ANA_REG1_CAP_CONT_MASK) |
		       (XTAL32K_CAP_CONT_MAX << ALP_ALIF_SE_SVD_ANA_REG1_XTAL32K_CAP_CONT_LSB);
	}
	if (want != ana) {
		alif_se_hw_reg_write(ALIF_SE_REG_ANA_REG1, want);
	}

	if ((alif_se_hw_reg_read(ALIF_SE_REG_RET_CTRL) & ret_need) != ret_need ||
	    (alif_se_hw_reg_read(ALIF_SE_REG_ANA_REG1) & want) != want ||
	    (plan->lfxo &&
	     (alif_se_hw_reg_read(ALIF_SE_REG_ANA_REG1) & ANA_REG1_CAP_CONT_MASK) !=
	         (XTAL32K_CAP_CONT_MAX << ALP_ALIF_SE_SVD_ANA_REG1_XTAL32K_CAP_CONT_LSB))) {
		return ALP_ERR_IO; /* retention not guaranteed: do not sleep */
	}
	return ALP_OK;
}

/* ---- Arming ------------------------------------------------------------------- */

typedef struct {
	bool lptimer;
	bool rtc_timer;
	bool int_pad;
} armed_t;

static void disarm(const armed_t *a)
{
	if (a->int_pad) {
		alif_se_hw_rtc_int_disarm();
	}
	if (a->rtc_timer) {
		(void)alp_som_power_rtc_countdown_cancel();
	}
	if (a->lptimer) {
		alif_se_hw_wake_timer_disarm();
	}
}

static alp_status_t arm(const sleep_plan_t *plan, armed_t *a)
{
	alp_status_t s;

	*a = (armed_t){ 0 };
	if ((plan->hw & ALP_SOM_ARM_RTC_TIMER) != 0u) {
		uint32_t actual = 0u;

		s = alp_som_power_rtc_countdown_start(plan->rtc_seconds, &actual);
		if (s != ALP_OK) {
			return s;
		}
		a->rtc_timer = true;
	}
	if ((plan->hw & ALP_SOM_ARM_LPTIMER) != 0u) {
		/* NOT armed here: the LPTIMER is armed last, inside the interrupt-off entry,
		 * so no SE call, readback or console output can outlast the interval (bench
		 * U8c).  Recorded now so every unwind cancels it. */
		a->lptimer = true;
	}
	if ((plan->hw & (ALP_SOM_ARM_RTC_TIMER | ALP_SOM_ARM_RTC_INT)) != 0u) {
		s = alif_se_hw_rtc_int_arm();
		if (s != ALP_OK) {
			return s; /* the caller unwinds what is recorded in @p a */
		}
		a->int_pad = true;
	}
	return ALP_OK;
}

/* ---- The cycle record --------------------------------------------------------- */

/* Quiesce writes the record only when it quiesced something; the wake decode needs
 * one every cycle, so complete (or create) it here. */
static void save_cycle_record(const sleep_plan_t *plan)
{
	alp_som_pd_record_t rec = { 0 };
	uint32_t            now = 0u;

	(void)alp_som_pd_store_load(&rec);
	rec.mode        = (uint32_t)plan->mode;
	rec.wake_source = 0u;
	rec.slept_ms    = 0u;
	rec.armed       = plan->wake;
	rec.armed_hw    = plan->hw;
	/* Probed NOW, before the sleep: only if the NSRST syndrome bit does clear can a set bit
	 * at the next boot be read as a pin reset (see ALP_SOM_REC_NSRST_TRUSTED). */
	if (alp_som_power_reset_syndrome_trusted()) {
		rec.armed_hw |= ALP_SOM_REC_NSRST_TRUSTED;
	}
	rec.timed_bit   = plan->timed_bit;
	rec.armed_ms    = plan->armed_ms;
	rec.entry_rtc_s = (alp_som_power_rtc_seconds(&now) == ALP_OK) ? now : 0u;
	alp_som_pd_store_save(&rec);
}

/* ---- request_sleep ------------------------------------------------------------- */

static void
fill_info(alp_power_wake_info_t *info, alp_power_mode_t mode, uint32_t wake, uint32_t ms)
{
	if (info != NULL) {
		info->realised_mode = mode;
		info->wake_source   = wake;
		info->slept_ms      = ms;
	}
}

/* Which armed source ended an aborted sleep (the sleep did not power the core down). */
static uint32_t fired_sources(const sleep_plan_t *plan)
{
	uint32_t fired = 0u;
	uint8_t  flags = 0u;

	if ((plan->hw & ALP_SOM_ARM_LPTIMER) != 0u && alif_se_hw_wake_timer_pending()) {
		fired |= plan->timed_bit;
	}
	if ((plan->hw & (ALP_SOM_ARM_RTC_TIMER | ALP_SOM_ARM_RTC_INT)) != 0u &&
	    alp_som_power_rtc_wake_service(&flags) == ALP_OK &&
	    (flags & (RV3028C7_WAKE_TF | RV3028C7_WAKE_AF)) != 0u) {
		fired |= ((plan->hw & ALP_SOM_ARM_RTC_TIMER) != 0u) ? plan->timed_bit : ALP_POWER_WAKE_RTC;
	}
	return fired;
}

/* Put the SE and the retention registers back to what they were before this cycle
 * touched them: the live OFF profile is written back and the two always-on
 * registers get their snapshot values; all three are read back.  set_off_cfg is not
 * side-effect-free, so "the old profile" alone is not enough. */
typedef struct {
	off_profile_t live;
	uint32_t      ret_ctrl;
	uint32_t      ana_reg1;
} undo_t;

static alp_status_t undo_se(const undo_t *u)
{
	off_profile_t chk;
	alp_status_t  s = se_rc_to_alp(se_service_set_off_cfg((off_profile_t *)&u->live));

	if (alif_se_hw_reg_read(ALIF_SE_REG_RET_CTRL) != u->ret_ctrl) {
		alif_se_hw_reg_write(ALIF_SE_REG_RET_CTRL, u->ret_ctrl);
	}
	if (alif_se_hw_reg_read(ALIF_SE_REG_ANA_REG1) != u->ana_reg1) {
		alif_se_hw_reg_write(ALIF_SE_REG_ANA_REG1, u->ana_reg1);
	}
	if (s != ALP_OK || off_cfg_read(&chk) != 0 || !off_profile_matches(&u->live, &chk) ||
	    alif_se_hw_reg_read(ALIF_SE_REG_RET_CTRL) != u->ret_ctrl ||
	    alif_se_hw_reg_read(ALIF_SE_REG_ANA_REG1) != u->ana_reg1) {
		return ALP_ERR_IO; /* could not be put back: the caller reports it */
	}
	return ALP_OK;
}

static alp_status_t deep_sleep(alp_power_backend_state_t *state,
                               alp_power_mode_t           mode,
                               uint32_t                   wake_after_ms,
                               alp_power_wake_info_t     *info)
{
	sleep_plan_t  plan  = { .mode = mode };
	armed_t       armed = { 0 };
	undo_t        undo;
	off_profile_t off;
	off_profile_t readback;
	alp_status_t  why = ALP_ERR_INVAL;
	alp_status_t  s;
	bool          se_touched = false;
	uint32_t      fired;

	fill_info(info, ALP_POWER_MODE_RUN, 0u, 0u);

	/* 1. Validate everything first. */
	if (!retention_valid(&state->retain, &why)) {
		return refuse(1, "retention_invalid", (int)state->retain.level, why);
	}
	plan.memory_blocks = retained_blocks(&state->retain);
	plan.lfxo =
	    lfxo_confirmed() ||
	    IS_ENABLED(CONFIG_ALP_SDK_POWER_ALIF_SE_BENCH_FORCE_LFXO); /* fixes the LPTIMER rate */
	s = plan_wake(state, wake_after_ms, &plan);
	if (s != ALP_OK) {
		return refuse(1, "wake_plan", (int)wake_after_ms, s);
	}

	/* 2. Refuse what cannot work, before touching anything. */
	s = refuse_if_unfit();
	if (s != ALP_OK) {
		return s;
	}
	s = refuse_if_pending(&plan);
	if (s != ALP_OK) {
		return s;
	}

	/* 3. Build the profile from what the SE reports now.  Read-only so far. */
	int rc = off_cfg_read(&undo.live);

	s = se_rc_to_alp(rc);
	if (s != ALP_OK) {
		return refuse(3, "se_get_off_cfg", rc, s);
	}
	if (undo.live.dcdc_mode == OFF_DCDC_MODE_SENTINEL) {
		undo.live.dcdc_mode = DCDC_MODE_OFF; /* what the client sends to the SE: 0 */
	}
	s = build_off_profile(&off, &undo.live, &plan);
	if (s != ALP_OK) {
		return refuse(3, "live_dcdc_out_of_range", (int)undo.live.dcdc_voltage, s);
	}

	/* 4. Quiesce the SoM domains; on failure it has already put them back. */
	s = alp_som_power_quiesce(mode, NULL);
	if (s != ALP_OK) {
		return refuse(4, "quiesce", (int)s, s);
	}

	/* 5. Arm the wake sources and record the cycle. */
	s = arm(&plan, &armed);
	if (s != ALP_OK) {
		(void)refuse(5, "arm_wake_source", (int)s, s);
		goto unwind;
	}
	save_cycle_record(&plan);

	/* 6. Hand the profile to the SE, read it back, re-assert what its write cleared.
	 * From here on the SE and the two retention registers are changed, so every exit
	 * goes through undo_se(). */
	undo.ret_ctrl = alif_se_hw_reg_read(ALIF_SE_REG_RET_CTRL);
	undo.ana_reg1 = alif_se_hw_reg_read(ALIF_SE_REG_ANA_REG1);
	se_touched    = true;
	rc            = se_service_set_off_cfg(&off);
	s             = se_rc_to_alp(rc);
	if (s != ALP_OK) {
		(void)refuse(6, "se_set_off_cfg", rc, s);
		goto unwind;
	}
	rc = off_cfg_read(&readback);
	s  = se_rc_to_alp(rc);
	if (s != ALP_OK) {
		(void)refuse(6, "se_get_off_cfg_readback", rc, s);
		goto unwind;
	}
	if (!off_profile_matches(&off, &readback)) {
		off_profile_log_diff(&off, &readback);
		(void)refuse(6, "off_profile_mismatch", 0, ALP_ERR_IO);
		s = ALP_ERR_IO;
		goto unwind;
	}
	s = reassert_retention(&plan);
	if (s != ALP_OK) {
		(void)refuse(6, "retention_not_stuck", (int)alif_se_hw_reg_read(ALIF_SE_REG_RET_CTRL), s);
		goto unwind;
	}

	/* The countdown / alarm started above may already have fired.  A latched, enabled
	 * flag means the wake is spent (and with pulse mode the pad may even have gone
	 * high again), so do not sleep on it.  The entry re-reads the pad too, with the
	 * edge interrupt armed. */
	if ((plan.hw & (ALP_SOM_ARM_RTC_TIMER | ALP_SOM_ARM_RTC_INT)) != 0u) {
		bool spent = false;

		if (alp_som_power_rtc_flags_pending(&spent) == ALP_OK && spent) {
			s = refuse(7, "rtc_wake_already_latched", 1, ALP_ERR_BUSY);
			goto unwind;
		}
	}

	/* 7. Enter.  Interrupts are off and the wake pad is armed inside; this does not
	 * return when the subsystem powers down. */
	s = alif_se_hw_enter_ewic((plan.hw & (ALP_SOM_ARM_RTC_TIMER | ALP_SOM_ARM_RTC_INT)) != 0u,
	                          ((plan.hw & ALP_SOM_ARM_LPTIMER) != 0u) ? plan.lptimer_ticks : 0u);
	if (s != ALP_OK) {
		/* INT already asserted / LPTIMER already fired (BUSY), LPTIMER not live (IO), ... */
		(void)refuse(7, alif_se_hw_enter_reason(), (int)s, s);
		goto unwind;
	}

	/* Back here means the subsystem did not power down: a wake source fired first, or
	 * nothing did.  Either way the SE profile and the domains are put back. */
	fired = fired_sources(&plan);
	disarm(&armed);
	(void)alp_som_power_restore(NULL);
	alp_som_pd_store_clear();
	s = undo_se(&undo);
	fill_info(info, ALP_POWER_MODE_RUN, fired, 0u);
	if (s != ALP_OK) {
		return s;
	}
	/* A fired source is an ordinary early wake (OK).  No source fired: the core stayed
	 * up for a reason this backend cannot name, which is a failed sleep, not a wake. */
	return (fired != 0u) ? ALP_OK : refuse(7, "stayed_up_no_source", 0, ALP_ERR_IO);

unwind:
	disarm(&armed);
	(void)alp_som_power_restore(NULL);
	alp_som_pd_store_clear();
	if (se_touched && undo_se(&undo) != ALP_OK) {
		return ALP_ERR_IO;
	}
	return s;
}

/* ---- Ops ------------------------------------------------------------------------ */

/* SLEEP and DEEP_SLEEP are the pm_policy backend's business: this backend takes the
 * "power" class on the E8 (an exact silicon match beats the wildcard), so it
 * forwards them rather than leaving the E8 without them. */
#if defined(CONFIG_ALP_SDK_POWER_PM_POLICY)
#define PM_AVAILABLE 1
#else
#define PM_AVAILABLE 0
#endif

static alp_status_t
se_open(alp_power_backend_state_t *state, alp_capabilities_t *caps_out, uint32_t *wake_caps_out)
{
	uint32_t pm_caps = 0u;

#if PM_AVAILABLE
	alp_status_t s = alp_power_pm_policy_ops.open(state, caps_out, &pm_caps);

	if (s != ALP_OK) {
		return s;
	}
#else
	(void)state;
	(void)caps_out;
#endif
	if (wake_caps_out != NULL) {
		*wake_caps_out = pm_caps | stop_wake_caps();
	}
	return ALP_OK;
}

static alp_status_t se_configure_wake_source(alp_power_backend_state_t *state, uint32_t wake_bitmap)
{
	(void)state;
	(void)wake_bitmap;
	return ALP_OK; /* armed at request time, from the mirrored bitmap */
}

static alp_status_t se_configure_retention(alp_power_backend_state_t *state,
                                           const alp_power_retain_t  *retain)
{
	alp_status_t why = ALP_ERR_INVAL;

	(void)state;
	return retention_valid(retain, &why) ? ALP_OK : why;
}

static alp_status_t se_request_sleep(alp_power_backend_state_t *state,
                                     alp_power_mode_t           mode,
                                     uint32_t                   wake_after_ms,
                                     alp_power_wake_info_t     *info)
{
	if (is_deep_mode(mode)) {
		return deep_sleep(state, mode, wake_after_ms, info);
	}
	if (mode == ALP_POWER_MODE_SLEEP || mode == ALP_POWER_MODE_DEEP_SLEEP) {
#if PM_AVAILABLE
		return alp_power_pm_policy_ops.request_sleep(state, mode, wake_after_ms, info);
#else
		return ALP_ERR_NOSUPPORT;
#endif
	}
	return ALP_ERR_INVAL;
}

static void se_close(alp_power_backend_state_t *state)
{
#if PM_AVAILABLE
	alp_power_pm_policy_ops.close(state);
#else
	(void)state;
#endif
}

static uint32_t se_mode_wake_caps(const alp_power_backend_state_t *state, alp_power_mode_t mode)
{
	(void)state;
	if (is_deep_mode(mode)) {
		return stop_wake_caps();
	}
#if PM_AVAILABLE
	if (mode == ALP_POWER_MODE_SLEEP || mode == ALP_POWER_MODE_DEEP_SLEEP) {
		return ALP_POWER_WAKE_TIMER; /* what the pm_policy backend arms */
	}
#endif
	return 0u;
}

static const alp_power_ops_t _ops = {
	.open                  = se_open,
	.configure_wake_source = se_configure_wake_source,
	.configure_retention   = se_configure_retention,
	.request_sleep         = se_request_sleep,
	.close                 = se_close,
	.mode_wake_caps        = se_mode_wake_caps,
	.domain_policy_set     = alp_som_power_ops_policy_set,
	.domain_info           = alp_som_power_ops_domain_info,
	.boot_wake_info        = alp_som_power_ops_boot_wake_info,
};

ALP_BACKEND_REGISTER(power,
                     alif_se_stop,
                     {
                         .silicon_ref = "alif:ensemble:e8",
                         .vendor      = "alif",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

/* ---- Cold-boot wake decode ------------------------------------------------------ */

/*
 * TODO(#2784 addendum 6): the wake is a cold boot into whatever clock tree the SE left,
 * and bench U8c saw UART5 at ~1/5 of its baud and a slow tick afterwards, so the PLL was
 * not running.  Once the instrumented data (BKRAM slots PRE / BOOT, printed by
 * aen-power-stop) confirms it, re-apply the full explicit RUN profile at PRE_KERNEL_1
 * before any peripheral init, then call uart_configure().  Reference: the vendor sample
 * sdk-alif samples/drivers/pm/system_off does exactly that
 * (SYS_INIT(app_set_run_params, PRE_KERNEL_1, 46) with PLL / 160 MHz / LFXO / MRAM).
 * NOT implemented yet on purpose: instrument first.
 */

/* Part 1, before the timer driver initialises (it clears the status): the LPTIMER. */
void alp_som_power_wake_decode_early(alp_som_pd_record_t *rec)
{
	if ((rec->armed_hw & ALP_SOM_ARM_LPTIMER) != 0u && alif_se_hw_wake_timer_pending()) {
		rec->wake_source |= rec->timed_bit;
	}
}

/* Part 2, once BRD_I2C is up: the RV-3028 flags and the slept time. */
bool alp_som_power_wake_decode_i2c(alp_som_pd_record_t *rec)
{
	uint8_t  flags = 0u;
	uint32_t now   = 0u;
	bool     porf  = false;

	/* A STANDBY record is not vouched for by STOP_MODE_STAT, so it can be a stale one
	 * left in retained SRAM.  An RV-3028 that reports its power-on-reset flag lost
	 * power, i.e. the module was power-cycled since: discard the cycle's wake data.
	 * (The RTSS_HE_RESET cause is no help: 0 means "POR or Secure-Enclave initiated",
	 * which is also what a genuine wake looks like.) */
	if (rec->mode == (uint32_t)ALP_POWER_MODE_STANDBY && alp_som_power_rtc_porf(&porf) == ALP_OK &&
	    porf) {
		return false;
	}

	if ((rec->armed_hw & (ALP_SOM_ARM_RTC_TIMER | ALP_SOM_ARM_RTC_INT)) != 0u &&
	    alp_som_power_rtc_wake_service(&flags) == ALP_OK &&
	    (flags & (RV3028C7_WAKE_TF | RV3028C7_WAKE_AF)) != 0u) {
		/* The countdown the SDK started reports the source the caller asked for; the
		 * caller's own alarm is the RTC. */
		rec->wake_source |=
		    ((rec->armed_hw & ALP_SOM_ARM_RTC_TIMER) != 0u) ? rec->timed_bit : ALP_POWER_WAKE_RTC;
	}

	/* The RV-3028 is the time base (+-1 ppm): a calendar delta, 1 s resolution.  Only
	 * with no RV-3028 reading does a timer wake fall back to its nominal length. */
	if (rec->entry_rtc_s != 0u && alp_som_power_rtc_seconds(&now) == ALP_OK &&
	    now >= rec->entry_rtc_s) {
		rec->slept_ms = (now - rec->entry_rtc_s) * 1000u;
	} else if ((rec->wake_source & rec->timed_bit) != 0u && rec->timed_bit != 0u) {
		rec->slept_ms = rec->armed_ms;
	}
	return true;
}

#endif /* CONFIG_ALP_SDK_POWER_ALIF_SE */
