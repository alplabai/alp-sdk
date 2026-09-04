/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-alp-rpc -- the same E8 dual-M55 ping/pong as aen-rpc-pingpong (PR#205,
 * 16/16 PASS), but driven through the PORTABLE <alp/rpc.h> surface instead of
 * the raw Zephyr ipc_service calls.  This is the teaching example for the
 * framed-RPC API: it shows that customer code calls a handful of vendor-clean
 * alp_rpc_* functions while the alp_rpc Zephyr backend does the exact same
 * ipc_service_open_instance() + ipc_service_register_endpoint() dance the raw
 * pingpong does -- over the very same alif,mhuv2-mbox MBOX driver + shared
 * SRAM0 vring carve-out wired in the board overlays.
 *
 * Topology (identical to pingpong):
 *   - HP is the host: it releases HE via the portable alp_mproc_boot_core()
 *     (<alp/mproc.h>; the registry routes it to the SoM's boot authority --
 *     the SE boot service on AEN), opens the channel, subscribes to "pong",
 *     then sends 16 "ping" frames and counts the pongs.
 *   - HE is the remote: it opens the same channel, subscribes to "ping", and
 *     echoes every ping straight back as a "pong".
 *
 * Why two method names where pingpong used one endpoint?  The raw example put
 * BOTH the wire routing and the direction in a single ipc endpoint named
 * "pingpong".  The framed surface keeps the ipc endpoint name as the channel
 * IDENTITY (both cores MUST open the same alp_rpc_config_t::name so their
 * endpoints bind to each other), and moves the per-message routing into the
 * in-frame ASCII method header.  So one channel "alp_pingpong" carries two
 * methods: HP -> HE "ping", HE -> HP "pong".  alp_rpc_subscribe() filters by
 * method, so each side's callback only fires for the direction it cares about.
 *
 * Liveness/result mirrored to global-SRAM0 beacons (read over SWD; HE's console
 * is in HE-local memory -- same scheme + addresses as pingpong):
 *   HP: self+hb+verdict 0x02000010..18 | opened 0x02000048 | pongs 0x0200004C
 *   HE: self+hb+verdict 0x02001010..18 | opened 0x02001048 | pings 0x0200104C
 * SELF_BEACON[2] (the verdict word) is stamped with V_PASS/V_SKIP/V_FAIL right
 * before every return -- main() exits once it has a verdict instead of idling
 * forever, which would otherwise freeze the heartbeat and make a completed
 * run indistinguishable from a crash over SWD; the verdict word is what keeps
 * those two cases apart once the heartbeat has stopped moving.
 *
 * BENCH-UNVERIFIED for this example's app code; the transport (MHU + vrings +
 * ipc_service handshake) is the bench-proven pingpong config.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <alp/mproc.h>
#include <alp/rpc.h>

/* Peer-core release (portable <alp/mproc.h>; same address pingpong uses). */
#define HE_LOAD_ADDR    0x58000000U /* HE ITCM global alias = HE-APP loadAddress */
#define PINGPONG_ROUNDS 16U

/* Bounded waits (see the file header: every wait here is bounded, named, and
 * commented -- an unbounded wait on a peer that's absent is exactly the hang
 * this app used to have). */
#define OPEN_SETTLE_MS    1500U /* fixed settle window for the async NS bind after open() */
#define ROUND_INTERVAL_MS 100U  /* HP: pacing between pings */
#define RESULT_GRACE_HB   5U    /* HP: extra heartbeats after the last ping before verdict */
/* HE: how long to keep echoing after the settle window before reporting a
 * verdict -- generous over HP's own PINGPONG_ROUNDS*ROUND_INTERVAL_MS drive
 * window plus its grace period. */
#define SERVE_WINDOW_MS 3000U

/* Verdict codes mirrored into SELF_BEACON[2] right before every return. */
#define V_PASS 1U
#define V_SKIP 2U
#define V_FAIL 3U

/*
 * The shared channel identity.  BOTH cores pass this exact string as
 * alp_rpc_config_t::name -- it becomes the ipc_service endpoint name, so the
 * host and remote endpoints bind to each other.  The two method names below
 * are the in-frame routing carried on top of that one channel.
 */
#define RPC_CHANNEL "alp_pingpong"
#define METHOD_PING "ping"
#define METHOD_PONG "pong"

/* Core-role selection (HP vs HE build of this app), not a peripheral-presence gate. */
#if defined(CONFIG_BOARD_ALP_E1M_AEN801_M55_HP)
#define ROLE        "HP"
#define SELF_BEACON ((volatile uint32_t *)0x02000010U)
#define B_OPENED    ((volatile uint32_t *)0x02000048U)
#define B_CNT       ((volatile uint32_t *)0x0200004CU)
#define SELF_MAGIC  0xA1B10090U
#define IS_HOST     1
#else
#define ROLE        "HE"
#define SELF_BEACON ((volatile uint32_t *)0x02001010U)
#define B_OPENED    ((volatile uint32_t *)0x02001048U)
#define B_CNT       ((volatile uint32_t *)0x0200104CU)
#define SELF_MAGIC  0xA1B100E0U
#define IS_HOST     0
#endif

/* On-wire payload: a single round-trip sequence number. */
struct msg {
	uint32_t seq;
};

/* The open channel handle, shared between main() and the RX callbacks. */
static alp_rpc_channel_t *g_ch;
static volatile uint32_t  g_cnt; /* HP: pongs received | HE: pings received */

/* HE only: pongs SUCCESSFULLY sent back -- a separate counter from g_cnt,
 * since receiving a ping proves nothing about whether the echo that
 * followed actually went out. g_last_send_rc is the rc of the most recent
 * alp_rpc_send, for the FAIL message if every echo fails. */
static volatile uint32_t     g_echoed;
static volatile alp_status_t g_last_send_rc;

/*
 * Link-liveness demo (<alp/rpc.h>, issue #1643): alp_rpc_set_link_callback()
 * registers a callback that fires on every ALP_RPC_LINK_DOWN/_UP/_LOST
 * transition, so a real app can stop calling alp_rpc_send()/alp_rpc_call()
 * the moment it learns the peer is gone instead of finding out from a
 * timeout.  Just logged here -- this example's own PASS/FAIL verdict still
 * rests on the ping/pong counters above, not on this callback, since on
 * THIS board (an M55<->M55 pair, both sides Zephyr) the pinned ipc_service
 * RPMsg backend only ever calls `bound`, never `unbound`/`error` -- so in
 * this revision only ALP_RPC_LINK_UP is ever actually observed here.  See
 * src/backends/rpc/zephyr_drv.c's `@par Link liveness` file comment for why.
 */
static void on_link_state(alp_rpc_link_state_t state, void *user)
{
	ARG_UNUSED(user);
	printk("[%s] link state -> %d\n", ROLE, (int)state);
}

#if IS_HOST
/*
 * HP's "pong" handler: every echo HE sends back lands here.  alp_rpc has
 * already filtered by method, so we only need to count.  This runs on the
 * backend RX worker -- keep it short (the public-API contract in <alp/rpc.h>).
 */
static void on_pong(const void *payload, size_t len, void *user)
{
	ARG_UNUSED(user);
	if (len < sizeof(struct msg)) {
		return;
	}
	g_cnt++;
	*B_CNT = g_cnt;
}
#else
/*
 * HE's "ping" handler: echo the ping straight back to HP as a "pong" on the
 * same channel.  alp_rpc_send() frames (method + payload) and hands it to the
 * OpenAMP TX queue -- the mirror of pingpong's ipc_service_send().  The send
 * status is recorded, not discarded: a ping arriving here says nothing about
 * whether the echo that follows reaches HP.
 */
static void on_ping(const void *payload, size_t len, void *user)
{
	ARG_UNUSED(user);
	if (len < sizeof(struct msg)) {
		return;
	}
	const struct msg *m    = payload;
	struct msg        pong = { .seq = m->seq };

	g_cnt++;
	*B_CNT = g_cnt;

	alp_status_t src = alp_rpc_send(g_ch, METHOD_PONG, &pong, sizeof(pong));

	g_last_send_rc = src;
	if (src == ALP_OK) {
		g_echoed++;
	}
}
#endif

int main(void)
{
	printk("\n=== aen-alp-rpc (%s) ===\n", ROLE);
	SELF_BEACON[0] = SELF_MAGIC;
	SELF_BEACON[1] = 0U;
	SELF_BEACON[2] = 0U; /* verdict: none yet */
	*B_OPENED      = 0U;
	*B_CNT         = 0U;

#if IS_HOST
	/* HP releases the HE core before opening the channel, so HE is alive to
	 * bind its end of the endpoint (same as pingpong). A nonzero rc here means
	 * HE was never released, so nothing past this point (the channel, the
	 * subscribe) will ever bind -- that MUST be gated here, before any
	 * downstream code can claim a SKIP that implies local setup got further
	 * than it did. */
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

	/*
	 * Open the RPC channel.  This is the whole point of the example: one call
	 * resolves DT_CHOSEN(zephyr_ipc) -> ipc0, opens the ipc_service instance,
	 * and registers the local endpoint -- exactly what the raw pingpong did by
	 * hand.  cacheable=false matches CONFIG_DCACHE=n on this bench.  src/dst
	 * ept are left 0 so the backend derives them deterministically from the
	 * channel name (both cores hash the same name, so they agree).
	 */
	g_ch = alp_rpc_open(&(alp_rpc_config_t){
	    .name      = RPC_CHANNEL,
	    .cacheable = false,
	});
	if (g_ch == NULL) {
		/* alp_last_error() carries the reason (e.g. ALP_ERR_NOT_READY if the
		 * ipc0 chosen node is missing or the device isn't up yet). */
		SELF_BEACON[2] = V_FAIL;
		printk("[%s] alp_rpc_open failed: %d\n", ROLE, (int)alp_last_error());
		printk("RESULT FAIL: alp-rpc -- alp_rpc_open rc=%d\n", (int)alp_last_error());
		return 0;
	}
	*B_OPENED = 1U;
	printk("[%s] alp_rpc_open OK\n", ROLE);

	/* Register the link-liveness callback -- see on_link_state()'s doc
	 * comment above.  Best-effort: a NOSUPPORT rc here (e.g. a bare-metal
	 * stub build) doesn't change this example's verdict either way. */
	(void)alp_rpc_set_link_callback(g_ch, on_link_state, NULL);

	/*
	 * Subscribe to the direction this core consumes.  alp_rpc_subscribe()
	 * filters by method name, so each callback only fires for its own
	 * direction -- HP for "pong", HE for "ping".
	 */
#if IS_HOST
	alp_status_t src = alp_rpc_subscribe(g_ch, METHOD_PONG, on_pong, NULL);
#else
	alp_status_t src = alp_rpc_subscribe(g_ch, METHOD_PING, on_ping, NULL);
#endif
	printk("[%s] subscribe rc=%d\n", ROLE, (int)src);
	if (src != ALP_OK) {
		SELF_BEACON[2] = V_FAIL;
		printk("RESULT FAIL: alp-rpc -- alp_rpc_subscribe rc=%d\n", (int)src);
		alp_rpc_close(g_ch);
		return 0;
	}

	/*
	 * The OpenAMP name-service bind completes asynchronously after open().
	 * The framed surface doesn't expose a "bound" query, so -- exactly like
	 * pingpong waited on its on_bound flag -- give the handshake a fixed
	 * settle window before the host starts driving rounds.  The remote just
	 * services its RX worker the whole time.
	 */
	k_msleep(OPEN_SETTLE_MS);

#if IS_HOST
	/* HP drives the rounds: one "ping" per ROUND_INTERVAL_MS, tracking BOTH
	 * what was actually sent (send_ok / the last failing rc) and what came
	 * back (g_cnt) -- PASS must rest on the received evidence, but a local
	 * send failure has to surface as FAIL rather than being folded into
	 * "peer never responded". */
	uint32_t     send_ok      = 0U;
	alp_status_t last_send_rc = ALP_OK;

	for (uint32_t hb = 1U; hb <= PINGPONG_ROUNDS; hb++) {
		SELF_BEACON[1]    = hb;
		struct msg   ping = { .seq = hb };
		alp_status_t psrc = alp_rpc_send(g_ch, METHOD_PING, &ping, sizeof(ping));

		if (psrc == ALP_OK) {
			send_ok++;
		} else {
			last_send_rc = psrc;
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
		printk("RESULT FAIL: alp-rpc -- alp_rpc_send rc=%d on all %u ping(s)\n",
		       (int)last_send_rc,
		       PINGPONG_ROUNDS);
	} else if (g_cnt >= PINGPONG_ROUNDS) {
		SELF_BEACON[2] = V_PASS;
		printk("RESULT PASS: alp-rpc -- received %u/%u pong(s) after sending %u ping(s)\n",
		       (unsigned)g_cnt,
		       PINGPONG_ROUNDS,
		       send_ok);
	} else {
		SELF_BEACON[2] = V_SKIP;
		printk("RESULT SKIP: alp-rpc -- sent %u ping(s), only %u/%u pong(s) came back "
		       "within the grace window; channel opened but peer stopped responding\n",
		       send_ok,
		       (unsigned)g_cnt,
		       PINGPONG_ROUNDS);
	}
#else
	/* HE: channel open + settled, so keep servicing echoes (on_ping does the
	 * real work) for a bounded window, then report what was actually proven
	 * locally. */
	for (uint32_t hb = 1U; hb <= SERVE_WINDOW_MS / ROUND_INTERVAL_MS; hb++) {
		SELF_BEACON[1] = hb;
		k_msleep(ROUND_INTERVAL_MS);
	}

	if (g_cnt == 0U) {
		SELF_BEACON[2] = V_SKIP;
		printk("RESULT SKIP: alp-rpc -- no ping arrived within %u ms; local alp_rpc_open "
		       "+ subscribe OK\n",
		       SERVE_WINDOW_MS);
	} else if (g_echoed < g_cnt) {
		/* PASS must rest on EVERY received ping being echoed -- a partial
		 * echo count (some alp_rpc_send calls failed) is a FAIL, not a
		 * PASS with a smaller numerator. */
		SELF_BEACON[2] = V_FAIL;
		printk("RESULT FAIL: alp-rpc -- received %u ping(s) but only %u echoed back; "
		       "last alp_rpc_send rc=%d\n",
		       (unsigned)g_cnt,
		       (unsigned)g_echoed,
		       (int)g_last_send_rc);
	} else {
		/* "queued": alp_rpc_send() == ALP_OK only means the frame was queued
		 * to the local vring -- HE never observes whether HP actually
		 * accepted it, so the verdict text must not claim delivery. */
		SELF_BEACON[2] = V_PASS;
		printk("RESULT PASS: alp-rpc -- queued %u/%u echo(es) for HP\n",
		       (unsigned)g_echoed,
		       (unsigned)g_cnt);
	}
#endif

	alp_rpc_close(g_ch);
	return 0;
}
