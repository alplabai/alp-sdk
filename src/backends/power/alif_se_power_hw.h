/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * Silicon seam of the Alif SE STOP / STANDBY backend (#2784, unit U7).  INTERNAL.
 *
 * alif_se_power.c holds every decision (validation, refusals, the OFF profile,
 * the wake-source choice, the wake decode) and touches no register itself; this
 * seam is the only code that does.  alif_se_power_hw.c implements it on the real
 * part, tests/unit/power_alif_se replaces it with a recording fake, which is how
 * the decisions are covered on native_sim.
 */

#ifndef ALP_BACKENDS_POWER_ALIF_SE_POWER_HW_H
#define ALP_BACKENDS_POWER_ALIF_SE_POWER_HW_H

#include <stdbool.h>
#include <stdint.h>

#include <alp/peripheral.h>

/** The always-on VBAT / ANA registers the backend checks and re-asserts after
 *  the Secure Enclave has written its profile. */
typedef enum {
	ALIF_SE_REG_RET_CTRL = 0, /**< VBAT.RET_CTRL       (VBAT + 0x0C) */
	ALIF_SE_REG_ANA_REG1,     /**< ANA.VBAT_ANA_REG1   (ANA  + 0x38) */
	ALIF_SE_REG_ANA_MISC,     /**< ANA.MISC_CTRL       (ANA  + 0x00) */
} alif_se_reg_t;

uint32_t alif_se_hw_reg_read(alif_se_reg_t reg);
void     alif_se_hw_reg_write(alif_se_reg_t reg, uint32_t value);

/** DHCSR.C_DEBUGEN: a debugger is attached (keeps PDDEBUG powered). */
bool alif_se_hw_debugger_attached(void);

/** The D-cache is enabled (SCB->CCR.DC).  Cleaning it hangs on this silicon, so
 *  the sleep is refused rather than attempted. */
bool alif_se_hw_dcache_active(void);

/** PWRMODCTL.CPDLPSTATE core / EPU / RAMS low-power-state requests are all OFF,
 *  the precondition the vendor subsystem-off sequence assumes. */
bool alif_se_hw_lpstate_off(void);

/* ---- LPTIMER wake timer (the `alp,power-wake-timer` chosen node) ------------ */

/** The chosen wake timer exists, is enabled and its driver is ready. */
bool alif_se_hw_wake_timer_present(void);
/** Counter frequency in Hz as the devicetree states it (LFRC actually runs ~4.5 %
 *  faster; the caller documents that). */
uint32_t alif_se_hw_wake_timer_hz(void);
/** Program a one-shot underflow in @p ticks counts and unmask its interrupt. */
alp_status_t alif_se_hw_wake_timer_arm(uint32_t ticks);
/** Stop the timer and mask its interrupt.  Idempotent. */
void alif_se_hw_wake_timer_disarm(void);
/** The timer's underflow status is latched.  Reads the register directly, so it is
 *  valid on the early cold-boot path before the driver has initialised. */
bool alif_se_hw_wake_timer_pending(void);

/* ---- RV-3028 /INT -> P15_0 -> LPGPIO0 wake input ---------------------------- */

/** The RTC domain declares a `wake-gpios` input. */
bool alif_se_hw_rtc_int_present(void);
/** Logical level of /INT: 1 = asserted (a wake is already pending), 0 = idle,
 *  negative = cannot be read. */
int alif_se_hw_rtc_int_asserted(void);
/** Make the pad a plain input and check it is usable.  The interrupt itself is
 *  switched on only inside alif_se_hw_enter_ewic(), under the interrupt lock. */
alp_status_t alif_se_hw_rtc_int_arm(void);
/** Switch the pad interrupt and the combined line off.  Idempotent. */
void alif_se_hw_rtc_int_disarm(void);

/* ---- Entry ------------------------------------------------------------------ */

/**
 * Enter the EWIC subsystem-off sleep: interrupts off (PRIMASK, BASEPRI 0, as the
 * DFP does), the wake pad armed (@p rtc_int: falling-edge interrupt on the
 * RV-3028 /INT pad plus the LPGPIO combined line toward the EWIC), read-modify-write
 * of RTSS_HE_CTRL (WIC on, EWIC selected, COLD_WAKEUP cleared so the power domain
 * may drop), SLEEPDEEP, WFI.  On success the core loses power and the wake is a
 * cold boot, so this does not return.  It returns only when the sleep was aborted (a
 * wake source fired before power was removed): the wake pad is disarmed again, and
 * RTSS_HE_CTRL and the core state are put back exactly as found, all before
 * interrupts are re-enabled.  A failure to arm the pad returns its error without
 * entering, and so does a pad that already reads asserted once the edge is armed
 * (ALP_ERR_BUSY: the RV-3028 fired before the interrupt existed).
 */
alp_status_t alif_se_hw_enter_ewic(bool rtc_int);

#endif /* ALP_BACKENDS_POWER_ALIF_SE_POWER_HW_H */
