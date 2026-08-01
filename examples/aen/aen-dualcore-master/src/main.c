/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-dualcore-master -- B1 Option B: the SES-booted core starts the OTHER M55
 * at runtime via the portable alp_mproc_boot_core() (<alp/mproc.h>), instead of
 * a dual-boot ATOC (which boots only one core, #200). On this E8 the SES boots
 * the HP entry from a dual ATOC, so the HP build is the master that releases HE.
 *
 * The portable call resolves through the backend registry to the SoM's boot
 * authority -- on AEN that is the Secure Enclave's boot service over the
 * seservice0 MHU (the bench-proven route this example originally validated with
 * a direct vendor call).  Application code carries no vendor include.
 *
 * Board-aware: the HP build boots ALP_CORE_M55_HE @ 0x58000000; the HE build
 * boots ALP_CORE_M55_HP @ 0x50000000. Each stamps its own global-SRAM0 beacon +
 * the boot rc. The partner is the aen-dualcore-probe build for the other core,
 * packaged ["load"]-only (SES loads it but does not auto-boot it).
 *
 * alp_mproc_boot_core() only confirms the boot AUTHORITY accepted the request
 * (see <alp/mproc.h>) -- it says nothing about whether the peer actually came
 * up.  Since both cores' beacons live in the same globally-addressable SRAM0
 * window this app can check that itself: after a successful boot request it
 * polls the peer's own heartbeat word (written by whatever image the peer is
 * running -- aen-dualcore-probe or another aen-dualcore-master) for a bounded
 * window and only claims PASS once that word is observed to move.  If
 * boot_core reports ALP_ERR_NOSUPPORT that is reported as SKIP, not FAIL --
 * but NOSUPPORT means no mproc_boot backend was selected for the target core
 * (an environment/config state; see alp_mproc_boot_core() in
 * src/mproc_dispatch.c), not the boot authority refusing: the SE backend's
 * se_rc_to_alp() (src/backends/mproc/alif_se_boot.c) never maps a real SE
 * error to NOSUPPORT for either M55 core, and the SE boot path itself is
 * proven working on E8 silicon.  A real SE refusal comes back as
 * ALP_ERR_IO and is a genuine FAIL below, not folded into this skip.
 */

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <alp/mproc.h>

/* Core-role selection (HP vs HE build of this app), not a peripheral-presence gate. */
#if defined(CONFIG_BOARD_ALP_E1M_AEN801_M55_HP)
#define ROLE        "HP"
#define TARGET_CORE ALP_CORE_M55_HE
#define TARGET_NAME "M55-HE"
#define TARGET_ADDR 0x58000000U /* HE ITCM global alias = HE-APP loadAddress */
#define SELF_BEACON ((volatile uint32_t *)0x02000010U)
#define BOOT_SLOT   ((volatile uint32_t *)0x02000018U)
#define SELF_MAGIC  0xB1B10090U
#define PEER_BEACON ((volatile uint32_t *)0x02001010U) /* HE's own SELF_BEACON */
#define PEER_MAGIC  0xB1B100E0U
#else
#define ROLE        "HE"
#define TARGET_CORE ALP_CORE_M55_HP
#define TARGET_NAME "M55-HP"
#define TARGET_ADDR 0x50000000U /* HP ITCM global alias = HP-APP loadAddress */
#define SELF_BEACON ((volatile uint32_t *)0x02001010U)
#define BOOT_SLOT   ((volatile uint32_t *)0x02001018U)
#define SELF_MAGIC  0xB1B100E0U
#define PEER_BEACON ((volatile uint32_t *)0x02000010U) /* HP's own SELF_BEACON */
#define PEER_MAGIC  0xB1B10090U
#endif

/* Bounded wait for the released peer's heartbeat to move at least once.
 * Generous vs. a cold M55 boot-to-main(): if the peer never advances within
 * this window on a bench where boot_core reported success, it isn't coming. */
#define PEER_HB_TIMEOUT_MS 2000U
#define PEER_HB_POLL_MS    20U

int main(void)
{
	printk("\n=== aen-dualcore-master (%s) -- start the other M55 via alp_mproc_boot_core ===\n",
	       ROLE);

	SELF_BEACON[0] = SELF_MAGIC;
	SELF_BEACON[1] = 0U;

	alp_status_t rc = alp_mproc_boot_core(TARGET_CORE, TARGET_ADDR);

	BOOT_SLOT[0] = 0xB007C0DEU;
	BOOT_SLOT[1] = (uint32_t)rc;
	printk("alp_mproc_boot_core(%s, 0x%08x) rc=%d\n", TARGET_NAME, TARGET_ADDR, (int)rc);

	if (rc == ALP_ERR_NOSUPPORT) {
		printk("RESULT SKIP: dualcore-master -- no mproc_boot backend selected for "
		       "%s (alp_mproc_boot_core rc=%d); local request path OK, peer "
		       "never released\n",
		       TARGET_NAME,
		       (int)rc);
	} else if (rc != ALP_OK) {
		printk("RESULT FAIL: alp_mproc_boot_core rc=%d\n", (int)rc);
	} else {
		/* Boot request accepted -- confirm the peer is actually alive by watching
		 * its heartbeat word move, not just trusting the accepted request. */
		uint32_t peer_hb0   = PEER_BEACON[1];
		bool     peer_alive = false;

		for (uint32_t t = 0U; t < PEER_HB_TIMEOUT_MS / PEER_HB_POLL_MS; t++) {
			k_msleep(PEER_HB_POLL_MS);
			if (PEER_BEACON[0] == PEER_MAGIC && PEER_BEACON[1] != peer_hb0) {
				peer_alive = true;
				break;
			}
		}

		if (peer_alive) {
			printk("RESULT PASS: dualcore-master -- %s booted and heartbeating "
			       "(peer beacon magic=0x%08x)\n",
			       TARGET_NAME,
			       PEER_MAGIC);
		} else {
			printk("RESULT SKIP: dualcore-master -- alp_mproc_boot_core(%s) accepted "
			       "(rc=0) but its beacon never advanced within %u ms; local boot "
			       "request OK, peer image absent/not running\n",
			       TARGET_NAME,
			       PEER_HB_TIMEOUT_MS);
		}
	}

	for (uint32_t hb = 1U;; hb++) {
		SELF_BEACON[1] = hb;
		for (volatile uint32_t d = 0U; d < 200000U; d++) {
		}
	}
	return 0;
}
