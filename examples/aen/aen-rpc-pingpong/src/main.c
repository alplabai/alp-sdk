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
 *   HP: self+hb+verdict 0x02000010..18 | bound 0x02000048 | pongs 0x0200004C
 *   HE: self+hb+verdict 0x02001010..18 | bound 0x02001048 | pings 0x0200104C
 * SELF_BEACON[2] (the verdict word) is stamped with V_PASS/V_SKIP/V_FAIL right
 * before every return -- main() now exits once it has a verdict instead of
 * idling forever, which would otherwise freeze the heartbeat and make a
 * completed run indistinguishable from a crash over SWD; the verdict word is
 * what keeps those two cases apart once the heartbeat has stopped moving.
 */

#include <stdbool.h>
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

/* Bounded waits (see the file header: every wait here is bounded, named,
 * and commented -- an unbounded wait on a peer that's absent is exactly the
 * hang this app used to have). */
#define REGISTER_TIMEOUT_MS 4000U /* max wait for register_endpoint() to clear -EAGAIN */
#define REGISTER_POLL_MS    20U
#define BIND_TIMEOUT_MS     5000U /* max wait for the NS handshake ("bound") to complete */
#define BIND_POLL_MS        10U
#define ROUND_INTERVAL_MS   100U /* HP: pacing between pings */
#define RESULT_GRACE_HB     5U   /* HP: extra heartbeats after the last ping before verdict */
/* HE: how long to keep echoing after bind before reporting a verdict --
 * generous over HP's own PINGPONG_ROUNDS*ROUND_INTERVAL_MS drive window
 * plus its grace period. */
#define SERVE_WINDOW_MS 3000U

/* Verdict codes mirrored into SELF_BEACON[2] right before every return. */
#define V_PASS 1U
#define V_SKIP 2U
#define V_FAIL 3U

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

/* bound/cnt/echoed/last_send_rc are written from the ipc_service RX/bind
 * callbacks (invoked off the OpenAMP backend's own context, not from main()'s
 * call stack), so all need volatile even though this is single-core-per-image
 * code. */
static struct ipc_ept    ept;
static volatile bool     bound;
static volatile uint32_t cnt;          /* HP: pongs received | HE: pings received */
static volatile uint32_t echoed;       /* HE only: pongs SUCCESSFULLY sent back (a
                                         * separate counter from `cnt` -- receiving a
                                         * ping proves nothing about whether the echo
                                         * that followed actually went out). */
static volatile int      last_send_rc; /* HE only: rc of the most recent echo send,
                                         * for the FAIL message if every echo fails. */

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
	/* HE: echo the ping straight back as a pong -- and record whether the
	 * send actually succeeded. A ping arriving here says nothing about
	 * whether the echo that follows reaches HP; only a checked send does. */
	struct msg pong = { .seq = m->seq };
	int        src  = ipc_service_send(&ept, &pong, sizeof(pong));

	last_send_rc = src;
	if (src >= 0) {
		echoed++;
	}
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
	SELF_BEACON[2] = 0U; /* verdict: none yet */
	*B_BOUND       = 0U;
	*B_CNT         = 0U;

#if IS_HOST
	/* Only HP calls this -- HE never self-boots. On AEN, alp_mproc_boot_core()
	 * routes through the SoM's boot authority (the SE boot service), which is
	 * the entity actually allowed to release HE from reset/hold; HP asking on
	 * HE's behalf keeps a single, attestable boot path instead of HE trusting
	 * its own start conditions. A nonzero rc here means HE was never released,
	 * so nothing past this point (ipc0, the endpoint) will ever bind -- that
	 * MUST be gated here, before any downstream code can claim a SKIP that
	 * implies local setup got further than it did. */
	alp_status_t brc = alp_mproc_boot_core(ALP_CORE_M55_HE, HE_LOAD_ADDR);

	printk("[HP] boot_core rc=%d\n", (int)brc);

	/* This build ships CONFIG_HAS_ALIF_SE_SERVICES=y and no native_sim
	 * overlay, so alp_mproc_boot_core() always resolves to the E8 SE
	 * backend for ALP_CORE_M55_HE (<alp/mproc.h>'s ALP_ERR_NOSUPPORT case
	 * -- "no boot authority for core in this build" -- is not reachable
	 * here). Any nonzero rc, NOSUPPORT included, is therefore a real
	 * local error: report it as FAIL, not a skippable environment state. */
	if (brc != ALP_OK) {
		SELF_BEACON[2] = V_FAIL;
		printk("RESULT FAIL: alp_mproc_boot_core rc=%d\n", (int)brc);
		return 0;
	}
#endif

	const struct device *ipc = DEVICE_DT_GET(DT_NODELABEL(ipc0));

	if (!device_is_ready(ipc)) {
		SELF_BEACON[2] = V_FAIL;
		printk("[%s] ipc0 not ready\n", ROLE);
		printk("RESULT FAIL: pingpong -- ipc0 device not ready\n");
		return 0;
	}

	int rc = ipc_service_open_instance(ipc);

	/* -EALREADY means the backend auto-opened the instance already (some
	 * backends do this at device-init time) -- not a failure, just means this
	 * explicit open was redundant. Any other negative rc is a real problem
	 * with the MHUv2 mailbox / shared-memory setup underneath. */
	if (rc < 0 && rc != -EALREADY) {
		SELF_BEACON[2] = V_FAIL;
		printk("[%s] open_instance rc=%d\n", ROLE, rc);
		printk("RESULT FAIL: pingpong -- ipc_service_open_instance rc=%d\n", rc);
		return 0;
	}

	/* register_endpoint can report -EAGAIN until the instance is INITED. */
	for (uint32_t t = 0U; t < REGISTER_TIMEOUT_MS / REGISTER_POLL_MS; t++) {
		rc = ipc_service_register_endpoint(ipc, &ept, &ept_cfg);
		if (rc == 0) {
			break;
		}
		k_msleep(REGISTER_POLL_MS);
	}
	printk("[%s] register_endpoint rc=%d\n", ROLE, rc);
	if (rc != 0) {
		SELF_BEACON[2] = V_FAIL;
		printk("RESULT FAIL: pingpong -- register_endpoint rc=%d after %u ms\n",
		       rc,
		       REGISTER_TIMEOUT_MS);
		return 0;
	}

	/* Wait for the endpoint to bind (the NS handshake completes async). */
	for (uint32_t t = 0U; !bound && t < BIND_TIMEOUT_MS / BIND_POLL_MS; t++) {
		k_msleep(BIND_POLL_MS);
	}
	printk("[%s] bound=%d\n", ROLE, (int)bound);

	if (!bound) {
		SELF_BEACON[2] = V_SKIP;
		printk("RESULT SKIP: pingpong -- peer never bound the 'pingpong' endpoint within "
		       "%u ms; local ipc0 open + endpoint registration OK\n",
		       BIND_TIMEOUT_MS);
		(void)ipc_service_deregister_endpoint(&ept);
		return 0;
	}

#if IS_HOST
	/* HP drives the rounds: one ping per ROUND_INTERVAL_MS, tracking BOTH
	 * what was actually sent (send_ok) and what came back (cnt) -- PASS must
	 * rest on the received evidence, but a local send failure has to surface
	 * as FAIL rather than being folded into "peer never responded". */
	uint32_t send_ok = 0U;

	for (uint32_t hb = 1U; hb <= PINGPONG_ROUNDS; hb++) {
		SELF_BEACON[1]  = hb;
		struct msg ping = { .seq = hb };
		int        src  = ipc_service_send(&ept, &ping, sizeof(ping));

		if (src >= 0) {
			send_ok++;
		}
		k_msleep(ROUND_INTERVAL_MS);
	}
	/* Grace window: give the last few pongs time to arrive before evaluating. */
	for (uint32_t hb = PINGPONG_ROUNDS + 1U; hb <= PINGPONG_ROUNDS + RESULT_GRACE_HB; hb++) {
		SELF_BEACON[1] = hb;
		k_msleep(ROUND_INTERVAL_MS);
	}

	if (send_ok == 0U) {
		SELF_BEACON[2] = V_FAIL;
		printk("RESULT FAIL: pingpong -- ipc_service_send failed on all %u ping(s)\n",
		       PINGPONG_ROUNDS);
	} else if (cnt >= PINGPONG_ROUNDS) {
		SELF_BEACON[2] = V_PASS;
		printk("RESULT PASS: pingpong -- received %u/%u pong(s) after sending %u ping(s)\n",
		       (unsigned)cnt,
		       PINGPONG_ROUNDS,
		       send_ok);
	} else {
		SELF_BEACON[2] = V_SKIP;
		printk("RESULT SKIP: pingpong -- sent %u ping(s), only %u/%u pong(s) came back "
		       "within the grace window; peer bound but stopped responding\n",
		       send_ok,
		       (unsigned)cnt,
		       PINGPONG_ROUNDS);
	}
#else
	/* HE: bound, so keep servicing echoes (on_recv does the real work) for a
	 * bounded window, then report what was actually proven locally. */
	for (uint32_t hb = 1U; hb <= SERVE_WINDOW_MS / ROUND_INTERVAL_MS; hb++) {
		SELF_BEACON[1] = hb;
		k_msleep(ROUND_INTERVAL_MS);
	}

	if (cnt == 0U) {
		SELF_BEACON[2] = V_SKIP;
		printk("RESULT SKIP: pingpong -- bound but no ping arrived within %u ms; local "
		       "ipc0 open + endpoint registration OK\n",
		       SERVE_WINDOW_MS);
	} else if (echoed < cnt) {
		/* PASS must rest on EVERY received ping being echoed -- a partial
		 * echo count (some ipc_service_send calls failed) is a FAIL, not a
		 * PASS with a smaller numerator. */
		SELF_BEACON[2] = V_FAIL;
		printk("RESULT FAIL: pingpong -- received %u ping(s) but only %u echoed back; "
		       "last ipc_service_send rc=%d\n",
		       (unsigned)cnt,
		       (unsigned)echoed,
		       last_send_rc);
	} else {
		/* "queued": ipc_service_send() >= 0 only means the frame was queued
		 * to the local vring -- HE never observes whether HP actually
		 * accepted it, so the verdict text must not claim delivery. */
		SELF_BEACON[2] = V_PASS;
		printk("RESULT PASS: pingpong -- queued %u/%u echo(es) for HP\n",
		       (unsigned)echoed,
		       (unsigned)cnt);
	}
#endif

	(void)ipc_service_deregister_endpoint(&ept);
	return 0;
}
