/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * mproc-dual-os-yocto-zephyr -- Cortex-A55 / Yocto-Linux consumer.
 *
 * Sister to ../m33/main.c on the same V2N silicon.  The M33 firmware
 * is the producer; this binary runs in user space under Yocto Linux
 * on the A55 cluster and is the consumer.
 *
 * Same <alp/mproc.h> surface as the M33 firmware.  Under Yocto the
 * SDK's mproc backend is the user-space variant -- mailbox is wired
 * to the Linux mailbox-controller character device (under the hood
 * /dev/mhuN or rpmsg), and the shared region is exposed by
 * meta-alp-sdk's device tree as a reserved memory carve-out the
 * runtime mmap()s into the user-space process.
 *
 * Build: this file is plain CMake (no Zephyr); meta-alp-sdk's
 * recipes-apps/mproc-dual-os recipe drives the build inside the
 * bitbake image.  Standalone the binary builds against the host
 * SDK's sysroot via:
 *
 *   source /opt/alp-sdk/.../environment-setup-cortexa55-...
 *   cmake -S linux -B build
 *   cmake --build build
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

/* The Linux/Yocto build of the ALP SDK exports the same headers as
 * the Zephyr build -- only the backend differs.  See README.md. */
#include "alp/mproc.h"
#include "alp/peripheral.h"

/* IPC region constants -- MUST match ../m33/main.c. */
#define SHMEM_REGION_NAME   "alp_dualos_ring"
#define SHMEM_REGION_SIZE   512u
#define MBOX_CHANNEL        0u
#define RING_SLOT_COUNT     16u
#define RING_HEAD_OFFSET    0u
#define RING_TAIL_OFFSET    4u
#define RING_SLOTS_OFFSET   16u

/* Drain budget -- this demo binary exits once it has seen this
 * many samples; production code would loop forever. */
#define DRAIN_COUNT         4u

/* Pull a uint32_t out of the ring slot pointed at by `tail`. */
static uint32_t ring_pop(const uint8_t *base, uint32_t tail)
{
    const size_t slot = tail % RING_SLOT_COUNT;
    const size_t off  = RING_SLOTS_OFFSET + slot * sizeof(uint32_t);
    uint32_t v;
    memcpy(&v, base + off, sizeof(v));
    return v;
}

/* Doorbell callback.  The M33 producer signals with a zero-byte
 * mailbox message after each push; we just nudge the foreground
 * loop -- the actual data lives in shmem. */
static volatile int g_dingdong;

static void on_doorbell(uint32_t channel, const void *data, size_t len, void *user)
{
    (void)channel;
    (void)data;
    (void)len;
    (void)user;
    g_dingdong = 1;
}

int main(void)
{
    printf("[a55] dual-os consumer coming up\n");

    /* Open the same shared region as the M33 firmware.  Yocto's
     * mproc backend maps the SoC-reserved DT carve-out into this
     * user-space process's address space. */
    const alp_shmem_config_t shmem_cfg = {
        .name      = SHMEM_REGION_NAME,
        .size      = SHMEM_REGION_SIZE,
        .cacheable = false,
    };
    alp_shmem_t *shmem = alp_shmem_open(&shmem_cfg);

    /* Open the doorbell.  Peer is the M33 -- the SDK's user-space
     * mproc backend resolves that to the matching Linux mailbox
     * controller channel. */
    const alp_mbox_config_t mbox_cfg = {
        .channel = MBOX_CHANNEL,
        .peer    = ALP_CORE_SELF,    /* SELF == the local A55; the SDK fills in the peer. */
    };
    alp_mbox_t *mbox = alp_mbox_open(&mbox_cfg);

    if (shmem == NULL || mbox == NULL) {
        printf("[a55]   open failed: last_err=%d\n", (int)alp_last_error());
        return 1;
    }

    /* View the region.  Same single-pointer surface as the M33
     * side -- the user-space backend hides the mmap() internally. */
    void  *base = NULL;
    size_t size = 0u;
    if (alp_shmem_view(shmem, &base, &size) != ALP_OK) {
        printf("[a55]   shmem view failed\n");
        return 1;
    }
    const uint8_t *ring = (const uint8_t *)base;

    /* Attach the doorbell callback.  Runs on the SDK's mbox thread
     * (background thread under Yocto). */
    if (alp_mbox_set_callback(mbox, on_doorbell, NULL) != ALP_OK) {
        printf("[a55]   set_callback failed\n");
        return 1;
    }

    /* Consumer loop -- read producer head, drain through our local
     * tail, post the updated tail back so the producer can wrap. */
    uint32_t tail = 0u;
    for (uint32_t drained = 0; drained < DRAIN_COUNT; ) {
        uint32_t head;
        memcpy(&head, ring + RING_HEAD_OFFSET, sizeof(head));

        while (tail < head && drained < DRAIN_COUNT) {
            const uint32_t v = ring_pop(ring, tail);
            printf("[a55] sample %u value=0x%04x\n",
                   (unsigned)drained, (unsigned)v);
            tail += 1u;
            drained += 1u;
        }

        /* Publish our progress back -- producer reads RING_TAIL_OFFSET
         * to know when slots become available again. */
        memcpy((uint8_t *)base + RING_TAIL_OFFSET, &tail, sizeof(tail));

        if (drained < DRAIN_COUNT) {
            /* Wait for the next doorbell.  100 ms poll is fine for
             * the demo; a real consumer would use alp_mbox_recv()
             * blocking variant once that lands. */
            g_dingdong = 0;
            for (int i = 0; i < 50 && !g_dingdong; ++i) {
                usleep(100 * 1000);
            }
        }
    }

    alp_mbox_close(mbox);
    alp_shmem_close(shmem);
    printf("[dual-os] done\n");
    return 0;
}
