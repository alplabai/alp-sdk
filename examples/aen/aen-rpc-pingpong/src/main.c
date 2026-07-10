/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-rpc-pingpong -- a real Zephyr ipc_service / OpenAMP-RPMsg ping/pong between
 * the two E8 M55 cores. HP is the host + boots HE via the portable
 * alp_mproc_boot_core() (<alp/mproc.h>; the backend registry routes it to the
 * SoM's boot authority -- the SE boot service on AEN); both open the ipc0
 * instance and register a "pingpong" endpoint over the alif,mhuv2-mbox MBOX
 * driver. HP sends a ping; HE echoes a pong; HP counts.
 * Resolves alp-sdk #45 (mailbox.controller) / #50 (alp_rpc_open NOT_READY).
 *
 * The transport runs over the non-secure HE<->HP MHU-1 pair (per-core alias) +
 * a shared SRAM0 vring carve-out. Two silicon quirks the MBOX driver handles:
 * the RX combined IRQ does not fire on this frame (the driver POLLs CH0_STAT),
 * and the sender must assert ACCESS_REQUEST before each ring or it does not
 * propagate.
 *
 * Liveness/result mirrored to global-SRAM0 beacons (read over SWD; HE's console
 * is in HE-local memory):
 *   HP: self 0x02000010 (magic+hb) | bound 0x02000048 | pongs 0x0200004C
 *   HE: self 0x02001010 (magic+hb) | bound 0x02001048 | pings 0x0200104C
 */

#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/printk.h>
#include <zephyr/ipc/ipc_service.h>

#include <alp/mproc.h>

/* This is HP's belief about where HE's image lands, passed to
 * alp_mproc_boot_core() as the entry address -- it must agree with the HE
 * ATOC's loadAddress (see README "Package as a dual ATOC"), or HP will boot
 * HE into the wrong place. */
#define HE_LOAD_ADDR    0x58000000U /* HE ITCM global alias = HE-APP loadAddress */
#define PINGPONG_ROUNDS 16U

/* Core-role selection (HP vs HE build of this app), not a peripheral-presence gate.
 * Both cores' beacons live in the same global SRAM0 region but at disjoint
 * offsets (0x02000xxx vs 0x02001xxx) so a single SWD memory read can show
 * both sides' state side by side without them clobbering each other; the
 * distinct SELF_MAGIC values let a dump be identified as HP's or HE's even
 * without knowing which offset was read. */
#if defined(CONFIG_BOARD_ALP_E1M_AEN801_M55_HP)
#define ROLE        "HP"
#define SELF_BEACON ((volatile uint32_t *)0x02000010U)
#define B_BOUND     ((volatile uint32_t *)0x02000048U)
#define B_CNT       ((volatile uint32_t *)0x0200004CU)
#define SELF_MAGIC  0xB1B10090U
#define IS_HOST     1
#else
#define ROLE        "HE"
#define SELF_BEACON ((volatile uint32_t *)0x02001010U)
#define B_BOUND     ((volatile uint32_t *)0x02001048U)
#define B_CNT       ((volatile uint32_t *)0x0200104CU)
#define SELF_MAGIC  0xB1B100E0U
#define IS_HOST     0
#endif

/* The whole RPMsg payload: just a sequence number. Kept minimal because it's
 * copied through the shared SRAM0 vring on every send -- there's nothing here
 * to validate or interpret beyond "a message arrived". */
struct msg {
	uint32_t seq;
};

/* bound/cnt are written from the ipc_service RX/bind callbacks (invoked off
 * the OpenAMP backend's own context, not from main()'s call stack), so both
 * need volatile even though this is single-core-per-image code. */
static struct ipc_ept    ept;
static volatile bool     bound;
static volatile uint32_t cnt;

static void on_bound(void *priv)
{
	ARG_UNUSED(priv);
	/* Fires once the far side's endpoint of the same name has registered too
	 * -- the NS (name-service) handshake over the vring completed. Nothing in
	 * main() drives this directly; it can happen at any point after
	 * register_endpoint(), which is why main() polls `bound` below instead of
	 * assuming it's true immediately after the call returns. */
	bound    = true;
	*B_BOUND = 1U;
}

static void on_recv(const void *data, size_t len, void *priv)
{
	ARG_UNUSED(priv);
	if (len < sizeof(struct msg)) {
		return;
	}
	const struct msg *m = data;

	cnt++;
	*B_CNT = cnt;
#if !IS_HOST
	/* HE: echo the ping straight back as a pong. */
	struct msg pong = { .seq = m->seq };

	(void)ipc_service_send(&ept, &pong, sizeof(pong));
#else
	/* HP: a pong arriving at all (within the round budget checked in
	 * main()'s loop) is the whole test -- the sequence number isn't
	 * cross-checked against what was sent. */
	ARG_UNUSED(m);
#endif
}

/* The endpoint name is the binding key: HP's and HE's ipc_service instances
 * match "pingpong" endpoints across the vring during the NS handshake, so
 * this string (and only this string) has to agree between the two builds. */
static struct ipc_ept_cfg ept_cfg = {
	.name = "pingpong",
	.cb   = { .bound = on_bound, .received = on_recv },
};

int main(void)
{
	printk("\n=== aen-rpc-pingpong (%s) ===\n", ROLE);
	SELF_BEACON[0] = SELF_MAGIC;
	SELF_BEACON[1] = 0U;
	*B_BOUND       = 0U;
	*B_CNT         = 0U;

#if IS_HOST
	/* Only HP calls this -- HE never self-boots. On AEN, alp_mproc_boot_core()
	 * routes through the SoM's boot authority (the SE boot service), which is
	 * the entity actually allowed to release HE from reset/hold; HP asking on
	 * HE's behalf keeps a single, attestable boot path instead of HE trusting
	 * its own start conditions. A nonzero rc here means HE was never released,
	 * so nothing past this point (ipc0, the endpoint) will ever bind. */
	alp_status_t brc = alp_mproc_boot_core(ALP_CORE_M55_HE, HE_LOAD_ADDR);

	printk("[HP] boot_core rc=%d\n", (int)brc);
#endif

	const struct device *ipc = DEVICE_DT_GET(DT_NODELABEL(ipc0));

	if (!device_is_ready(ipc)) {
		printk("[%s] ipc0 not ready\n", ROLE);
		return 0;
	}

	int rc = ipc_service_open_instance(ipc);

	/* -EALREADY means the backend auto-opened the instance already (some
	 * backends do this at device-init time) -- not a failure, just means this
	 * explicit open was redundant. Any other negative rc is a real problem
	 * with the MHUv2 mailbox / shared-memory setup underneath. */
	if (rc < 0 && rc != -EALREADY) {
		printk("[%s] open_instance rc=%d\n", ROLE, rc);
		return 0;
	}

	/* register_endpoint can report -EAGAIN until the instance is INITED. */
	for (uint32_t t = 0U; t < 200U; t++) {
		rc = ipc_service_register_endpoint(ipc, &ept, &ept_cfg);
		if (rc == 0) {
			break;
		}
		k_msleep(20);
	}
	printk("[%s] register_endpoint rc=%d\n", ROLE, rc);

	/* Wait for the endpoint to bind (the NS handshake completes async). */
	for (uint32_t t = 0U; !bound && t < 500U; t++) {
		k_msleep(10);
	}
	printk("[%s] bound=%d\n", ROLE, (int)bound);

	for (uint32_t hb = 1U;; hb++) {
		/* Heartbeat: a monotonically increasing beacon word, so a bench
		 * script polling over SWD can tell "still running" from "hung/crashed"
		 * without needing a console (HE's console isn't SWD-visible). */
		SELF_BEACON[1] = hb;
#if IS_HOST
		/* HP drives the rounds: one ping per 100 ms until ROUNDS pongs. */
		if (bound && hb <= PINGPONG_ROUNDS) {
			struct msg ping = { .seq = hb };

			(void)ipc_service_send(&ept, &ping, sizeof(ping));
		}
		if (hb == PINGPONG_ROUNDS + 5U) {
			printk("RESULT: pingpong %s -- pongs=%u/%u\n",
			       (cnt >= PINGPONG_ROUNDS) ? "PASS" : "INCOMPLETE",
			       (unsigned)cnt,
			       PINGPONG_ROUNDS);
		}
#endif
		k_msleep(100);
	}
	return 0;
}
