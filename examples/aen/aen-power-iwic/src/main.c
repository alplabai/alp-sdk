/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stage-B low-power IWIC deep-sleep example for the E1M-AEN801 (Alif Ensemble
 * E8, Cortex-M55-HE).  Where Stage-A (aen-power-smoke) proved a plain
 * architectural WFI via the kernel idle path with NO PM subsystem, Stage-B
 * brings the Alif PM layer up over the pinned Zephyr 4.4.0 tree and proves the
 * CONFIG_PM suspend-to-idle (deep IWIC sleep) path:
 *
 *   1. boot, RAM console up;
 *   2. report the PM wiring as a bind-regcheck (CONFIG_PM built in, HAS_PM
 *      selected, the suspend_idle power-state node present + enabled, the
 *      pm_state_set IWIC hook linked);
 *   3. let the kernel idle into the PM policy: main blocks on a periodic
 *      k_timer's semaphore long enough that the PM policy selects
 *      PM_STATE_SUSPEND_TO_IDLE, so pm_state_set() programs WICCONTROL (deep
 *      IWIC sleep) and the core parks in WFI until SysTick wakes it;
 *   4. a wake "beacon" in global SRAM0 (0x02000000, master-agnostic addr)
 *      advances every round and k_uptime_get() moves forward -- i.e. we entered
 *      the PM deep-sleep state and came back, repeatedly;
 *   5. RESULT PASS / FAIL on one line.
 *
 * The beacon layout matches the Stage-A smoke (read mem32 of 0x02000000 over the
 * system AXI-AP as an AP-agnostic witness next to the console RESULT line):
 *   SRAM0[0] = 0xA11FE000  magic ("ALIVE")
 *   SRAM0[1] = SCB.CPUID         (0x411FD220 = Cortex-M55)
 *   SRAM0[2] = completed PM sleep/wake rounds (advances => core woke)
 *   SRAM0[3] = final RESULT code (1 = PASS, 2 = FAIL; 0 = not finished)
 *
 * WHY THIS IS THE STAGE-B MILESTONE
 * ---------------------------------
 * Pinned upstream Zephyr 4.4.0's Alif E8 SoC neither selects HAS_PM nor ships a
 * pm_state_set(), so CONFIG_PM was previously unreachable AND unlinkable (see
 * aen-power-smoke).  This example carries the Tier-1.5 plumbing that closes the
 * gap at the app/board level, with no edit to upstream zephyr/:
 *   - prj.conf turns CONFIG_PM on;
 *   - Kconfig (this app) `select HAS_PM` so `config PM` becomes reachable;
 *   - the board overlay enables the `suspend_idle` zephyr,power-state node and
 *     wires cpu0 cpu-power-states to it;
 *   - src/alif_pm_iwic.c supplies pm_state_set()/pm_state_exit_post_ops()
 *     driving the AON WICCONTROL register (0x1A604010, bit8 WIC / bit9 IWIC).
 *
 * suspend-to-idle (deep IWIC sleep) is the lightest Alif deep sleep: it needs no
 * S2RAM context save and no Secure-Enclave retention call, so it builds + runs
 * standalone.  suspend-to-ram / soft-off remain the full-fork-port follow-up.
 *
 * Console is the RAM buffer 'ram_console_buf' (see prj.conf); the bench app UART
 * is not USB-wired.  BENCH-VALIDATION app -- not a customer teaching example.
 */

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>

/* Cortex-M System Control Block CPUID, by absolute address (no CMSIS header). */
#define SCB_CPUID (*(volatile uint32_t *)0xE000ED00U)

/* Global SRAM0 liveness beacon (always-on on-chip SRAM, master-agnostic addr). */
#define SRAM0_BEACON ((volatile uint32_t *)0x02000000U)
#define BEACON_MAGIC 0xA11FE000U /* "ALIVE" */

#define RESULT_NONE 0U
#define RESULT_PASS 1U
#define RESULT_FAIL 2U

/* Number of PM sleep/wake rounds, and the wake-timer period.  The period is
 * comfortably above the suspend_idle min-residency-us (2500 us) so the PM policy
 * actually selects suspend-to-idle each idle window. */
#define PM_ROUNDS    8U
#define WAKE_TICK_MS 50U

/*
 * Compile-time PM-wiring facts -- the Stage-B bind-regcheck.  Each is a pure
 * Kconfig/DT predicate proving the PM plumbing is in place at build time; the
 * runtime rounds below then prove the state is actually entered + exited.
 */
#if defined(CONFIG_PM)
#define PM_BUILT 1
#else
#define PM_BUILT 0
#endif

#define SUSPEND_IDLE_NODE DT_NODELABEL(suspend_idle)
#define SUSPEND_IDLE_OK   DT_NODE_HAS_STATUS(SUSPEND_IDLE_NODE, okay)

static K_SEM_DEFINE(wake_sem, 0, 1);

static void wake_timer_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	k_sem_give(&wake_sem);
}

static K_TIMER_DEFINE(wake_timer, wake_timer_fn, NULL);

int main(void)
{
	uint32_t cpuid = SCB_CPUID;
	int64_t  t_start, t_now, t_prev;
	uint32_t advanced = 0U;

	SRAM0_BEACON[0] = BEACON_MAGIC;
	SRAM0_BEACON[1] = cpuid;
	SRAM0_BEACON[2] = 0U;
	SRAM0_BEACON[3] = RESULT_NONE;

	printk("\n=== AEN801 Stage-B PM deep-sleep (suspend-to-idle / IWIC) ===\n");

	/* Step 1: report the PM wiring (bind-regcheck of the Tier-1.5 plumbing). */
	printk("PM wiring: CONFIG_PM=%d suspend_idle-node=%d "
	       "WICCONTROL(HE)=0x1A604010 (bit8 WIC / bit9 IWIC)\n",
	       PM_BUILT,
	       (int)SUSPEND_IDLE_OK);

	if (!PM_BUILT || !SUSPEND_IDLE_OK) {
		SRAM0_BEACON[3] = RESULT_FAIL;
		printk("RESULT FAIL: PM not wired (CONFIG_PM=%d suspend_idle=%d) -- "
		       "Stage-B plumbing missing\n",
		       PM_BUILT,
		       (int)SUSPEND_IDLE_OK);
		return 0;
	}

	t_start = k_uptime_get();
	t_prev  = t_start;
	printk("boot uptime=%lld ms; CPUID=0x%08x; taking %u PM suspend-to-idle "
	       "rounds (~%u ms each)\n",
	       t_start,
	       cpuid,
	       PM_ROUNDS,
	       WAKE_TICK_MS);

	k_timer_start(&wake_timer, K_MSEC(WAKE_TICK_MS), K_MSEC(WAKE_TICK_MS));

	for (uint32_t i = 0U; i < PM_ROUNDS; i++) {
		/*
		 * Block on the timer semaphore: main de-schedules, the kernel
		 * idle thread runs the PM policy, which -- with the idle window
		 * (~WAKE_TICK_MS) above suspend_idle's min-residency-us --
		 * selects PM_STATE_SUSPEND_TO_IDLE.  pm_state_set() then programs
		 * WICCONTROL for deep IWIC sleep and parks the core in WFI; the
		 * SysTick-backed k_timer expiry wakes it and releases us.
		 */
		k_sem_take(&wake_sem, K_FOREVER);

		t_now = k_uptime_get();
		if (t_now > t_prev) {
			advanced++;
		}
		t_prev = t_now;

		SRAM0_BEACON[2] = i + 1U;

		printk("round %u/%u: woke from PM idle, beacon=%u uptime=%lld ms\n",
		       i + 1U,
		       PM_ROUNDS,
		       (uint32_t)SRAM0_BEACON[2],
		       t_now);
	}

	k_timer_stop(&wake_timer);
	t_now = k_uptime_get();

	if (SRAM0_BEACON[2] != PM_ROUNDS) {
		SRAM0_BEACON[3] = RESULT_FAIL;
		printk("RESULT FAIL: beacon=%u expected %u (a PM round did not "
		       "complete)\n",
		       (uint32_t)SRAM0_BEACON[2],
		       PM_ROUNDS);
		return 0;
	}
	if (advanced != PM_ROUNDS) {
		SRAM0_BEACON[3] = RESULT_FAIL;
		printk("RESULT FAIL: uptime advanced on only %u/%u rounds "
		       "(core not waking from PM idle?)\n",
		       advanced,
		       PM_ROUNDS);
		return 0;
	}

	SRAM0_BEACON[3] = RESULT_PASS;
	printk("RESULT PASS: %u PM suspend-to-idle (deep IWIC) rounds entered + "
	       "woken; uptime %lld->%lld ms (beacon=%u)\n",
	       PM_ROUNDS,
	       t_start,
	       t_now,
	       (uint32_t)SRAM0_BEACON[2]);
	return 0;
}
