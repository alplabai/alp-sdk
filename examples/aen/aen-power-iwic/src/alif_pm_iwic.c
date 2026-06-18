/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stage-B Alif IWIC deep-sleep PM hook for the E1M-AEN801 (Ensemble E8,
 * Cortex-M55-HE) -- a minimal CONFIG_PM pm_state_set() that the pinned upstream
 * Zephyr 4.4.0 Alif SoC layer does NOT provide.
 *
 * WHY THIS FILE EXISTS (the pinned-tree PM gap)
 * ---------------------------------------------
 * Upstream Zephyr's PM subsystem calls pm_state_set() unconditionally
 * (subsys/pm/pm.c:246) and each SoC is expected to supply it (every other SoC
 * ships soc/<vendor>/.../power.c with a __weak pm_state_set).  The pinned
 * Zephyr 4.4.0 Alif Ensemble SoC layer ships NO power.c and does NOT
 * `select HAS_PM` (only the alifsemi/zephyr_alif FORK's
 * soc/alif/common/rtss/power.c does).  So over the pinned tree CONFIG_PM is
 * both unreachable (HAS_PM unselected) AND unlinkable (no pm_state_set).
 *
 * This Tier-1.5 file closes BOTH halves at the app/board level, with no edit to
 * upstream zephyr/:  the companion Kconfig `select HAS_PM`s, and this TU defines
 * pm_state_set()/pm_state_exit_post_ops() so CONFIG_PM links.  It is the in-tree
 * "thin over the vendor SDK" body the SoC layer omits; it deliberately
 * implements ONLY the suspend-to-idle (deep IWIC) state -- the lightest Alif
 * deep sleep, which needs no S2RAM context save and no Secure-Enclave call, so
 * it is buildable + bench-runnable standalone.  suspend-to-ram / soft-off
 * (EWIC + subsystem-off + SE retention config) are intentionally left to the
 * full fork power.c port; entering them here would be a no-op/!-EBUSY at best.
 *
 * REGISTER FACTS (all confirmed against the Alif CMSIS DFP, E8 = AE822FA0E5597)
 * ---------------------------------------------------------------------------
 *   AON base            = 0x1A604000   (DFP soc.h: AON_BASE)
 *   AON.RTSS_HE_CTRL    = AON + 0x10   (DFP soc.h AON_Type: RTSS_HE_CTRL @0x10)
 *   WICCONTROL (HE)     = 0x1A604010   (== AON.RTSS_HE_CTRL; this is the HE core
 *                                       WIC-control register)
 *   WICCONTROL bit 8    = WIC enable   (DFP pm.c WICCONTROL_WIC_Pos = 8)
 *   WICCONTROL bit 9    = IWIC select  (DFP pm.c WICCONTROL_IWIC_Pos = 9;
 *                                       1 = IWIC, 0 = EWIC)
 * The deep-sleep entry sequence (set WICCONTROL WIC|IWIC, raise CPDLPSTATE.CLPSTATE
 * to ON_CLK_OFF to avoid core state loss, set SCB SLEEPDEEP, DSB/ISB, WFI, then
 * restore) mirrors the Alif DFP pm.c pm_core_enter_deep_sleep() and the
 * Apache-2.0 fork soc/alif/common/rtss/power.c.  IWIC wake sources are NVIC
 * interrupts 0-63 + events/NMI/debug (DFP pm.c note); the M55 SysTick is
 * architectural exception #15, NOT an NVIC IRQ, and always wakes WFI/WFE.
 *
 * Console / bench: this file carries no I/O; the example main() reports the
 * RESULT line over the RAM console.  No /home paths, no DFP source, no
 * license-gated content -- only the transcribed register addresses/bits above.
 */

#include <zephyr/kernel.h>
#include <zephyr/pm/pm.h>
#include <zephyr/sys/sys_io.h>

/*
 * WICCONTROL for the RTSS-HE core, by absolute address (the pinned Alif SoC
 * header exposes no AON struct / macro -- soc.h is bare CMSIS).  HE core only:
 * the HP core would use AON.RTSS_HP_CTRL = AON_BASE + 0x0 = 0x1A604000.
 */
#define AEN_AON_BASE      0x1A604000UL
#define AEN_WICCONTROL_HE (AEN_AON_BASE + 0x10UL) /* AON.RTSS_HE_CTRL */

#define WICCONTROL_WIC_Pos  8U
#define WICCONTROL_WIC_Msk  (1UL << WICCONTROL_WIC_Pos)
#define WICCONTROL_IWIC_Pos 9U
#define WICCONTROL_IWIC_Msk (1UL << WICCONTROL_IWIC_Pos)

/* WICCONTROL bit9 select values. */
#define PM_WIC_IS_EWIC 0U /* WIC used is EWIC */
#define PM_WIC_IS_IWIC 1U /* WIC used is IWIC */

/*
 * CPDLPSTATE.CLPSTATE low-power-state field values (M55 core power domain).
 * We raise to ON_CLK_OFF before an IWIC sleep so the core does NOT lose state
 * (the IWIC + CPU share a power domain, so both must stay on to wake).
 */
#define PM_LPSTATE_ON         0U
#define PM_LPSTATE_ON_CLK_OFF 1U

/*
 * Architectural deep WIC sleep: program WICCONTROL, set SLEEPDEEP, barrier, WFI.
 * Called with interrupts disabled.  Returns after the wake interrupt fires.
 */
static void aen_enter_wic_sleep(uint32_t wic_is_iwic)
{
	uint32_t regval = WICCONTROL_WIC_Msk | (wic_is_iwic ? WICCONTROL_IWIC_Msk : 0U);

	/* Select deep WIC sleep of the requested type (IWIC here). */
	sys_write32(regval, AEN_WICCONTROL_HE);

	/* Request deep sleep at the architectural level. */
	SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
	__DSB();
	__ISB();

	/* Park the core.  An IWIC wake source (NVIC IRQ 0-63 / event / SysTick
	 * exception) brings it back here. */
	__WFI();

	/* Leave deep sleep and clear the WIC selection. */
	SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;
	sys_write32(0U, AEN_WICCONTROL_HE);
	__DSB();
	__ISB();
}

/*
 * suspend-to-idle == Alif "deep IWIC sleep": raise CLPSTATE to ON_CLK_OFF so the
 * core keeps its state, take the IWIC WIC sleep, then restore CLPSTATE.
 */
static void aen_enter_deep_sleep_iwic(void)
{
	uint32_t old_cpdlpstate = PWRMODCTL->CPDLPSTATE;

	PWRMODCTL->CPDLPSTATE = (old_cpdlpstate & ~PWRMODCTL_CPDLPSTATE_CLPSTATE_Msk) |
	                        (PM_LPSTATE_ON_CLK_OFF << PWRMODCTL_CPDLPSTATE_CLPSTATE_Pos);

	aen_enter_wic_sleep(PM_WIC_IS_IWIC);

	PWRMODCTL->CPDLPSTATE = old_cpdlpstate;
}

/*
 * PM subsystem entry: lower the CPU into the requested state.  Only
 * suspend-to-idle (deep IWIC sleep) is implemented on this batch; the deeper
 * Alif states are documented as the full-fork-port work item.  Called by the PM
 * idle path with the scheduler locked.
 */
void pm_state_set(enum pm_state state, uint8_t substate_id)
{
	ARG_UNUSED(substate_id);

	switch (state) {
	case PM_STATE_SUSPEND_TO_IDLE:
		__disable_irq();
		__set_BASEPRI(0);
		aen_enter_deep_sleep_iwic();
		break;
	default:
		/* Deeper states not wired in this Stage-B body -- the PM policy
		 * only ever selects suspend-to-idle here (it is the sole
		 * enabled power-state in devicetree). */
		break;
	}
}

/*
 * PM subsystem exit: re-enable interrupts the idle path masked before sleeping.
 * Mirrors the fork pm_state_exit_post_ops().
 */
void pm_state_exit_post_ops(enum pm_state state, uint8_t substate_id)
{
	ARG_UNUSED(state);
	ARG_UNUSED(substate_id);

	__enable_irq();

	/* Re-enable interrupts disabled when the kernel entered idle. */
	irq_unlock(0);
}
