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
 */

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
#define ROLE        "HP"
#define SELF_BEACON ((volatile uint32_t *)0x02000010U)
#define DB_BEACON   ((volatile uint32_t *)0x02000020U)
#define SELF_MAGIC  0xB1B10090U
#else
#define ROLE        "HE"
#define SELF_BEACON ((volatile uint32_t *)0x02001010U)
#define DB_BEACON   ((volatile uint32_t *)0x02001020U)
#define SELF_MAGIC  0xB1B100E0U
#endif

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

	uint32_t received = 0U;

	for (uint32_t hb = 1U;; hb++) {
		SELF_BEACON[1] = hb;
		if (sys_read32(RCV_CH0_ST) & DOORBELL_BIT) {
			received++;
			DB_BEACON[0] = received;
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
	while (sys_read32(SND_ACC_RDY) == 0U) {
		/* wait for the link to come ready */
	}
	printk("MHU-1 sender ready -- ringing HP @0x400B0000\n");

	uint32_t sent = 0U;

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
