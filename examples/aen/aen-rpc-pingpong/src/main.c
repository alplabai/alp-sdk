/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-rpc-pingpong -- a real Zephyr ipc_service / OpenAMP-RPMsg ping/pong between
 * the two E8 M55 cores. HP is the host + boots HE (se_service_boot_cpu); both
 * open the ipc0 instance and register a "pingpong" endpoint over the
 * alif,mhuv2-mbox MBOX driver. HP sends a ping; HE echoes a pong; HP counts.
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
#include <se_service.h>

/* HP boots HE through the Secure Enclave: EXTSYS_1_HE is the SE's CPU id for the
 * HE external subsystem, and HE_LOAD_ADDR is where HE's image sits in MRAM (the
 * ATOC load base). Once the endpoint binds, HP sends PINGPONG_ROUNDS pings. */
#define EXTSYS_1_HE     3U
#define HE_LOAD_ADDR    0x58000000U
#define PINGPONG_ROUNDS 16U

/* One source file, two roles: the board it is built for selects HP vs HE at
 * compile time. Each role gets its own beacon addresses -- HP in the 0x0200_00xx
 * SRAM0 window, HE in 0x0200_10xx -- so the host can watch both cores over SWD
 * (HE has no USB console). IS_HOST gates the host-only boot + ping-drive code. */
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

struct msg {
	uint32_t seq;
};

static struct ipc_ept    ept;
static volatile bool     bound;
static volatile uint32_t cnt;

static void on_bound(void *priv)
{
	ARG_UNUSED(priv);
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
	ARG_UNUSED(m);
#endif
}

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
	int brc = se_service_boot_cpu(EXTSYS_1_HE, HE_LOAD_ADDR);

	printk("[HP] boot_cpu rc=%d\n", brc);
#endif

	const struct device *ipc = DEVICE_DT_GET(DT_NODELABEL(ipc0));

	if (!device_is_ready(ipc)) {
		printk("[%s] ipc0 not ready\n", ROLE);
		return 0;
	}

	int rc = ipc_service_open_instance(ipc);

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
