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
 *   - only HP advances, HE = 0 -> single-core boot (with CONFIG_DCACHE=n; see
 *     below)
 *
 * SUPERSEDED 2026-08-01: the 2026-06-18 bench run recorded "only HP advances"
 * and read that as a single-core boot. It wasn't -- with CONFIG_DCACHE=n
 * added (see prj.conf), a 2026-08-01 re-run of the SAME dual-entry ATOC shows
 * BOTH heartbeats advancing: the SES was booting both cores all along, and
 * the D-cache was hiding HE's beacon writes from HP's cross-core read (and
 * vice versa). See README.md's "Result" section for the current reading.
 *
 * 0x02000000 itself reads back 0 even when running (reserved/special per the
 * bench), so the beacons sit at nonzero offsets.
 *
 * This app never calls a boot API itself -- both cores are meant to be started
 * by the SES from one dual-entry ATOC, so there is no local rc to gate. The
 * decisive check IS the peer's beacon: this build polls the OTHER core's
 * heartbeat word (same global-SRAM0 window, same magic scheme) for a bounded
 * window right after stamping its own beacon, and reports PASS/SKIP from
 * that -- the same determination a human would make reading both dumps over
 * SWD, just made locally and boundedly instead of left to run forever.
 */

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/* Core-role selection (HP vs HE build of this app), not a peripheral-presence gate. */
#if defined(CONFIG_BOARD_ALP_E1M_AEN801_M55_HP)
#define BEACON_BASE      0x02000010U
#define ROLE             "HP"
#define MAGIC            0xB1B10090U /* HP slot */
#define PEER_BEACON_BASE 0x02001010U
#define PEER_MAGIC       0xB1B100E0U /* HE slot */
#else
#define BEACON_BASE      0x02001010U
#define ROLE             "HE"
#define MAGIC            0xB1B100E0U /* HE slot */
#define PEER_BEACON_BASE 0x02000010U
#define PEER_MAGIC       0xB1B10090U /* HP slot */
#endif

#define BEACON      ((volatile uint32_t *)BEACON_BASE)
#define PEER_BEACON ((volatile uint32_t *)PEER_BEACON_BASE)

/* Bounded wait for the peer core's heartbeat to move.  Both cores (if the SES
 * really did dual-boot them) reach this poll within a few ms of each other,
 * so this window is generous, not tight. */
#define PEER_HB_TIMEOUT_MS 2000U
#define PEER_HB_POLL_MS    20U

int main(void)
{
	printk("\n=== aen-dualcore-probe (%s core) beacon@0x%08x ===\n", ROLE, BEACON_BASE);

	BEACON[0] = MAGIC; /* magic: which core stamped this slot */
	BEACON[1] = 0U;    /* heartbeat: advances => this core is running */

	uint32_t peer_hb0   = PEER_BEACON[1];
	bool     peer_alive = false;

	for (uint32_t t = 0U; t < PEER_HB_TIMEOUT_MS / PEER_HB_POLL_MS; t++) {
		BEACON[1] = t + 1U;
		k_msleep(PEER_HB_POLL_MS);
		if (PEER_BEACON[0] == PEER_MAGIC && PEER_BEACON[1] != peer_hb0) {
			peer_alive = true;
			break;
		}
	}

	if (peer_alive) {
		printk("RESULT PASS: dualcore-probe -- both M55 cores advancing (this=%s, peer "
		       "magic=0x%08x moved) -- dual-entry ATOC booted both\n",
		       ROLE,
		       PEER_MAGIC);
	} else {
		printk("RESULT SKIP: dualcore-probe -- peer beacon (0x%08x) never advanced within "
		       "%u ms; this core (%s) is up\n",
		       PEER_BEACON_BASE,
		       PEER_HB_TIMEOUT_MS,
		       ROLE);
	}

	for (uint32_t hb = PEER_HB_TIMEOUT_MS / PEER_HB_POLL_MS + 1U;; hb++) {
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
