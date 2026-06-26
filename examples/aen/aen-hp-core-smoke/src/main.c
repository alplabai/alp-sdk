/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * First-light smoke for the M55-HP (RTSS-HP) core of the E1M-AEN801 (Alif
 * Ensemble E8).  Every prior bench app ran on the M55-HE core; this is the first
 * alp-sdk image targeted at the SECOND M55.
 *
 * Boot model
 * ----------
 * The HP core is held in reset at power-on (the J-Link AP map shows only the HE
 * core's AHB-AP with a readable CPUID; the HP core's AP reads no CPUID until it
 * is released).  It is released by the Secure Enclave booting an `M55_HP` ATOC:
 * SETOOLS authors the package with cpu_id="M55_HP", loadAddress=0x50000000 (the
 * HP ITCM global base, vs the HE core's 0x58000000), flags ["load","boot"]; the
 * SES loads this image there and starts the HP core.
 *
 * Observability
 * -------------
 * The bench reads memory over SWD via the HE/AXI debug AP, not the HP core's AP,
 * so this app does NOT rely on the HP core's RAM console being reachable.
 * Instead it writes a LIVENESS BEACON to global SRAM0 (0x02000000) -- always-on
 * on-chip SRAM at the same address from every master, so the host reads it over
 * the system AXI-AP regardless of HP core state:
 *
 *   SRAM0[0] = 0xA11FE000  magic  ("ALIVE")
 *   SRAM0[1] = SCB.CPUID          (0x411FD220 = Cortex-M55)
 *   SRAM0[2] = SCB.VTOR           (vector base; 0x50000000 confirms the SES HP load)
 *   SRAM0[3] = heartbeat          (incremented every ~100 ms -> ADVANCING means
 *                                  the HP core is actively executing, not a stale
 *                                  value left by a prior image)
 *
 * The RAM console line is bonus (read it via the AXI-AP at the HP DTCM global
 * address if wired); the beacon is the primary, AP-agnostic proof.
 *
 * BENCH-VALIDATION app -- not a customer teaching example.
 */

#include <zephyr/kernel.h>

/* Cortex-M System Control Block, by absolute address (no CMSIS header needed). */
#define SCB_CPUID (*(volatile uint32_t *)0xE000ED00U)
#define SCB_VTOR  (*(volatile uint32_t *)0xE000ED08U)

/* Global SRAM0 liveness beacon (always-on on-chip SRAM, master-agnostic addr). */
#define SRAM0_BEACON ((volatile uint32_t *)0x02000000U)
#define BEACON_MAGIC 0xA11FE000U /* "ALIVE" */

int main(void)
{
	uint32_t cpuid = SCB_CPUID;
	uint32_t vtor  = SCB_VTOR;
	uint32_t hb    = 0U;

	/* Seed the beacon immediately so even a one-shot read after boot proves
	 * the HP core reached main(). */
	SRAM0_BEACON[0] = BEACON_MAGIC;
	SRAM0_BEACON[1] = cpuid;
	SRAM0_BEACON[2] = vtor;
	SRAM0_BEACON[3] = hb;

	printk("\n=== AEN801 M55-HP core smoke (first light) ===\n");
	printk("HP core alive: CPUID=0x%08x VTOR=0x%08x\n", cpuid, vtor);
	printk("SRAM0 beacon @0x02000000: magic=0x%08x cpuid=0x%08x vtor=0x%08x\n",
	       BEACON_MAGIC,
	       cpuid,
	       vtor);
	printk("RESULT PASS: M55-HP reached main() and seeded the SRAM0 beacon\n");

	/* Advancing heartbeat: a host that reads SRAM0[3] twice sees it move, which
	 * proves the HP core is actively running (not a stale beacon). */
	while (1) {
		SRAM0_BEACON[3] = ++hb;
		printk("HP heartbeat %u\n", hb);
		k_msleep(100);
	}

	return 0;
}
