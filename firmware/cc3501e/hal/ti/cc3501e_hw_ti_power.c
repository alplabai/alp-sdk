/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- power policy (CMD_POWER_POLICY, 0x62)
 * via the CC35xx (WFF3) Power driver.
 *
 * Split by hardware subsystem out of cc3501e_hw_ti.c (issue #703, #461
 * Phase B).  cc3501e_hw_ti.c keeps platform lifecycle + the deferred-reboot
 * latch; see cc3501e_hw_ti_internal.h for the cross-TU seam.
 *
 * Built ONLY for CC3501E_HAL_BACKEND=ti (the bench build), against TI's
 * SimpleLink CC35xx SDK.  CI builds the stub backend instead, so this file
 * is never on the SDK-free path.
 */

#include <stdint.h>

#include <ti/drivers/Power.h> /* Power_setConstraint/Policy (pulls PowerWFF3.h via DeviceFamily_CC35XX) */

#include "alp/protocol/cc3501e.h"

#include "../cc3501e_hw.h"

/* --------------------------------------------------------------- */
/* Power policy (CMD_POWER_POLICY, 0x62) -- real CC35xx Power driver. */
/*                                                                   */
/* The host's coarse policy preset maps onto the CC35xx (WFF3) Power  */
/* manager: the idle-loop Power policy function (Power_setPolicy +    */
/* Power_enablePolicy) plus a balanced set of operational constraints */
/* (Power_setConstraint / Power_releaseConstraint).  Grounded in      */
/* <ti/drivers/Power.h> (Power_setConstraint/releaseConstraint/       */
/* setPolicy/enablePolicy) and <ti/drivers/power/PowerWFF3.h>         */
/* (PowerWFF3_DISALLOW_SLEEP/_IDLE, PowerWFF3_doWFI,                  */
/* PowerWFF3_sleepPolicy) -- the latter is auto-included by Power.h    */
/* under DeviceFamily_CC35XX.                                         */
/*                                                                   */
/* Constraints are REFERENCE-COUNTED per id by the Power manager       */
/* (PowerWFF3.c constraintCounts[]), and the bridge SPI driver itself  */
/* transiently sets/releases PowerWFF3_DISALLOW_SLEEP around each      */
/* transfer -- so this HAL owns AT MOST ONE long-lived reference per   */
/* constraint id and tracks it in pp_constraints_held, releasing the   */
/* previous policy's references before declaring the new ones.  That   */
/* keeps our count balanced (Power_releaseConstraint asserts a         */
/* non-zero count) and never disturbs the SPI driver's own count. */

/* Bitmask (1 << PowerWFF3_DISALLOW_*) of the constraints this HAL currently
 * holds on the host policy's behalf.  Starts empty (boot default = whatever the
 * SysConfig PowerWFF3_Config selected); each apply re-derives the desired set. */
static uint8_t pp_constraints_held;

/* Declare @id (a PowerWFF3_DISALLOW_* constraint) on the policy's behalf if not
 * already held; idempotent so re-applying the same policy is a no-op. */
static void pp_hold_constraint(uint8_t id)
{
	const uint8_t bit = (uint8_t)(1u << id);
	if ((pp_constraints_held & bit) == 0u) {
		Power_setConstraint(id); /* WFF3: always Power_SOK */
		pp_constraints_held |= bit;
	}
}

/* Release @id if this HAL holds it (balanced against pp_hold_constraint so the
 * Power manager's per-id count never underflows -- it asserts count != 0). */
static void pp_release_constraint(uint8_t id)
{
	const uint8_t bit = (uint8_t)(1u << id);
	if ((pp_constraints_held & bit) != 0u) {
		Power_releaseConstraint(id);
		pp_constraints_held &= (uint8_t)~bit;
	}
}

int cc3501e_hw_set_power_policy(uint8_t policy, uint8_t wake_events, uint32_t idle_ms_before_sleep)
{
	/* Validate per the header contract: an all-zero wake_events bitmap is only
	 * meaningful for the non-sleeping presets (PERFORMANCE / BALANCED); a
	 * low-power preset with NO declared wake source would idle the device with
	 * no way back, so reject it up front (the host must keep at least
	 * ALP_CC3501E_WAKE_HOST_SPI for a low-power policy). */
	if (wake_events == ALP_CC3501E_WAKE_NONE &&
	    (policy == ALP_CC3501E_PP_LOW_POWER || policy == ALP_CC3501E_PP_DEEP_SLEEP)) {
		return CC3501E_HW_ERR_INVAL;
	}

	switch (policy) {
	case ALP_CC3501E_PP_PERFORMANCE:
		/* Lowest latency: forbid SLEEP and IDLE so the idle loop only ever
		 * clock-gates via WFI (PowerWFF3_doWFI), which any peripheral IRQ --
		 * the bridge SPI CS, a GPIO edge -- wakes immediately. */
		pp_hold_constraint(PowerWFF3_DISALLOW_SLEEP);
		pp_hold_constraint(PowerWFF3_DISALLOW_IDLE);
		Power_setPolicy(PowerWFF3_doWFI);
		Power_enablePolicy();
		break;
	case ALP_CC3501E_PP_BALANCED:
		/* Default: let the aggressive sleep policy opportunistically IDLE/SLEEP
		 * between events but keep no extra constraints -- the policy already
		 * falls back to IDLE then WFI when SLEEP is inappropriate. */
		pp_release_constraint(PowerWFF3_DISALLOW_SLEEP);
		pp_release_constraint(PowerWFF3_DISALLOW_IDLE);
		Power_setPolicy(PowerWFF3_sleepPolicy);
		Power_enablePolicy();
		break;
	case ALP_CC3501E_PP_LOW_POWER:
	case ALP_CC3501E_PP_DEEP_SLEEP:
		/* Aggressive idle: drop all DISALLOW constraints so the sleep policy can
		 * reach the deepest state its latency budget allows (PowerWFF3_SLEEP),
		 * waking on the Power driver's hardwired sleep wake sources (RTC +
		 * CSYSPWRUPREQ, configured inside Power_init / PowerWFF3_sleepPolicy) and
		 * any still-clocked peripheral IRQ.  DEEP_SLEEP and LOW_POWER share the
		 * same reachable state on this device -- WFF3 exposes a single SLEEP
		 * state (PowerWFF3_SLEEP), not a separate deep-sleep tier. */
		pp_release_constraint(PowerWFF3_DISALLOW_SLEEP);
		pp_release_constraint(PowerWFF3_DISALLOW_IDLE);
		Power_setPolicy(PowerWFF3_sleepPolicy);
		Power_enablePolicy();
		break;
	default:
		return CC3501E_HW_ERR_INVAL;
	}

	/* wake_events: the routed sources (HOST_SPI / GPIO_IRQ / BLE / Wi-Fi) wake
	 * the core through their own still-clocked peripheral interrupts while in the
	 * WFF3 SLEEP state -- the validation above is the load-bearing use of the
	 * bitmap.  A per-source SLEEP wake-MASK has no public SDK surface: the WFF3
	 * Power driver hardwires its sleep wake sources (RTC + CSYSPWRUPREQ) in
	 * Power_init/PowerWFF3_sleepPolicy and neither <ti/drivers/Power.h> nor
	 * PowerWFF3.h exposes a Power_setWakeup()/configure-wake API (GPIO.h offers
	 * only GPIO_CFG_SHUTDOWN_WAKE_*, a per-pin SHUTDOWN -- not SLEEP -- knob
	 * applied at GPIO config time, not here). */
	/* deferred: per-bit wake_events -> HW SLEEP wake mask -- no PowerWFF3 wake-source API. */

	/* idle_ms_before_sleep: PowerWFF3_sleepPolicy derives the sleep decision from
	 * the time until the next scheduled ClockP event vs the SLEEP transition
	 * latency (PowerWFF3_TOTALTIMESLEEP); a value of 0 means "use that policy
	 * default", which is exactly what running the stock policy does.  A nonzero
	 * minimum-idle threshold cannot be programmed: PowerWFF3.h exposes no
	 * idle-hysteresis setter, only the fixed latency constants. */
	/* deferred: nonzero idle_ms_before_sleep threshold -- no PowerWFF3 idle-hysteresis setter. */
	(void)idle_ms_before_sleep;

	return CC3501E_HW_OK;
}
