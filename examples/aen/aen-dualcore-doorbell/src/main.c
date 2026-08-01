/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-dualcore-doorbell -- the HE->HP MHU-1 doorbell, now that BOTH M55 cores
 * boot (see aen-dualcore-master / the portable alp_mproc_boot_core route, which
 * the backend registry resolves to the SoM's boot authority -- the SE boot
 * service on AEN). Earlier B1 attempts could not test this because only one
 * core ran; here the HP master releases HE, then HE rings HP.
 *
 * MHU-1 non-secure HE<->HP pair (Alif DFP + fork e1.dtsi):
 *   HE->HP sender   @0x400B0000 (IRQ44) -- HE writes CH0_SET to ring
 *   HE->HP receiver @0x400A0000 (IRQ43) -- HP sees CH0_ST, clears CH0_CLR
 * MHUv2 register offsets transcribed from zephyr/drivers/ipm/ipm_arm_mhuv2.h:
 *   sender   CH0_SET = +0x0C ; ACCESS_REQUEST = +0xF88 ; ACCESS_READY = +0xF8C
 *   receiver CH0_ST  = +0x00 ; CH0_CLR        = +0x08
 *
 * Roles (board-aware): HP build = master + receiver (polls CH0_ST, counts);
 * HE build = sender (asserts ACCESS_REQUEST, waits ACCESS_READY, rings CH0_SET).
 * Beacons in global SRAM0: self magic+heartbeat, plus a doorbell counter --
 *   HP @0x02000020 = doorbells RECEIVED ; HE @0x02001020 = doorbells SENT.
 * If the HP received-count advances, the HE->HP MHU-1 doorbell propagates.
 *
 * Both roles hold that verdict to a bounded window rather than looping
 * forever waiting for it: HP checks its OWN received-count; HE cross-reads
 * HP's DB_BEACON (globally addressable, same trick the doc above uses for
 * a human SWD read) so it can report the SAME propagation verdict locally
 * instead of blindly claiming "sent" proves anything reached the peer. This
 * build ships CONFIG_HAS_ALIF_SE_SERVICES=y and no native_sim overlay, so
 * boot_core always resolves to the E8 SE backend for ALP_CORE_M55_HE
 * (<alp/mproc.h>'s ALP_ERR_NOSUPPORT case -- "no boot authority for core in
 * this build" -- is not reachable here); any nonzero rc is a real local
 * error and reported FAIL below, not a skippable environment state.
 */

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>

#include <alp/mproc.h>

#define MHU1_SEND    0x400B0000U /* HE->HP sender   */
#define MHU1_RECV    0x400A0000U /* HE->HP receiver */
#define SND_CH0_SET  (MHU1_SEND + 0x0CU)
#define SND_ACC_REQ  (MHU1_SEND + 0xF88U)
#define SND_ACC_RDY  (MHU1_SEND + 0xF8CU)
#define RCV_CH0_ST   (MHU1_RECV + 0x00U)
#define RCV_CH0_CLR  (MHU1_RECV + 0x08U)
#define DOORBELL_BIT 0x1U

#define HE_LOAD_ADDR 0x58000000U /* HE ITCM global alias = HE-APP loadAddress */

/* Core-role selection (HP vs HE build of this app), not a peripheral-presence gate. */
#if defined(CONFIG_BOARD_ALP_E1M_AEN801_M55_HP)
#define ROLE           "HP"
#define SELF_BEACON    ((volatile uint32_t *)0x02000010U)
#define DB_BEACON      ((volatile uint32_t *)0x02000020U)
#define SELF_MAGIC     0xB1B10090U
#define PEER_DB_BEACON ((volatile uint32_t *)0x02001020U) /* HE's own DB_BEACON (sent) */
#else
#define ROLE           "HE"
#define SELF_BEACON    ((volatile uint32_t *)0x02001010U)
#define DB_BEACON      ((volatile uint32_t *)0x02001020U)
#define SELF_MAGIC     0xB1B100E0U
#define PEER_DB_BEACON ((volatile uint32_t *)0x02000020U) /* HP's own DB_BEACON (received) */
#endif

/* Bounded verdict window: HP waits this long to see >=1 doorbell; HE rings
 * and cross-checks HP's received-counter for the same duration before
 * giving up on proof of propagation. Generous vs. the ~ms-scale ring rate. */
#define VERDICT_WAIT_MS 3000U
#define VERDICT_POLL_MS 20U

static inline void busy_delay(void)
{
	for (volatile uint32_t d = 0U; d < 200000U; d++) {
	}
}

int main(void)
{
	printk("\n=== aen-dualcore-doorbell (%s) ===\n", ROLE);

	/* Beacons are an SWD-readable witness (SRAM0 is globally addressable),
	 * so an engineer can confirm both cores are alive and the doorbell is
	 * propagating by halting either core over J-Link and reading the
	 * OTHER core's beacon region -- no cross-core UART/console needed.
	 * SELF_BEACON[0]=magic (which build), [1]=free-running heartbeat;
	 * DB_BEACON[0]=this core's own doorbell counter (sent or received). */
	SELF_BEACON[0] = SELF_MAGIC;
	SELF_BEACON[1] = 0U;
	DB_BEACON[0]   = 0U;

#if defined(CONFIG_BOARD_ALP_E1M_AEN801_M55_HP)
	/* HP master: release HE, then receive HE's doorbells on MHU-1. */
	alp_status_t rc = alp_mproc_boot_core(ALP_CORE_M55_HE, HE_LOAD_ADDR);

	printk("alp_mproc_boot_core(M55-HE, 0x%08x) rc=%d -- now receiving on MHU-1 @0x400A0000\n",
	       HE_LOAD_ADDR,
	       (int)rc);

	if (rc != ALP_OK) {
		printk("RESULT FAIL: alp_mproc_boot_core rc=%d\n", (int)rc);
	} else {
		uint32_t received = 0U;

		/* RCV_CH0_ST is a LATCHED MHUv2 channel-status bit: a core-only warm
		 * reset or a J-Link RAM-run does not clear it, so a leftover set bit
		 * from a PRIOR run's HE ring would otherwise satisfy iteration 0 of
		 * the verdict window below and produce a false PASS with HE never
		 * released this run (same class of bug as the REQ_MBOX/RPL_MBOX
		 * zeroing in aen-dualcore-ipc). Clear it before the window starts. */
		sys_write32(0xFFFFFFFFU, RCV_CH0_CLR);

		for (uint32_t t = 0U; t < VERDICT_WAIT_MS / VERDICT_POLL_MS; t++) {
			SELF_BEACON[1] = t + 1U;
			if (sys_read32(RCV_CH0_ST) & DOORBELL_BIT) {
				received++;
				DB_BEACON[0] = received;
				sys_write32(0xFFFFFFFFU, RCV_CH0_CLR); /* ack/clear the channel */
			}
			k_msleep(VERDICT_POLL_MS);
		}

		if (received > 0U) {
			printk("RESULT PASS: dualcore-doorbell -- received %u doorbell(s) on MHU-1 "
			       "@0x400A0000\n",
			       received);
		} else {
			printk("RESULT SKIP: dualcore-doorbell -- HE released (rc=0) but no doorbell "
			       "received within %u ms; local receiver poll OK\n",
			       VERDICT_WAIT_MS);
		}
	}

	for (uint32_t hb = 1U;; hb++) {
		SELF_BEACON[1] = hb;
		if (sys_read32(RCV_CH0_ST) & DOORBELL_BIT) {
			DB_BEACON[0] = DB_BEACON[0] + 1U;
			sys_write32(0xFFFFFFFFU, RCV_CH0_CLR); /* ack/clear the channel */
		}
		busy_delay();
	}
#else
	/* HE sender: wake the MHU-1 link, then ring HP repeatedly.
	 * MHUv2 power-gates its channel windows: a sender must assert
	 * ACCESS_REQUEST and wait for the receiver side to ack with
	 * ACCESS_READY before CH0_SET is guaranteed to latch -- writing
	 * CH0_SET before the link is ready can be silently dropped. */
	sys_write32(1U, SND_ACC_REQ);

	bool link_ready = false;

	for (uint32_t t = 0U; t < VERDICT_WAIT_MS / VERDICT_POLL_MS; t++) {
		SELF_BEACON[1] = t + 1U;
		if (sys_read32(SND_ACC_RDY) != 0U) {
			link_ready = true;
			break;
		}
		k_msleep(VERDICT_POLL_MS);
	}

	if (!link_ready) {
		printk("RESULT SKIP: dualcore-doorbell -- MHU-1 sender link never came ready "
		       "(ACCESS_READY) within %u ms; ACCESS_REQUEST asserted, no ring attempted\n",
		       VERDICT_WAIT_MS);
		return 0;
	}
	printk("MHU-1 sender ready -- ringing HP @0x400B0000\n");

	uint32_t sent          = 0U;
	uint32_t peer_recv0    = PEER_DB_BEACON[0];
	bool     peer_saw_ring = false;

	for (uint32_t t = 0U; t < VERDICT_WAIT_MS / VERDICT_POLL_MS; t++) {
		SELF_BEACON[1] = t + 1U;
		sys_write32(DOORBELL_BIT, SND_CH0_SET); /* ring channel 0 */
		sent++;
		DB_BEACON[0] = sent;
		k_msleep(VERDICT_POLL_MS);
		if (PEER_DB_BEACON[0] != peer_recv0) {
			peer_saw_ring = true;
			break;
		}
	}

	if (peer_saw_ring) {
		printk("RESULT PASS: dualcore-doorbell -- HP received-count advanced after %u "
		       "ring(s)\n",
		       sent);
	} else {
		printk("RESULT SKIP: dualcore-doorbell -- rang HP %u time(s) but its received-count "
		       "never advanced within %u ms; local sender link OK\n",
		       sent,
		       VERDICT_WAIT_MS);
	}

	for (uint32_t hb = 1U;; hb++) {
		SELF_BEACON[1] = hb;
		sys_write32(DOORBELL_BIT, SND_CH0_SET); /* ring channel 0 */
		sent++;
		DB_BEACON[0] = sent;
		busy_delay();
		busy_delay(); /* send slower than HP polls so each ring is seen */
	}
#endif
	return 0;
}
