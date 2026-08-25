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

#ifdef CC3501E_BLE
#include "cc3501e_nimble_host.h" /* cc3501e_nimble_host_is_enabled -- see pp_long_sleep_safe() */
#endif

/* Whether the N-DTIM LONG sleep interval may be programmed right now.
 *
 * It may not, while BLE is up.  Wi-Fi and BLE share this device's HIF, and
 * sleeping through several DTIM periods starves BLE with it: bench-measured on
 * e1m-aen-evk-01 with BLE enabled and advertising between presets, PERFORMANCE
 * and LOW_POWER are clean (BLE adv 0/0, PING 0) and the damage always starts at
 * DEEP_SLEEP or the step right after it -- one run lost the BLE op immediately
 * (`DEEP_SLEEP ... BLE adv -4/-4`), another lost the following BALANCED.  Once
 * lost, the bridge stays at PING -> -5 for the rest of the run.
 *
 * DEEP_SLEEP is the ONLY preset that programs WAKE_UP_EVENT_N_DTIM, and its
 * constraint work is a no-op (LOW_POWER already released them), so the long sleep
 * interval is the only thing that distinguishes it.
 *
 * This was suspected first and wrongly discarded: withholding the LSI back then
 * appeared to just move the wedge, but that run still carried the ISR-context and
 * policy-swap races since fixed.  With those gone the signal is clean.
 *
 * So DEEP_SLEEP keeps its power-save mode and ELP sleep authorisation but falls
 * back to waking every DTIM -- LOW_POWER's radio behaviour -- for as long as BLE
 * is enabled.  Wi-Fi-only builds and BLE-disabled runs get the full interval. */
static bool pp_long_sleep_safe(void)
{
#ifdef CC3501E_BLE
	return !cc3501e_nimble_host_is_enabled();
#else
	return true;
#endif
}

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

/* Apply the CORE half of @p policy.  TASK CONTEXT ONLY.
 *
 * Power_setPolicy() swaps the function pointer the idle loop runs and
 * Power_enablePolicy() re-arms it.  Those are configuration calls, not runtime
 * ones: driving them from the SPI-dispatch ISR races the idle loop that may be
 * executing the very policy being replaced.
 *
 * That race is issue #1683.  With BLE enabled, applying a preset intermittently
 * wedged the whole device -- the policy call timed out and the bridge went to
 * PING -> -5, unrecoverable without a reset, at a point that moved between runs.
 * It is NOT the radio half: the wedge survived skipping every Wlan_Set() call.
 * BLE does not cause it either, it just loads the shared HIF enough to lose the
 * race reliably -- which is why a Wi-Fi-only build looked fine for so long.
 *
 * The constraint helpers were never the problem: they are balanced by
 * pp_constraints_held, and Power_setConstraint/releaseConstraint are ISR-safe. */
static void pp_apply_core(uint8_t policy)
{
	/* Enable the configured policy ONCE.  PowerWFF3_config already names
	 * PowerWFF3_sleepPolicy as policyFxn, but nothing enables it at init, so
	 * before the first POWER_POLICY the core never idled at all. */
	static bool policy_enabled;

	if (!policy_enabled) {
		Power_enablePolicy();
		policy_enabled = true;
	}

	/* CONSTRAINTS ONLY -- deliberately no Power_setPolicy() here.
	 *
	 * PowerWFF3_sleepPolicy already expresses all four presets on its own: it
	 * "considers active constraints ... the first goal is to enter SLEEP; if that
	 * is not appropriate ... the secondary goal is the IDLE state; if that is
	 * disallowed ... the policy will fallback and simply invoke WFI"
	 * (PowerWFF3.h).  Holding DISALLOW_SLEEP + DISALLOW_IDLE therefore gives
	 * exactly what PowerWFF3_doWFI gives, with no policy swap.
	 *
	 * Swapping the policy function at runtime is what made #1683 intermittent.
	 * Power_setPolicy() replaces the pointer the idle loop invokes, so it races
	 * whatever the idle task is doing -- moving the call from the SPI-dispatch ISR
	 * onto this task narrowed that window but could not close it, because the idle
	 * loop is concurrent with EVERY task.  Not calling it at all closes it by
	 * construction.  Power_setConstraint/releaseConstraint are safe to call
	 * anytime and are the mechanism the policy is documented to read. */
	switch (policy) {
	case ALP_CC3501E_PP_PERFORMANCE:
		/* Lowest latency: forbid SLEEP and IDLE, so the policy falls all the way
		 * back to WFI -- any peripheral IRQ (the bridge SPI CS, a GPIO edge)
		 * wakes it immediately. */
		pp_hold_constraint(PowerWFF3_DISALLOW_SLEEP);
		pp_hold_constraint(PowerWFF3_DISALLOW_IDLE);
		break;
	case ALP_CC3501E_PP_BALANCED:
	case ALP_CC3501E_PP_LOW_POWER:
	case ALP_CC3501E_PP_DEEP_SLEEP:
		/* Drop our DISALLOW constraints so the policy can reach the deepest state
		 * its latency budget allows.  All three share the same CORE behaviour --
		 * WFF3 exposes a single SLEEP state -- and differ on the RADIO, where
		 * DEEP_SLEEP takes the N-DTIM long sleep interval (pp_apply_radio()). */
		pp_release_constraint(PowerWFF3_DISALLOW_SLEEP);
		pp_release_constraint(PowerWFF3_DISALLOW_IDLE);
		break;
	default:
		return;
	}
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

	/* Withhold the multi-DTIM sleep while BLE is up -- see pp_long_sleep_safe().
	 * The preset still gets POWER_SAVE_MODE + ELP; only the long interval is
	 * downgraded, because that is what starves the shared HIF. */
	const bool long_sleep = want_lsi && pp_long_sleep_safe();

	if (want_lsi || policy == ALP_CC3501E_PP_LOW_POWER) {
		WlanLongSleepInterval lsi = { 0 };

		lsi.WakeUpEvent = long_sleep ? (uint8_t)WAKE_UP_EVENT_N_DTIM : (uint8_t)WAKE_UP_EVENT_DTIM;
		lsi.ListenInterval = long_sleep ? pp_idle_ms_to_dtims(idle_ms) : 1u;
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

	/* Core FIRST, then radio -- both on this task, never in the ISR (#1683). */
	pp_apply_core(pp_policy_latched);

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
	 * Neither half may run here.  Wlan_Set() is a blocking vendor radio call, and
	 * Power_setPolicy()/Power_enablePolicy() race the idle loop (#1683).  Both are
	 * deferred to cc3501e_hw_power_service(), which the bringup task drains via
	 * cc3501e_hw_tick().  Bench-measured with either inline: every preset returned
	 * -4 and the bridge itself went to PING -> -5.
	 *
	 * CONSEQUENCE FOR THE WIRE CONTRACT: a RESP_OK to POWER_POLICY means QUEUED,
	 * not APPLIED -- the same semantic OTA_BEGIN turned out to have.  A host that
	 * needs the realised state polls it; see cc3501e_hw_power_radio_ok(). */
	pp_radio_dirty = true;

	return CC3501E_HW_OK;
}
