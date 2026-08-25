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

#include <stdbool.h>
#include <stdint.h>

#include <ti/drivers/Power.h> /* Power_setConstraint/Policy (pulls PowerWFF3.h via DeviceFamily_CC35XX) */

#include <wlan_if.h> /* Wlan_Set -- the RADIO half of the power policy */

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

/* ---- Radio power save -------------------------------------------------------
 *
 * The Power_setPolicy() work below only governs the WFF3 CORE.  On a Wi-Fi part
 * the dominant term is the RADIO: an associated station that never enters power
 * save keeps its receiver up continuously, which costs orders of magnitude more
 * than anything the core's WFI/SLEEP state can save.  Nothing in this firmware
 * ever called Wlan_Set(), so every policy -- including DEEP_SLEEP -- left the
 * radio fully awake and the presets differed only in core state.
 *
 * Three independent knobs, all via Wlan_Set (see wlan_if.h):
 *   WLAN_SET_POWER_SAVE       station PS mode (ACTIVE / AUTO / POWER_SAVE)
 *   WLAN_SET_POWER_MANAGEMENT sleep authorisation (ALWAYS_ACTIVE / ELP)
 *   WLAN_SET_LSI              long sleep interval -- wake every Nth DTIM
 *
 * LSI is what finally separates LOW_POWER from DEEP_SLEEP.  Before this the two
 * presets were documented as sharing one reachable state because WFF3 exposes a
 * single core SLEEP tier; on the radio side they are genuinely different.
 *
 * LATENCY IS THE TRADE.  Waking only every Nth DTIM means inbound frames queue
 * at the AP until the next wake, so DEEP_SLEEP adds hundreds of ms of inbound
 * latency and will cut throughput hard.  That is the point of the preset, and it
 * is why BALANCED -- not a low-power mode -- stays the default. */

/* Latched so a policy set BEFORE the radio exists still lands: Wlan_Set only
 * works once a role is up, and the host may configure power at any time. */
static uint8_t  pp_policy_latched = ALP_CC3501E_PP_BALANCED;
static uint32_t pp_idle_ms_latched;
/* Set by the SPI-dispatch ISR, consumed by the TASK -- see cc3501e_hw_power_service(). */
static volatile bool pp_radio_dirty;
/* Result of the last TASK-side apply, surfaced via cc3501e_hw_power_radio_ok()
 * because POWER_POLICY has no async-result opcode of its own. */
static volatile bool pp_radio_ok = true;

/* Map idle_ms_before_sleep onto a DTIM count.  A DTIM period is typically ~100 ms
 * (beacon 102.4 ms, DTIM 1); treat the host's idle budget as "how long may we
 * stay asleep" and clamp to the field's uint8_t range.  This finally gives
 * idle_ms_before_sleep a meaning -- it was previously accepted and discarded
 * because PowerWFF3 has no idle-hysteresis setter. */
static uint8_t pp_idle_ms_to_dtims(uint32_t idle_ms)
{
	uint32_t dtims = idle_ms / 100u;

	if (dtims < 2u) {
		dtims = 2u; /* N_DTIM below 2 is just DTIM */
	}
	if (dtims > 255u) {
		dtims = 255u;
	}
	return (uint8_t)dtims;
}

/* Apply the radio half of @p policy.  Best-effort: returns 0 when the radio is
 * not up yet (the caller latches and re-applies after role-up) and never fails
 * the whole policy call, so a host that sets power before Wi-Fi still gets the
 * core-side policy applied. */
static bool pp_apply_radio(uint8_t policy, uint32_t idle_ms)
{
	uint8_t               ps;
	WlanPowerManagement_e pm;
	bool                  want_lsi = false;

	switch (policy) {
	case ALP_CC3501E_PP_PERFORMANCE:
		ps = (uint8_t)WLAN_STATION_ACTIVE_MODE;
		pm = POWER_MANAGEMENT_ALWAYS_ACTIVE_MODE;
		break;
	case ALP_CC3501E_PP_BALANCED:
		ps = (uint8_t)WLAN_STATION_AUTO_PS_MODE;
		pm = POWER_MANAGEMENT_ELP_MODE;
		break;
	case ALP_CC3501E_PP_LOW_POWER:
		/* Sleep between DTIMs but wake on EVERY one: still responsive to
		 * downlink traffic within one beacon period. */
		ps = (uint8_t)WLAN_STATION_POWER_SAVE_MODE;
		pm = POWER_MANAGEMENT_ELP_MODE;
		break;
	case ALP_CC3501E_PP_DEEP_SLEEP:
		ps       = (uint8_t)WLAN_STATION_POWER_SAVE_MODE;
		pm       = POWER_MANAGEMENT_ELP_MODE;
		want_lsi = true;
		break;
	default:
		return false;
	}

	bool ok = (Wlan_Set(WLAN_SET_POWER_SAVE, &ps) >= 0);

	ok = (Wlan_Set(WLAN_SET_POWER_MANAGEMENT, &pm) >= 0) && ok;

	if (want_lsi || policy == ALP_CC3501E_PP_LOW_POWER) {
		WlanLongSleepInterval lsi = { 0 };

		lsi.WakeUpEvent    = want_lsi ? (uint8_t)WAKE_UP_EVENT_N_DTIM : (uint8_t)WAKE_UP_EVENT_DTIM;
		lsi.ListenInterval = want_lsi ? pp_idle_ms_to_dtims(idle_ms) : 1u;
		ok                 = (Wlan_Set(WLAN_SET_LSI, &lsi) >= 0) && ok;
	}
	return ok;
}

/* Re-apply the latched radio policy after a role comes up.  Called from the
 * Wi-Fi path once Wlan_RoleUp(STA) succeeds -- without this a power policy set
 * while the radio was down would be silently lost. */
void cc3501e_hw_power_reapply_radio(void)
{
	pp_radio_dirty = true;
}

/* TASK-context drain for the latched radio policy.  Called from cc3501e_hw_tick().
 * Applying with no role up is not an error -- Wlan_Set legitimately refuses then,
 * and the STA role-up path re-arms the dirty flag once the radio exists. */
void cc3501e_hw_power_service(void)
{
	if (!pp_radio_dirty) {
		return;
	}
	pp_radio_dirty = false;

	const bool radio_up = (cc3501e_hw_radio_role() != ALP_CC3501E_ROLE_OFF);
	const bool ok       = pp_apply_radio(pp_policy_latched, pp_idle_ms_latched);

	pp_radio_ok = radio_up ? ok : true;
}

bool cc3501e_hw_power_radio_ok(void)
{
	return pp_radio_ok;
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
		 * same CORE state on this device -- WFF3 exposes a single SLEEP state
		 * (PowerWFF3_SLEEP), not a separate deep-sleep tier.  They differ on the
		 * RADIO: see pp_apply_radio(), where DEEP_SLEEP takes the N-DTIM long
		 * sleep interval and LOW_POWER wakes every DTIM. */
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
	/* deferred: per-bit wake_events -> HW SLEEP wake mask -- no PowerWFF3 wake-source API.
	 * (Still true for the CORE.  The RADIO's wake behaviour IS now configured,
	 * via WLAN_SET_LSI's WakeUpEvent -- see pp_apply_radio().) */

	/* idle_ms_before_sleep: PowerWFF3_sleepPolicy derives the sleep decision from
	 * the time until the next scheduled ClockP event vs the SLEEP transition
	 * latency (PowerWFF3_TOTALTIMESLEEP); a value of 0 means "use that policy
	 * default", which is exactly what running the stock policy does.  A nonzero
	 * minimum-idle threshold cannot be programmed: PowerWFF3.h exposes no
	 * idle-hysteresis setter, only the fixed latency constants. */
	/* Core policy is set; now the radio -- the dominant term.  Latch first so a
	 * policy set before Wlan_RoleUp() is re-applied by
	 * cc3501e_hw_power_reapply_radio() once the STA role is up. */
	pp_policy_latched  = policy;
	pp_idle_ms_latched = idle_ms_before_sleep;

	/* Latch the radio half for the TASK.  This function runs in SPI-DISPATCH (ISR)
	 * context, and Wlan_Set() is a blocking vendor radio call -- the same reason
	 * handle_sock_recv() cannot call lwip_recv() and the worker seam exists at all.
	 *
	 * Calling it inline here does not merely fail: bench-measured on
	 * e1m-aen-evk-01, every preset returned -4 AND the bridge itself went to
	 * PING -> -5 on the later presets.  So the radio work is deferred to
	 * cc3501e_hw_power_service(), which the bringup task drains via
	 * cc3501e_hw_tick().
	 *
	 * CONSEQUENCE FOR THE WIRE CONTRACT: a RESP_OK to POWER_POLICY means QUEUED,
	 * not APPLIED -- the same semantic OTA_BEGIN turned out to have.  A host that
	 * needs the realised state polls it; see cc3501e_hw_power_radio_ok(). */
	pp_radio_dirty = true;

	return CC3501E_HW_OK;
}
