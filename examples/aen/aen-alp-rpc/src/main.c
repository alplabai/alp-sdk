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
 *   - HP is the host: it releases HE (se_service_boot_cpu), opens the channel,
 *     subscribes to "pong", then sends 16 "ping" frames and counts the pongs.
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
 *   HP: self 0x02000010 (magic+hb) | opened 0x02000048 | pongs 0x0200004C
 *   HE: self 0x02001010 (magic+hb) | opened 0x02001048 | pings 0x0200104C
 *
 * vendor-ext, BENCH-UNVERIFIED for this example's app code; the transport
 * (MHU + vrings + ipc_service handshake) is the bench-proven pingpong config.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <se_service.h>

#include <alp/rpc.h>

/* SE-service handle to release the HE core (same constants pingpong uses). */
#define EXTSYS_1_HE     3U
#define HE_LOAD_ADDR    0x58000000U
#define PINGPONG_ROUNDS 16U

/*
 * The shared channel identity.  BOTH cores pass this exact string as
 * alp_rpc_config_t::name -- it becomes the ipc_service endpoint name, so the
 * host and remote endpoints bind to each other.  The two method names below
 * are the in-frame routing carried on top of that one channel.
 */
#define RPC_CHANNEL "alp_pingpong"
#define METHOD_PING "ping"
#define METHOD_PONG "pong"

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
static volatile uint32_t  g_cnt;

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
 * OpenAMP TX queue -- the mirror of pingpong's ipc_service_send().
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
	(void)alp_rpc_send(g_ch, METHOD_PONG, &pong, sizeof(pong));
}
#endif

int main(void)
{
	printk("\n=== aen-alp-rpc (%s) ===\n", ROLE);
	SELF_BEACON[0] = SELF_MAGIC;
	SELF_BEACON[1] = 0U;
	*B_OPENED      = 0U;
	*B_CNT         = 0U;

#if IS_HOST
	/* HP releases the HE core before opening the channel, so HE is alive to
	 * bind its end of the endpoint (same as pingpong). */
	int brc = se_service_boot_cpu(EXTSYS_1_HE, HE_LOAD_ADDR);

	printk("[HP] boot_cpu rc=%d\n", brc);
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
		printk("[%s] alp_rpc_open failed: %d\n", ROLE, (int)alp_last_error());
		return 0;
	}
	*B_OPENED = 1U;
	printk("[%s] alp_rpc_open OK\n", ROLE);

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

	/*
	 * The OpenAMP name-service bind completes asynchronously after open().
	 * The framed surface doesn't expose a "bound" query, so -- exactly like
	 * pingpong waited on its on_bound flag -- give the handshake a fixed
	 * settle window before the host starts driving rounds.  The remote just
	 * services its RX worker the whole time.
	 */
	k_msleep(1500);

	for (uint32_t hb = 1U;; hb++) {
		SELF_BEACON[1] = hb;
#if IS_HOST
		/* HP drives the rounds: one "ping" per 100 ms until ROUNDS sent. */
		if (hb <= PINGPONG_ROUNDS) {
			struct msg ping = { .seq = hb };

			(void)alp_rpc_send(g_ch, METHOD_PING, &ping, sizeof(ping));
		}
		if (hb == PINGPONG_ROUNDS + 5U) {
			printk("RESULT: alp-rpc %s -- pongs=%u/%u\n",
			       (g_cnt >= PINGPONG_ROUNDS) ? "PASS" : "INCOMPLETE",
			       (unsigned)g_cnt,
			       PINGPONG_ROUNDS);
		}
#endif
		k_msleep(100);
	}
	return 0;
}
