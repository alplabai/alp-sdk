/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stage-A low-power smoke for the E1M-AEN801 (Alif Ensemble E8, Cortex-M55-HE).
 *
 * What it proves
 * --------------
 * The CPU can enter the architectural Cortex-M55 WFI sleep and be woken again
 * by the SysTick system clock, with the kernel surviving the round trip:
 *   1. device boots, the RAM console comes up;
 *   2. a periodic k_timer is the cadence/wake source -- each round the app waits
 *      on the timer's semaphore (the thread blocks -> the idle thread runs WFI),
 *      then takes one extra explicit architectural WFI via k_cpu_idle();
 *   3. between idles a "beacon" / heartbeat advances in global SRAM0 and
 *      k_uptime_get() moves forward -- i.e. we really slept and really woke,
 *      repeatedly;
 * reported with a single 'RESULT PASS:' / 'RESULT FAIL:' line.
 *
 * The wake "beacon" lives in global SRAM0 at 0x02000000 (same scheme as the
 * sibling aen-hp-core-smoke): always-on on-chip SRAM at the same address from
 * every master, so the host can read it over the system AXI-AP -- mem32 of
 * 0x02000000 -- as an AP-agnostic witness alongside the console RESULT line:
 *
 *   SRAM0[0] = 0xA11FE000  magic ("ALIVE")
 *   SRAM0[1] = SCB.CPUID         (0x411FD220 = Cortex-M55)
 *   SRAM0[2] = completed sleep/wake rounds (heartbeat; ADVANCES => core woke)
 *   SRAM0[3] = final RESULT code (1 = PASS, 2 = FAIL; 0 = not finished)
 *
 * Why WFI + a k_timer wake, and NOT CONFIG_PM here
 * ------------------------------------------------
 * Pinned upstream Zephyr 4.4.0's Alif E8 SoC does NOT `select HAS_PM`
 * (zephyr/soc/alif has no HAS_PM selection and ships no power.c / pm_state_set;
 * only the alifsemi/zephyr_alif FORK's soc/alif/ensemble/Kconfig does --
 * `select HAS_PM` at lines 28 and 50).  So CONFIG_PM
 * (zephyr/subsys/pm/Kconfig: `config PM ... depends on SYS_CLOCK_EXISTS &&
 * HAS_PM`, line 23) is UNREACHABLE over the pinned tree, AND upstream provides
 * no Alif pm_state_set() to link against (upstream subsys/pm/pm.c calls
 * pm_state_set() directly with no weak Alif fallback).  Forcing CONFIG_PM would
 * therefore either be silently dropped (HAS_PM unset) or fail to link.
 *
 * Stage-A therefore exercises the genuine WFI-equivalent baseline: a periodic
 * k_timer beats the wake cadence and the kernel's idle path lowers an idle CPU
 * to __WFI() with NO PM subsystem; SysTick wakes it.  This is the exact arch
 * entry that the Stage-B IWIC pm_state_set path wraps.
 *
 * SysTick as the wake source
 * --------------------------
 * CONFIG_CORTEX_M_SYSTICK is `default y` for the M55 (CPU_CORTEX_M55 selects
 * CPU_CORTEX_M_HAS_SYSTICK), and the upstream HE devicetree provides the
 * `arm,armv8.1m-systick` node (timer@e000e010 via arm/armv8.1-m.dtsi).  SysTick
 * is architectural EXCEPTION #15, NOT an NVIC IRQ -- so it is wholly unaffected
 * by the EWIC/IWIC "interrupts 0-63 only" deep-sleep wake range (fork
 * soc/alif/common/rtss/power.c:215-217, 287-291) and always wakes a plain WFI.
 * That is exactly why it is the right Stage-A wake: no peripheral, no IRQ-number
 * constraint, no PM dependency.
 *
 * Stage-B (NOT in this app -- documented for the bench)
 * -----------------------------------------------------
 * The deeper Alif sleeps (suspend-to-idle = deep IWIC sleep, suspend-to-ram,
 * soft-off) are driven by a board/SoC pm_state_set() that programs the WakeUp
 * Interrupt Controller via the AON WICCONTROL register.  For the RTSS-HE core
 * that register is AON_RTSS_HE_CTRL:
 *
 *     AON_BASE          = 0x1A604000          (fork soc_common.h:15)
 *     AON_RTSS_HE_CTRL  = AON_BASE + 0x10
 *                       = 0x1A604010          (fork soc_common.h:18)
 *     WICCONTROL (HE)   = AON_RTSS_HE_CTRL     (fork rtss/power.c:27-28)
 *                       => 0x1A604010
 *  (the RTSS-HP core would use AON_RTSS_HP_CTRL = AON_BASE + 0x0 = 0x1A604000,
 *   fork soc_common.h:16 / power.c:25-26.)
 *
 * with bit8 = WIC enable (WICCONTROL_WIC_Pos, fork power.c:45) and bit9 =
 * IWIC-vs-EWIC select (WICCONTROL_IWIC_Pos, fork power.c:50; IWIC=1, EWIC=0,
 * power.c:40-41).  Bringing that up over the pinned tree is a separate Tier-1.5
 * task: add `select HAS_PM`, the `power-states`/`cpu-power-states` DT nodes
 * (fork ensemble_rtss_he.dtsi:153-197: &suspend_idle &standby_s2ram &stop_s2ram
 * &subsys_off, all status=disabled by default), and the pm_state_set IWIC hook
 * (fork power.c:432).  See this front's report for the full node list.
 *
 * Console is the RAM buffer 'ram_console_buf' (see prj.conf); the bench app
 * UART is not wired to USB.  BENCH-VALIDATION app -- not a customer teaching
 * example.
 */

#include <zephyr/kernel.h>

/* Cortex-M System Control Block CPUID, by absolute address (no CMSIS header). */
#define SCB_CPUID (*(volatile uint32_t *)0xE000ED00U)

/* Global SRAM0 liveness beacon (always-on on-chip SRAM, master-agnostic addr).
 * Read mem32 of 0x02000000 over the system AXI-AP. */
#define SRAM0_BEACON ((volatile uint32_t *)0x02000000U)
#define BEACON_MAGIC 0xA11FE000U /* "ALIVE" */

/* Beacon RESULT codes (SRAM0[3]). */
#define RESULT_NONE 0U
#define RESULT_PASS 1U
#define RESULT_FAIL 2U

/* Number of sleep/wake rounds to take, and the wake-timer period. */
#define IDLE_ROUNDS  8U
#define WAKE_TICK_MS 50U

/*
 * The k_timer is the wake cadence.  Its expiry handler gives a semaphore; main
 * blocks on that semaphore, so the idle thread runs and lowers the CPU into WFI
 * until the next SysTick-driven timer expiry releases main.  This is the
 * portable stand-in for the Stage-B "wake on the configured WIC source": here
 * the SysTick-backed kernel timer is the source.
 */
static K_SEM_DEFINE(wake_sem, 0, 1);

static void wake_timer_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	k_sem_give(&wake_sem);
}

/* Periodic wake source: expiry gives wake_sem (see wake_timer_fn). */
static K_TIMER_DEFINE(wake_timer, wake_timer_fn, NULL);

int main(void)
{
	uint32_t cpuid = SCB_CPUID;
	int64_t  t_start, t_now, t_prev;
	uint32_t advanced = 0U;

	/* Seed the beacon immediately so even a one-shot read after boot proves
	 * main() was reached, before any sleeping. */
	SRAM0_BEACON[0] = BEACON_MAGIC;
	SRAM0_BEACON[1] = cpuid;
	SRAM0_BEACON[2] = 0U;
	SRAM0_BEACON[3] = RESULT_NONE;

	printk("\n=== AEN801 Stage-A low-power smoke "
	       "(k_timer wake -> WFI, SysTick) ===\n");

	t_start = k_uptime_get();
	t_prev  = t_start;
	printk("boot uptime=%lld ms; CPUID=0x%08x; taking %u WFI sleeps "
	       "(~%u ms each)\n",
	       t_start,
	       cpuid,
	       IDLE_ROUNDS,
	       WAKE_TICK_MS);

	/* Drive the wake cadence with a periodic k_timer (the brief's k_timer
	 * wake). The first expiry is one period out; it then repeats. */
	k_timer_start(&wake_timer, K_MSEC(WAKE_TICK_MS), K_MSEC(WAKE_TICK_MS));

	for (uint32_t i = 0U; i < IDLE_ROUNDS; i++) {
		/*
		 * Block on the timer's semaphore: main de-schedules, the kernel
		 * idle thread runs WFI, and the SysTick-backed k_timer expiry
		 * wakes the core and releases us.  Then take one EXPLICIT
		 * architectural WFI via k_cpu_idle() for good measure -- with a
		 * live SysTick it returns on the next tick, proving the core
		 * parks in WFI and the SysTick exception brings it back.  This
		 * explicit k_cpu_idle() is the exact arch entry the Stage-B IWIC
		 * path wraps.
		 */
		k_sem_take(&wake_sem, K_FOREVER);
		k_cpu_idle();

		t_now = k_uptime_get();
		if (t_now > t_prev) {
			advanced++;
		}
		t_prev = t_now;

		/* Heartbeat: completed rounds, in SRAM0 -- advances => woke. */
		SRAM0_BEACON[2] = i + 1U;

		printk("round %u/%u: woke, beacon=%u uptime=%lld ms\n",
		       i + 1U,
		       IDLE_ROUNDS,
		       (uint32_t)SRAM0_BEACON[2],
		       t_now);
	}

	k_timer_stop(&wake_timer);
	t_now = k_uptime_get();

	if (SRAM0_BEACON[2] != IDLE_ROUNDS) {
		SRAM0_BEACON[3] = RESULT_FAIL;
		printk("RESULT FAIL: beacon=%u expected %u (a round did not "
		       "complete)\n",
		       (uint32_t)SRAM0_BEACON[2],
		       IDLE_ROUNDS);
		return 0;
	}
	if (advanced != IDLE_ROUNDS) {
		SRAM0_BEACON[3] = RESULT_FAIL;
		printk("RESULT FAIL: uptime advanced on only %u/%u rounds "
		       "(SysTick not waking?)\n",
		       advanced,
		       IDLE_ROUNDS);
		return 0;
	}

	SRAM0_BEACON[3] = RESULT_PASS;
	printk("RESULT PASS: %u WFI sleeps entered + woken by SysTick; "
	       "uptime %lld->%lld ms (beacon=%u)\n",
	       IDLE_ROUNDS,
	       t_start,
	       t_now,
	       (uint32_t)SRAM0_BEACON[2]);
	return 0;
}
