/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-dualcore-he-master -- the HE-master -> HP-peer direction of B1 Option B
 * (portable alp_mproc_boot_core(), <alp/mproc.h>), as its OWN example instead
 * of a hand-paired combination of two others.
 *
 * Every one of the seven other AEN dual-core examples (aen-dualcore-master,
 * aen-dualcore-probe, aen-dualcore-doorbell, aen-dualcore-ipc, ...) releases
 * its peer HP-master -> HE-peer. That direction works with the plain
 * se_service_boot_cpu() release path (service 501). This example is the
 * OTHER direction -- HE releases HP -- which 501 cannot do at all: Alif's SE
 * Host Services API docs (SE_Host_Services_API_v1.109.0.pdf) say plainly
 * that 501 "does not perform image loading, verification, etc., it just
 * boots the core" (p.112), and that resetting the M55-HP core specifically
 * invalidates its TCM content on release -- p.113 scopes this to "FUSION
 * REV_Bx devices", p.115 instead says "Ensemble devices" with no qualifier
 * (E8 is Ensemble, so p.115 covers it either way; the two vendor passages
 * disagree with each other on scope and are cited here rather than
 * resolved). Bench-measured on E8 (2026-07-31, HE master releasing an HP
 * peer via 501): the SES table reported HP Loaded/Verified, but its ITCM
 * read as uninitialized SRAM, and releasing it vectored the core from empty
 * memory -- CFSR=0x00000101 (IACCVIOL+IBUSERR), PC=0xEFFFFFFE.
 *
 * The fix is service 500 (se_service_process_toc_entry(), "a convenient way
 * to boot a CPU core", p.112) against a peer ATOC entry flagged
 * ["load","boot","deferred"]: the SES skips the entry entirely at power-on
 * (SETOOLS guide AUGD0005 p.35: "skipped at boot time, i.e., no boot OR
 * load"), and this master's boot_core() call un-defers it at runtime -- load,
 * verify AND release together, strictly AFTER whatever reset the release
 * involves, matching p.115's own documented remedy (reset -> reload ->
 * release). This build selects that path via
 * CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC_PEER_IS_HP=y
 * (boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.conf), which
 * cascades CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC=y and
 * CONFIG_..._ENTRY_ID="ALP-HP" from their own Kconfig defaults -- see
 * zephyr/kconfigs/mproc-rpc-usb.kconfig. Silicon-proven on E8, 2026-08-01,
 * as this exact self-contained example (a from-scratch bench rebuild
 * reproduced the authoring build's zephyr.bin md5s bit-exactly): see
 * README.md.
 *
 * ONE app, role-by-board, same pattern as every sibling:
 *   - built for the RTSS-HE board -> MASTER: SES-booted normally
 *     (["load","boot"] @0x58000000), calls alp_mproc_boot_core() to release
 *     the deferred HP peer.
 *   - built for the RTSS-HP board -> PEER: SES-DEFERRED (["load","boot",
 *     "deferred"] @0x50000000), never boots itself and never calls a boot
 *     API -- it only comes up when the master's runtime request un-defers
 *     it, then stamps a global-SRAM0 beacon like every other peer role in
 *     this family.
 *
 * alp_mproc_boot_core() returning ALP_OK only confirms the SE boot authority
 * ACCEPTED the un-defer request -- it says nothing about whether the peer
 * actually came up (that is the entire lesson this example exists to teach:
 * the plain-501 direction above is ALSO accepted with rc=ALP_OK and STILL
 * fails on real silicon). So this master polls the peer's own beacon for a
 * bounded window and only claims PASS once that word is observed to move --
 * never on the accepted return code alone.
 *
 * Message discipline: every non-PASS verdict below states what was
 * OBSERVED (the peer's beacon word did not advance within the bound), never
 * an inferred cause ("peer never ran") -- CONFIG_DCACHE=n exists specifically
 * because an inferred "peer never ran" was WRONG on this bench: the peer was
 * running and its beacon was genuinely advancing, just invisible to a cached
 * cross-core read (see prj.conf).
 *
 * Build (see README.md for the ATOC + flags this pairs with):
 *   west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
 *     examples/aen/aen-dualcore-he-master -d build/he -- \
 *     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
 *   west build -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp \
 *     examples/aen/aen-dualcore-he-master -d build/hp -- \
 *     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
 */

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/* <alp/mproc.h> is included further down, MASTER-role only: the PEER role
 * never calls into the SDK (it doesn't even build with CONFIG_ALP_SDK=y --
 * see prj.conf / boards/*.conf), so its include path doesn't carry this
 * header at all. Keeping the include role-scoped, not just the call, keeps
 * the peer build exactly as lean as aen-dualcore-probe's. */

/* Global-SRAM0 beacon addresses + magics -- the same scheme (and the same
 * literal addresses) every aen-dualcore-* example uses, so a bench operator
 * reading this app over SWD needs no new mental model. 0x02000000 itself
 * reads back 0 even when running (reserved/special per the bench), so both
 * beacons sit at nonzero offsets. */
#define HP_BEACON ((volatile uint32_t *)0x02000010U) /* HP's own beacon slot */
#define HP_MAGIC  0xB1B10090U
#define HE_BEACON ((volatile uint32_t *)0x02001010U) /* HE's own beacon slot */
#define HE_BOOT   ((volatile uint32_t *)0x02001018U) /* HE-only: boot_core() rc */
#define HE_MAGIC  0xB1B100E0U
#define HP_ENTRY  0x50000000U /* HP ITCM global alias = the deferred ATOC entry's loadAddress */

/* Bounded wait for the peer's heartbeat to move at least once. Generous vs.
 * a cold M55 boot-to-main(): if the peer never advances within this window
 * on a bench where boot_core reported success, that IS the observation --
 * not proof of what the peer is doing, just that this core never saw it
 * move. */
#define PEER_HB_TIMEOUT_MS 2000U
#define PEER_HB_POLL_MS    20U

#if defined(CONFIG_BOARD_ALP_E1M_AEN801_M55_HP)

/* PEER role: passive. This core's ATOC entry is ["load","boot","deferred"],
 * so it never runs until the HE master's boot_core() call un-defers it --
 * reaching main() at all is itself part of the proof the deferred-TOC
 * release worked. From here it behaves exactly like aen-dualcore-probe:
 * stamp a beacon, then heartbeat forever for the master (or a bench SWD
 * read) to observe.
 */
int main(void)
{
	printk("\n=== aen-dualcore-he-master (HP peer) beacon@0x02000010 ===\n");

	HP_BEACON[0] = HP_MAGIC;
	HP_BEACON[1] = 0U;

	for (uint32_t hb = 1U;; hb++) {
		HP_BEACON[1] = hb;
		if ((hb & 0x3FFU) == 0U) {
			printk("HP heartbeat %u\n", hb);
		}
		for (volatile uint32_t d = 0U; d < 200000U; d++) {
			/* crude delay so the heartbeat is human-observable */
		}
	}

	return 0;
}

#else

#include <alp/mproc.h>

/* MASTER role (default: any board that isn't the HP peer, i.e. RTSS-HE --
 * core-role selection, not a peripheral-presence gate, same convention as
 * every sibling example). */
int main(void)
{
	printk("\n=== aen-dualcore-he-master (HE master) -- release the deferred HP peer "
	       "via alp_mproc_boot_core ===\n");

	HE_BEACON[0] = HE_MAGIC;
	HE_BEACON[1] = 0U;

	alp_status_t rc = alp_mproc_boot_core(ALP_CORE_M55_HP, HP_ENTRY);

	HE_BOOT[0] = 0xB007C0DEU;
	HE_BOOT[1] = (uint32_t)rc;
	printk("alp_mproc_boot_core(M55-HP, 0x%08x) rc=%d\n", HP_ENTRY, (int)rc);

	if (rc != ALP_OK) {
		/* A build-config bug (this build's PEER_IS_HP not matching the
		 * flashed ATOC's deferred entry) or a real backend regression --
		 * either way boot_core() itself refused the request, which is a
		 * local fact this app can state outright. */
		printk("RESULT FAIL: alp_mproc_boot_core rc=%d\n", (int)rc);
	} else {
		/* Accepted is not proof: the plain-501 direction this example
		 * exists to correct ALSO returns ALP_OK and then the peer never
		 * comes up. The only evidence that counts is the peer's own
		 * beacon moving. */
		uint32_t peer_hb0   = HP_BEACON[1];
		bool     peer_alive = false;

		for (uint32_t t = 0U; t < PEER_HB_TIMEOUT_MS / PEER_HB_POLL_MS; t++) {
			k_msleep(PEER_HB_POLL_MS);
			if (HP_BEACON[0] == HP_MAGIC && HP_BEACON[1] != peer_hb0) {
				peer_alive = true;
				break;
			}
		}

		if (peer_alive) {
			printk("RESULT PASS: dualcore-he-master -- M55-HP booted and heartbeating "
			       "(peer beacon magic=0x%08x)\n",
			       HP_MAGIC);
		} else {
			/* Observation only: the beacon word did not move within the
			 * bound. Do NOT claim "the peer never ran" -- CONFIG_DCACHE=n
			 * in prj.conf exists precisely because that inferred claim was
			 * measured wrong on this bench (see prj.conf's comment). */
			printk("RESULT SKIP: dualcore-he-master -- alp_mproc_boot_core(M55-HP) "
			       "accepted (rc=0) but its beacon (0x02000010) did not advance "
			       "within %u ms\n",
			       PEER_HB_TIMEOUT_MS);
		}
	}

	for (uint32_t hb = 1U;; hb++) {
		HE_BEACON[1] = hb;
		for (volatile uint32_t d = 0U; d < 200000U; d++) {
		}
	}
	return 0;
}

#endif
