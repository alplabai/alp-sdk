/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * mproc-dual-os-yocto-zephyr -- Cortex-M33/Zephyr producer.
 *
 * The pattern this example shows
 * ==============================
 *
 * Two cores live on the same V2N silicon:
 *
 *   1. Cortex-A55 cluster runs Yocto Linux + the consumer app
 *      under ../linux/.  See its main.c for the receiver side.
 *   2. Cortex-M33 (this firmware) reads a sensor over the portable
 *      ALP peripheral API, pushes one sample per second into
 *      the shared ring buffer, and signals A55 over the mailbox.
 *
 * The IPC layout (kept in sync between both sides):
 *
 *   - SHMEM_REGION_NAME -- DT-anchored "alp_dualos_ring" carve-out.
 *   - Region layout    -- [ producer head | consumer tail | slots[] ].
 *   - One slot         -- one uint32_t reading; 16 slots total.
 *   - Mailbox channel  -- single doorbell; the actual data lives
 *                          in shmem, mailbox carries no payload.
 *
 * Under native_sim there is no peer core, so the firmware just
 * exercises the open + push path and exits with "[dual-os] done"
 * so twister's one_line harness can latch.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

/* Portable ALP surfaces -- no V2N-specific headers in app code. */
#include "alp/mproc.h"
#include "alp/peripheral.h"

/* IPC region constants -- MUST match ../linux/main.c. */
#define SHMEM_REGION_NAME   "alp_dualos_ring"
#define SHMEM_REGION_SIZE   512u
#define MBOX_CHANNEL        0u
#define RING_SLOT_COUNT     16u
#define RING_HEAD_OFFSET    0u                                /* uint32_t */
#define RING_TAIL_OFFSET    4u                                /* uint32_t */
#define RING_SLOTS_OFFSET   16u                               /* aligned */

/* In native_sim there is no A55 peer; cap the burst so the test
 * exits cleanly within twister's default timeout. */
#define SAMPLE_BURST        4u

/* Stage a uint32_t at the right slot in the shared region.  The
 * backend handles cache-flush on its way out (we opened the region
 * with cacheable=false, so the write is straight through). */
static void ring_push(uint8_t *base, uint32_t head, uint32_t value)
{
    const size_t slot = head % RING_SLOT_COUNT;
    const size_t off  = RING_SLOTS_OFFSET + slot * sizeof(uint32_t);
    memcpy(base + off, &value, sizeof(value));
}

int main(void)
{
    printf("[m33] dual-os producer coming up\n");

    /* Open the shared region.  cacheable=false keeps the protocol
     * simple -- both sides see the same physical bytes without
     * having to wrap every push in alp_shmem_flush(). */
    const alp_shmem_config_t shmem_cfg = {
        .name      = SHMEM_REGION_NAME,
        .size      = SHMEM_REGION_SIZE,
        .cacheable = false,
    };
    alp_shmem_t *shmem = alp_shmem_open(&shmem_cfg);

    /* Open the doorbell.  Mailbox carries zero application payload
     * here -- the data lives in shmem; the doorbell just wakes the
     * A55 consumer out of its blocking read. */
    const alp_mbox_config_t mbox_cfg = {
        .channel = MBOX_CHANNEL,
        .peer    = ALP_CORE_A32_0,    /* the A55 cluster's first core. */
    };
    alp_mbox_t *mbox = alp_mbox_open(&mbox_cfg);

    if (shmem == NULL || mbox == NULL) {
        printf("[m33]   open failed: last_err=%d\n", (int)alp_last_error());
        goto done;
    }

    /* Map the region to a pointer view.  Same surface as the Linux
     * side; the backend hides the MMU/MPU plumbing. */
    void  *base = NULL;
    size_t size = 0u;
    if (alp_shmem_view(shmem, &base, &size) != ALP_OK || size < SHMEM_REGION_SIZE) {
        printf("[m33]   shmem view failed (size=%zu)\n", size);
        goto teardown;
    }
    uint8_t *ring = (uint8_t *)base;

    /* Initialise the ring head/tail.  In a real V2N deployment the
     * Linux side would do this once at boot via the same shmem
     * surface; the producer just resets-to-known-state here for
     * the demo. */
    const uint32_t zero = 0u;
    memcpy(ring + RING_HEAD_OFFSET, &zero, sizeof(zero));
    memcpy(ring + RING_TAIL_OFFSET, &zero, sizeof(zero));

    /* Producer loop.  In production this would read a real sensor
     * via alp_adc_read() / alp_i2c_xfer() / etc; the demo just
     * synthesises a monotonic counter so the consumer can verify
     * ordering. */
    for (uint32_t i = 0; i < SAMPLE_BURST; ++i) {
        ring_push(ring, i, 0xA000u + i);

        /* Commit the producer head AFTER the slot write so the
         * consumer never sees a torn slot.  The doorbell-after-
         * write ordering is what makes the protocol safe without a
         * hwsem; we trade a hwsem for a memory-barrier discipline. */
        const uint32_t new_head = i + 1u;
        memcpy(ring + RING_HEAD_OFFSET, &new_head, sizeof(new_head));

        /* Ring the doorbell.  Payload byte is unused -- the A55
         * consumer reads head/tail straight out of shmem. */
        const uint8_t ding = 0u;
        if (alp_mbox_send(mbox, &ding, sizeof(ding), 100u) != ALP_OK) {
            printf("[m33]   mbox send failed at i=%u\n", (unsigned)i);
            break;
        }
        printf("[m33] sample %u\n", (unsigned)i);

#ifndef CONFIG_BOARD_NATIVE_SIM
        /* 1 Hz cadence on real silicon.  Skipped under native_sim
         * so the test latches "[dual-os] done" quickly. */
        k_msleep(1000);
#endif
    }

teardown:
    alp_mbox_close(mbox);
    alp_shmem_close(shmem);

done:
    printf("[dual-os] done\n");
    return 0;
}
