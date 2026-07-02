/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-dualcore-probe -- the decisive "does a dual-entry ATOC boot BOTH M55s?"
 * test for the E1M-AEN801 (Ensemble E8). ONE app, role-by-board:
 *   - built for the RTSS-HE board -> writes the HE beacon at SRAM0 0x02001010
 *   - built for the RTSS-HP board -> writes the HP beacon at SRAM0 0x02000010
 * Both write to GLOBAL SRAM0 (master-agnostic addresses, readable over SWD from
 * whichever debug AP J-Link attaches to). Each core stamps a magic word then
 * advances a heartbeat forever. After flashing a dual-entry ATOC (HE@0x58000000
 * + HP@0x50000000, both flags [load,boot]) and resetting, read BOTH beacons:
 *   - both heartbeats advance  -> the SES booted BOTH cores from one power-on
 *   - only HP advances, HE = 0 -> single-core boot (the prior finding)
 *
 * 0x02000000 itself reads back 0 even when running (reserved/special per the
 * bench), so the beacons sit at nonzero offsets.
 */

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/* Core-role selection (HP vs HE build of this app), not a peripheral-presence gate. */
#if defined(CONFIG_BOARD_ALP_E1M_AEN801_M55_HP)
#define BEACON_BASE 0x02000010U
#define ROLE        "HP"
#define MAGIC       0xB1B10090U /* HP slot */
#else
#define BEACON_BASE 0x02001010U
#define ROLE        "HE"
#define MAGIC       0xB1B100E0U /* HE slot */
#endif

#define BEACON ((volatile uint32_t *)BEACON_BASE)

int main(void)
{
	printk("\n=== aen-dualcore-probe (%s core) beacon@0x%08x ===\n", ROLE, BEACON_BASE);

	BEACON[0] = MAGIC; /* magic: which core stamped this slot */
	BEACON[1] = 0U;    /* heartbeat: advances => this core is running */

	for (uint32_t hb = 1U;; hb++) {
		BEACON[1] = hb;
		if ((hb & 0x3FFU) == 0U) {
			printk("%s heartbeat %u\n", ROLE, hb);
		}
		for (volatile uint32_t d = 0U; d < 200000U; d++) {
			/* crude delay so the heartbeat is human-observable */
		}
	}

	return 0;
}
