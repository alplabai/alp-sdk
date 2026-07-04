/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-dualcore-ipc -- bidirectional HE<->HP shared-memory request/response on
 * top of the bench-proven non-secure MHU-1 doorbell (aen-dualcore-doorbell).
 *
 * The requester (HE) writes a request {seq, len, payload} into a SHARED global-
 * SRAM0 mailbox, rings HP, then waits. The responder (HP) reads the request,
 * computes a reply (payload+1), writes it into a separate reply mailbox, rings
 * HE back, and HE verifies reply.seq == request.seq and reply.payload ==
 * request.payload + 1. Completed round-trips are counted in a beacon.
 *
 * MHU-1 is the non-secure HE<->HP pair. Critically -- and this is the whole
 * basis for the REVERSE (HP->HE) ring -- the MHU base addresses are CPU-RELATIVE
 * ALIASES: the SAME numeric address routes to the OPPOSITE physical endpoint
 * depending on which core issues the access. Transcribed from the Alif DFP for
 * AE822FA0E5597 (PROPRIETARY, read-only, clean-room):
 *
 *   from the M55-HE core (Device/soc/AE822FA0E5597/include/rtss_he/soc.h):
 *     MHU_M55HE_M55HP_1_TX_BASE = 0x400B0000  -- "HE sends to HP"   (my TX)
 *     MHU_M55HP_M55HE_1_RX_BASE = 0x400A0000  -- "HE recvs from HP" (my RX)
 *   from the M55-HP core (Device/soc/AE822FA0E5597/include/rtss_hp/soc.h):
 *     MHU_M55HP_M55HE_1_TX_BASE = 0x400B0000  -- "HP sends to HE"   (my TX)
 *     MHU_M55HE_M55HP_1_RX_BASE = 0x400A0000  -- "HP recvs from HE" (my RX)
 *
 * i.e. from EITHER core, 0x400B0000 is "my sender frame to the other core" and
 * 0x400A0000 is "my receiver frame from the other core". The bus fabric cross-
 * routes each core's TX frame into the other core's RX frame, so one non-secure
 * MHU-1 pair carries BOTH directions -- no second physical block, no secure
 * MHU-0 / SESS setup. (Per-core IRQs 43=RX, 44=TX, soc.h:136-137; we poll, so
 * IRQs are unused here.) The HE->HP half of exactly this pair is bench-verified
 * in aen-dualcore-doorbell (every ring received).
 *
 * MHUv2 register offsets transcribed from zephyr/drivers/ipm/ipm_arm_mhuv2.h
 * (which matches the DFP MHU_*_TX_Type / MHU_*_RX_Type channel structs):
 *   sender   frame: CH0_SET = +0x0C ; ACCESS_REQUEST = +0xF88 ; ACCESS_READY = +0xF8C
 *   receiver frame: CH0_ST  = +0x00 ; CH0_CLR        = +0x08
 *
 * Roles (board-aware single app):
 *   HP build = master: alp_mproc_boot_core(ALP_CORE_M55_HE, 0x58000000) FIRST
 *              (the portable peer-core release from <alp/mproc.h>; the backend
 *              registry routes it to the SoM's boot authority -- the SE boot
 *              service on AEN), then the responder loop (recv request, reply
 *              payload+1, ring HE).
 *   HE build = requester: build a request, ring HP, wait for the reply, verify.
 *
 * Shared mailbox layout in global SRAM0 (fixed offsets, clear of the beacons):
 *   request @0x02002000  : struct ipc_msg, written by HE, read by HP
 *   reply   @0x02002040  : struct ipc_msg, written by HP, read by HE
 * Both cores agree on this layout; SRAM0 is coherent across both M55s in the
 * global view (the same window the doorbell beacons use).
 *
 * PASS: the HE round-trip counter (beacon @0x02002080) advances to N and the
 * mismatch counter (@0x02002084) stays 0 -- every reply == request.payload + 1.
 */

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/barrier.h>

#include <alp/mproc.h>

/* ----- non-secure MHU-1 frames (CPU-relative aliases; see file header) ----- */
#define MHU1_TX_FRAME 0x400B0000U /* "my sender frame to the other core"     */
#define MHU1_RX_FRAME 0x400A0000U /* "my receiver frame from the other core" */
#define TX_CH0_SET    (MHU1_TX_FRAME + 0x0CU)
#define TX_ACC_REQ    (MHU1_TX_FRAME + 0xF88U)
#define TX_ACC_RDY    (MHU1_TX_FRAME + 0xF8CU)
#define RX_CH0_ST     (MHU1_RX_FRAME + 0x00U)
#define RX_CH0_CLR    (MHU1_RX_FRAME + 0x08U)
#define DOORBELL_BIT  0x1U

/* ----- peer-core release (portable <alp/mproc.h> boot surface) ----- */
#define HE_LOAD_ADDR 0x58000000U /* HE ITCM global alias = HE-APP loadAddress */

/* ----- round-trip parameters ----- */
#define IPC_PAYLOAD_WORDS 14U /* 14 + seq + len = 16 words = 64 bytes/mailbox */
#define ROUND_TRIPS       64U /* N round-trips before the HE app idles       */

/*
 * Shared mailbox header agreed by both cores. 64 bytes total so request and
 * reply mailboxes sit one cache-line apart at the fixed SRAM0 offsets below.
 */
struct ipc_msg {
	uint32_t seq;                        /* request sequence number       */
	uint32_t len;                        /* valid payload words (<= max)  */
	uint32_t payload[IPC_PAYLOAD_WORDS]; /* request data / reply = data+1 */
};

/* Fixed global-SRAM0 mailbox + counter addresses, clear of the beacons. */
#define REQ_MBOX  ((volatile struct ipc_msg *)0x02002000U) /* HE writes, HP reads  */
#define RPL_MBOX  ((volatile struct ipc_msg *)0x02002040U) /* HP writes, HE reads  */
#define RT_DONE   ((volatile uint32_t *)0x02002080U)       /* HE round-trips done  */
#define RT_BAD    ((volatile uint32_t *)0x02002084U)       /* HE reply mismatches  */
#define HP_SERVED ((volatile uint32_t *)0x02002088U)       /* HP requests serviced */

/* Per-core liveness beacon (same scheme as master/doorbell).  The board #if
 * is core-role selection (HP vs HE build), not a peripheral-presence gate. */
#if defined(CONFIG_BOARD_ALP_E1M_AEN801_M55_HP)
#define ROLE        "HP"
#define SELF_BEACON ((volatile uint32_t *)0x02000010U)
#define SELF_MAGIC  0xB1B10090U
#else
#define ROLE        "HE"
#define SELF_BEACON ((volatile uint32_t *)0x02001010U)
#define SELF_MAGIC  0xB1B100E0U
#endif

/* Ring the other core: publish shared writes, then assert CH0_SET. */
static inline void mhu_ring(void)
{
	barrier_dmem_fence_full(); /* DMB: shared-buffer writes visible before signal */
	sys_write32(DOORBELL_BIT, TX_CH0_SET);
}

/* Non-blocking check + ack of an inbound doorbell on my RX frame. */
static inline int mhu_check_clear(void)
{
	if (sys_read32(RX_CH0_ST) & DOORBELL_BIT) {
		sys_write32(0xFFFFFFFFU, RX_CH0_CLR); /* ack/clear the channel */
		barrier_dmem_fence_full();            /* DMB: see payload after signal */
		return 1;
	}
	return 0;
}

/* Bring up my non-secure MHU-1 sender link (request access, wait ready). */
static inline void mhu_sender_ready(void)
{
	sys_write32(1U, TX_ACC_REQ);
	while (sys_read32(TX_ACC_RDY) == 0U) {
		/* wait for the link to come ready */
	}
}

int main(void)
{
	printk("\n=== aen-dualcore-ipc (%s) -- bidirectional HE<->HP request/response ===\n", ROLE);

	SELF_BEACON[0] = SELF_MAGIC;
	SELF_BEACON[1] = 0U;

#if defined(CONFIG_BOARD_ALP_E1M_AEN801_M55_HP)
	/*
	 * HP master + responder. Release HE FIRST (it is loaded but not booted by
	 * the SES), then service requests: for each HE doorbell, read REQ_MBOX,
	 * write RPL_MBOX with payload[i]+1 and the echoed seq, and ring HE back.
	 */
	*HP_SERVED      = 0U;
	alp_status_t rc = alp_mproc_boot_core(ALP_CORE_M55_HE, HE_LOAD_ADDR);

	printk("alp_mproc_boot_core(M55-HE, 0x%08x) rc=%d -- responder up on MHU-1\n",
	       HE_LOAD_ADDR,
	       (int)rc);

	mhu_sender_ready(); /* my TX frame = the HP->HE reply ring */

	uint32_t served   = 0U;
	uint32_t last_seq = 0U;

	for (uint32_t hb = 1U;; hb++) {
		SELF_BEACON[1] = hb;

		mhu_check_clear(); /* drain HE's doorbell (a non-blocking hint only) */

		/*
		 * Handshake on the SHARED request seq, not the doorbell edge: REQ_MBOX
		 * is coherent SRAM, so a new seq is the reliable "request ready" signal
		 * (the single-bit MHU channel races on back-to-back rings).
		 */
		uint32_t seq = REQ_MBOX->seq;

		if (seq != last_seq && seq != 0U) {
			barrier_dmem_fence_full(); /* see the request payload after seq */
			uint32_t len = REQ_MBOX->len;

			if (len > IPC_PAYLOAD_WORDS) {
				len = IPC_PAYLOAD_WORDS;
			}
			for (uint32_t i = 0U; i < len; i++) {
				RPL_MBOX->payload[i] = REQ_MBOX->payload[i] + 1U;
			}
			RPL_MBOX->len = len;
			barrier_dmem_fence_full();
			RPL_MBOX->seq = seq; /* echo seq LAST: HE polls it changing */

			last_seq = seq;
			served++;
			*HP_SERVED = served;

			mhu_ring(); /* hint HE that the reply is ready */
		}

		for (volatile uint32_t d = 0U; d < 2000U; d++) {
		}
	}
#else
	/*
	 * HE requester. Bring up its sender link, then run N round-trips: fill
	 * REQ_MBOX, ring HP, wait for HP's reply doorbell + the echoed seq, verify
	 * every reply word == request word + 1, and count the round-trip.
	 */
	*RT_DONE = 0U;
	*RT_BAD  = 0U;

	mhu_sender_ready(); /* my TX frame = the HE->HP request ring */

	printk("HE requester ready -- running %u round-trips over MHU-1\n", ROUND_TRIPS);

	uint32_t completed = 0U;
	uint32_t bad       = 0U;

	for (uint32_t seq = 1U; seq <= ROUND_TRIPS; seq++) {
		/* Build the request: payload[i] = seq*0x100 + i. */
		for (uint32_t i = 0U; i < IPC_PAYLOAD_WORDS; i++) {
			REQ_MBOX->payload[i] = (seq << 8) + i;
		}
		REQ_MBOX->len = IPC_PAYLOAD_WORDS;
		barrier_dmem_fence_full();
		REQ_MBOX->seq = seq; /* seq LAST: HP keys off it via the doorbell */

		mhu_ring(); /* hint HP that a request is ready */

		/*
		 * Wait on the SHARED reply seq (the real handshake); drain HP's doorbell
		 * as a non-blocking latency hint. Relying on the doorbell EDGE alone
		 * raced the single-bit channel re-arm and stalled after one round-trip.
		 */
		while (RPL_MBOX->seq != seq) {
			mhu_check_clear();
		}
		barrier_dmem_fence_full(); /* see the reply payload after the seq */

		/* Verify reply == request payload + 1, word for word. */
		int ok = (RPL_MBOX->len == IPC_PAYLOAD_WORDS);

		for (uint32_t i = 0U; ok && i < IPC_PAYLOAD_WORDS; i++) {
			if (RPL_MBOX->payload[i] != ((seq << 8) + i) + 1U) {
				ok = 0;
			}
		}

		if (ok) {
			completed++;
		} else {
			bad++;
		}
		*RT_DONE       = completed;
		*RT_BAD        = bad;
		SELF_BEACON[1] = seq;
	}

	printk("RESULT: round-trips completed=%u mismatches=%u (PASS if completed=%u, mismatches=0)\n",
	       completed,
	       bad,
	       ROUND_TRIPS);

	/* Idle: keep the beacon live so the bench read sees the final counters. */
	for (uint32_t hb = ROUND_TRIPS + 1U;; hb++) {
		SELF_BEACON[1] = hb;
		for (volatile uint32_t d = 0U; d < 200000U; d++) {
		}
	}
#endif
	return 0;
}
