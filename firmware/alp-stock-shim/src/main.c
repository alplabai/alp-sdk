/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Liveness beacon for the RZ/V2N / RZ/V2M CM33 stock image.
 *
 * The CM33 has no console on these SoMs (sci0 must stay disabled: a floating
 * RXD faults the core before main), so the only way to tell from Linux that
 * this core is running is memory.  The shim writes the liveness beacon into
 * the last 16 bytes of the `rsctbl` page -- the layout shared with the rpmsg-v2n
 * example and the A55 backend (<alp/protocol/amp_beacon.h>), so the A55 side
 * reads one format.  Linux sees it at the rsctbl page's A55
 * alias (see README.md; the page is the SoC metadata's `openamp_carveout`).
 *
 * Plain stores only.  No peripheral, no interrupt, no IPC (no MHU, no sci0,
 * no RIIC8, no port 9 / GD32 SPI, no DMAC).  The window is the board DTS's
 * `openamp_shm` reservation, which the A55 DT keeps `no-map`, so Linux never
 * hands it out.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>

#include <alp/protocol/amp_beacon.h>

/* Only the V2N / V2M CM33 boards have the `rsctbl` window; every other core
 * (AEN M55) gets the plain idle loop below. */
#if DT_NODE_EXISTS(DT_NODELABEL(rsctbl))
/* The beacon is the top 16 bytes of the rsctbl page, the same layout the RPC
 * firmware (examples/multicore/rpmsg-v2n/m33_sm) and the A55 backend use:
 * <alp/protocol/amp_beacon.h>.  The page address and size come from the board
 * .dts, generated from the SoC metadata's `openamp_carveout`.  This image
 * publishes the idle-shim version (no RPC); the attach epoch is zeroed once at boot
 * (clearing a stale odd value from a previous image) and never written again. */
#define RSCTBL_BASE DT_REG_ADDR(DT_NODELABEL(rsctbl))
#define RSCTBL_SIZE DT_REG_SIZE(DT_NODELABEL(rsctbl))
#define BEACON      ALP_AMP_BEACON_AT(RSCTBL_BASE, RSCTBL_SIZE)
#endif /* DT_NODE_EXISTS(rsctbl) */

int main(void)
{
#if DT_NODE_EXISTS(DT_NODELABEL(rsctbl))
	uint32_t count = 0;

	/* The window keeps its contents across a CM33 reset: clear the counter
	 * and publish the magic last, so a reader never pairs a fresh magic
	 * with a stale count. */
	BEACON->heartbeat    = 0;
	BEACON->attach_epoch = 0;
	BEACON->version      = ALP_AMP_BEACON_VERSION_IDLE_SHIM;
	barrier_dsync_fence_full();
	BEACON->magic = ALP_AMP_BEACON_MAGIC;
	barrier_dsync_fence_full();

	while (1) {
		k_sleep(K_SECONDS(1));
		BEACON->heartbeat = ++count;
		barrier_dsync_fence_full();
	}
#else
	while (1) {
		k_sleep(K_FOREVER);
	}
#endif

	return 0;
}
