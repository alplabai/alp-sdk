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
/** Raw PWRMODCTL.CPDLPSTATE, for the refusal diagnostic. */
uint32_t alif_se_hw_lpstate_read(void);

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

/** CGU / CLKCTL_SYS registers for the clock-restore trigger and its log. */
#define ALIF_SE_CGU_OSC_CTRL      0u
#define ALIF_SE_CGU_PLL_LOCK_CTRL 1u
#define ALIF_SE_CGU_PLL_CLK_SEL   2u
#define ALIF_SE_CGU_ACLK_CTRL     3u
uint32_t alif_se_hw_cgu_read(unsigned which);

/** Free-running cycle counter (DWT CYCCNT, enabled and left as found by the caller's
 *  convenience: TRCENA / CYCCNTENA are turned on if off).  For bounding waits when SysTick
 *  is not running yet (PRE_KERNEL_1), where k_busy_wait() would spin forever. */
uint32_t alif_se_hw_cycles(void);

/** LPGPIO EXT_PORTA (the pins as the on-chip GPIO block sees them). */
uint32_t alif_se_hw_lpgpio_ext_porta(void);

/** AON RTSS_HE_LPPERI_CKEN (0x1A60401C): the LP peripheral clock enables (the LPGPIO clock
 *  gate reads as an asserted active-low pad when off). */
uint32_t alif_se_hw_lpperi_cken(void);

/** LPRTC CCVR (the counter, units unproven: ~2 Hz on LFRC). */
uint32_t alif_se_hw_lprtc_ccvr(void);

/** This image runs on the M55-HP (CONFIG_SOC_AE822FA0E5597LS0_RTSS_HP).  The HP has no TCM
 *  retention and only a SOFT_OFF-class sleep (see alif_se_power.c), so the backend branches
 *  on it; it is a function so the host test can drive both cores from one binary. */
bool alif_se_hw_core_is_hp(void);

/** Raw LPRTC register at byte offset @p off (CCR 0x0C, CPSR 0x20, CPCVR 0x24); no clock
 *  enable. */
uint32_t alif_se_hw_lprtc_read(unsigned off);

/** This image's vector table base (SCB->VTOR), for the vendor-style OFF profile bench
 *  variant. */
uint32_t alif_se_hw_vtor_read(void);

/* ---- Entry ------------------------------------------------------------------ */

/**
 * Enter the EWIC subsystem-off sleep: interrupts off (PRIMASK, BASEPRI 0, as the
 * DFP does), the wake pad armed (@p rtc_int: falling-edge interrupt on the
 * RV-3028 /INT pad plus the LPGPIO combined line toward the EWIC), read-modify-write
 * of RTSS_HE_CTRL (WIC on, EWIC selected, COLD_WAKEUP cleared so the power domain
 * may drop), SLEEPDEEP, WFI.  When @p lptimer_ticks is non-zero the wake timer is armed
 * LAST, inside the interrupt-off section, and verified live (enabled, unmasked, not
 * already fired): ALP_ERR_BUSY = it had already fired, ALP_ERR_IO = not counting or
 * masked; either way nothing is entered.  On success the core loses power and the wake is a
 * cold boot, so this does not return.  It returns only when the sleep was aborted (a
 * wake source fired before power was removed): the wake pad is disarmed again, and
 * RTSS_HE_CTRL and the core state are put back exactly as found, all before
 * interrupts are re-enabled.  A failure to arm the pad returns its error without
 * entering, and so does a pad that already reads asserted once the edge is armed
 * (ALP_ERR_BUSY: the RV-3028 fired before the interrupt existed).
 */
alp_status_t alif_se_hw_enter_ewic(bool rtc_int, uint32_t lptimer_ticks);

/** Why the last alif_se_hw_enter_ewic() refused (a static string), for the diagnostic. */
const char *alif_se_hw_enter_reason(void);

/* RTSS_x_CTRL WIC [9:8]: bit 8 = WICCONTROL[0] (WIC enable), bit 9 = WICCONTROL[1] (IWIC,
 * 0 selects the EWIC). */
#define ALIF_SE_CTRL_WIC_EN   (1u << 8)
#define ALIF_SE_CTRL_WIC_IWIC (1u << 9)
#define ALIF_SE_CTRL_WIC_MASK (ALIF_SE_CTRL_WIC_EN | ALIF_SE_CTRL_WIC_IWIC)

/** The pure half of alp_som_power_core_off_take(): `ctrl` is the raw RTSS_x_CTRL.  Returns
 *  whether it says "EWIC subsystem-off requested" (WIC [9:8] == 0b01); `*cleared` is `ctrl`
 *  with WIC [9:8] cleared and COLD_WAKEUP and every other bit untouched. */
static inline bool alif_se_ctrl_core_off(uint32_t ctrl, uint32_t *cleared)
{
	*cleared = ctrl & ~ALIF_SE_CTRL_WIC_MASK;
	return (ctrl & ALIF_SE_CTRL_WIC_MASK) == ALIF_SE_CTRL_WIC_EN;
}

#endif /* ALP_BACKENDS_POWER_ALIF_SE_POWER_HW_H */
