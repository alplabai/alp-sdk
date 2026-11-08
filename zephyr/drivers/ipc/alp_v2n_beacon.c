/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Liveness beacon + reverse doorbell shared by every V2N/V2M CM33 firmware
 * that the A55 attaches to; see alp_v2n_beacon.h.  Moved verbatim from the
 * bench-proven examples/multicore/rpmsg-v2n/m33_sm/src/main.c (#683, #697,
 * #2586).
 */

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>

#include <alp/protocol/amp_beacon.h>
#include <alp/protocol/v2n_mhu_doorbell.h>

#include "alp_v2n_beacon.h"

#define RSC_TABLE_ADDR DT_REG_ADDR(DT_NODELABEL(rsctbl))

/* Version 2 (#2586): the attach-epoch word is published and honoured. */
#define RSCTBL_BEACON_VERSION (2U)

/* The beacon is the top 16 bytes of the rsctbl page; the resource table owns
 * the low end, so the two never overlap. */
#define RSCTBL_BEACON ALP_AMP_BEACON_AT(RSC_TABLE_ADDR, DT_REG_SIZE(DT_NODELABEL(rsctbl)))

/* Which register rings the A55 -- SWINT unit 12 SET (default) or RSP_INT_SET
 * of NS slot 8 with CONFIG_ALP_V2N_DOORBELL_RSP_CH8 -- comes from
 * <alp/protocol/v2n_mhu_doorbell.h>; the A55 must be built for the same line.
 * The RSP interrupt of NS slot 5 (what mbox_send() raises) reaches no A55 GIC
 * line (#697 cycle 10), hence the direct write. */
#define ALP_M33_DOORBELL_SET (ALP_V2N_MHU_B_CM33_BASE + ALP_V2N_DOORBELL_SET_OFF)

static uint32_t heartbeat_count;

static void heartbeat_expiry(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	heartbeat_count++;
	RSCTBL_BEACON->heartbeat = heartbeat_count;
	barrier_dsync_fence_full();
}

K_TIMER_DEFINE(heartbeat_timer, heartbeat_expiry, NULL);

void alp_v2n_beacon_start(void)
{
	RSCTBL_BEACON->attach_epoch = 0U;
	RSCTBL_BEACON->version      = RSCTBL_BEACON_VERSION;
	/* Magic last: the A55 trusts epoch + version only once it reads the magic. */
	barrier_dsync_fence_full();
	RSCTBL_BEACON->magic = ALP_AMP_BEACON_MAGIC;
	barrier_dsync_fence_full();
	k_timer_start(&heartbeat_timer, K_SECONDS(1), K_SECONDS(1));
}

void alp_v2n_beacon_epoch_bump(void)
{
	barrier_dsync_fence_full();
	RSCTBL_BEACON->attach_epoch = RSCTBL_BEACON->attach_epoch + 1U;
	barrier_dsync_fence_full();
}

void alp_v2n_doorbell_ring(void)
{
	*(volatile uint32_t *)ALP_M33_DOORBELL_SET = 1U;
}
