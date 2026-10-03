/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Liveness beacon for the RZ/V2N / RZ/V2M CM33 stock image.
 *
 * The CM33 has no console on these SoMs (sci0 must stay disabled: a floating
 * RXD faults the core before main), so the only way to tell from Linux that
 * this core is running is memory.  The shim writes a three-word beacon into
 * the top of the `rsctbl` window -- the same words, offsets and magic as the
 * rpmsg-v2n example, so the A55 side reads one format.  Linux sees them at
 * 0x4F700FF0 / 0x4F700FF4 / 0x4F700FF8.
 *
 * Plain stores only.  No peripheral, no interrupt, no IPC (no MHU, no sci0,
 * no RIIC8, no port 9 / GD32 SPI, no DMAC).  The window is the board DTS's
 * `openamp_shm` reservation, which the A55 DT keeps `no-map`, so Linux never
 * hands it out.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>

#define RSCTBL_ADDR DT_REG_ADDR(DT_NODELABEL(rsctbl))

#define BEACON_MAGIC (0xA10D0683U) /* "Alp Lab, #683" */
/* The word at +0xFF4 says which image is running: values below 0x100 are the RPC
 * firmware's beacon versions (1 today, 2 after #2586); 0x100 is this idle shim
 * (kind 1, revision 0, no RPC). An A55 RPC backend must not read 0x100 as an old
 * RPC firmware. */
#define BEACON_VERSION (0x100U)

/* Same layout as examples/multicore/rpmsg-v2n/m33_sm: top 16 bytes of the
 * 4 KiB resource-table page, clear of the table at the low end. */
struct beacon {
	uint32_t magic;
	uint32_t version;
	uint32_t heartbeat;
};

#define BEACON ((volatile struct beacon *)(RSCTBL_ADDR + 0xFF0))

int main(void)
{
	uint32_t count = 0;

	/* The window keeps its contents across a CM33 reset: clear the counter
	 * and publish the magic last, so a reader never pairs a fresh magic
	 * with a stale count. */
	BEACON->heartbeat = 0;
	BEACON->version   = BEACON_VERSION;
	barrier_dsync_fence_full();
	BEACON->magic = BEACON_MAGIC;
	barrier_dsync_fence_full();

	while (1) {
		k_sleep(K_SECONDS(1));
		BEACON->heartbeat = ++count;
		barrier_dsync_fence_full();
	}

	return 0;
}
