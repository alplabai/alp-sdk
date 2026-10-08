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
/** Configure the pad as a level-triggered interrupt input and open the LPGPIO
 *  combined interrupt toward the EWIC. */
alp_status_t alif_se_hw_rtc_int_arm(void);
/** Undo alif_se_hw_rtc_int_arm().  Idempotent. */
void alif_se_hw_rtc_int_disarm(void);

/* ---- Entry ------------------------------------------------------------------ */

/**
 * Enter the EWIC subsystem-off sleep: read-modify-write RTSS_HE_CTRL (WIC on,
 * EWIC selected, COLD_WAKEUP cleared so the power domain may drop), SLEEPDEEP,
 * WFI.  Called with interrupts locked.  On success the core loses power and the
 * wake is a cold boot, so this does not return; it returns only when the sleep
 * was aborted (a wake source fired before power was removed), with the register
 * and core state put back.
 */
void alif_se_hw_enter_ewic(void);

#endif /* ALP_BACKENDS_POWER_ALIF_SE_POWER_HW_H */
