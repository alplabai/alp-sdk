/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * mproc-mailbox -- M55-HP <-> M55-HE shared-memory mailbox
 * roundtrip on AEN (Alif Ensemble dual-Cortex-M55 silicon).
 *
 * The pattern this example shows
 * ==============================
 *
 * Two Cortex-M55 cores share a small region of cache-coherent
 * SRAM + a hardware mailbox + a hardware semaphore.  An app on
 * the M55-HP (the "application" core) sends a payload to the
 * M55-HE (the "high-efficiency" peer) and waits for a reply.
 *
 *   1. HP owns the application: Wi-Fi, MQTT, UI.
 *   2. HE owns offloadable work: PDM/audio DSP, slow sensor
 *      polling, inference pre/post-processing.
 *   3. Mailbox + shared memory are how HP hands work to HE and
 *      collects results.
 *
 * The SDK exposes this through <alp/mproc.h>.  Same API on AEN
 * (HP<->HE) and on V2N (Cortex-A55 <-> Cortex-M33); the
 * backend dispatches to the silicon's hardware mailbox.
 *
 * What you'll see in this example
 * ===============================
 *
 *   [mproc] init mbox + shmem
 *   [mproc] sending payload  "hello-from-HP" (13 bytes)
 *   [mproc] HE woke up, payload visible in shmem
 *   [mproc] HE replied       "echo: hello-from-HP" (19 bytes)
 *   [mproc] HP read reply OK
 *   [mproc] done
 *
 * Under native_sim there's no peer core; the example exercises
 * just the HP-side init + the framing-encode path + `done`.
 * Real-silicon runs both halves.
 *
 * Peer firmware
 * =============
 *
 * The HE-side application lives at
 * `examples/mproc-mailbox/peer/main.c` (TBD; gates on the v0.4
 * dual-image build flow landing in alplabai/alp-zephyr-modules).
 * For now, this file is the HP side only; the HE side comes
 * online with the v0.4 build flow.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "alp/mproc.h"

#define MBOX_ID         0u                  /* the AEN HP<->HE hardware mailbox */
#define SHMEM_REGION_ID 0u                  /* DT-defined coherent SRAM slice */
#define PAYLOAD_SIZE    32u                 /* fits comfortably in the mbox + slice */

static const char *PAYLOAD = "hello-from-HP";

int main(void)
{
    printf("[mproc] init mbox + shmem\n");

    alp_shmem_t *shmem = alp_shmem_open(SHMEM_REGION_ID);
    alp_mbox_t  *mbox  = alp_mbox_open(MBOX_ID);
    if (shmem == NULL || mbox == NULL) {
        printf("[mproc]   open failed: last_err=%d\n", (int)alp_last_error());
        goto done;
    }

    /* Stage the payload in shared memory.  Cache-flush is the
     * backend's responsibility -- alp_shmem_write_at handles
     * coherency on its way out. */
    const size_t payload_len = strlen(PAYLOAD);
    if (alp_shmem_write_at(shmem, /*offset*/ 0u,
                           PAYLOAD, payload_len) != ALP_OK) {
        printf("[mproc]   shmem write failed\n");
        goto teardown;
    }

    /* Send the mailbox notification.  Payload here is the
     * `(offset, length)` tuple that points at the staged bytes
     * in shmem; the peer reads the actual data through its own
     * shmem handle. */
    uint8_t mbox_msg[8] = {
        0, 0, 0, 0,                         /* offset (LE u32) */
        (uint8_t)(payload_len), 0, 0, 0,    /* length (LE u32) */
    };
    if (alp_mbox_send(mbox, mbox_msg, sizeof(mbox_msg)) != ALP_OK) {
        printf("[mproc]   mbox send failed\n");
        goto teardown;
    }
    printf("[mproc] sending payload  \"%s\" (%u bytes)\n",
           PAYLOAD, (unsigned)payload_len);

    /* Wait for the peer's reply.  On real silicon the peer
     * appends a prefix and writes back to shmem then signals
     * via the same mbox.  On native_sim there's no peer; we
     * skip the wait and exit cleanly. */
#ifdef CONFIG_BOARD_NATIVE_SIM
    printf("[mproc]   native_sim: no peer core; skipping reply\n");
#else
    uint8_t reply[8];
    size_t  reply_len = 0u;
    if (alp_mbox_recv(mbox, reply, sizeof(reply), &reply_len,
                      /*timeout_ms*/ 1000u) != ALP_OK) {
        printf("[mproc]   reply timeout (peer not running?)\n");
        goto teardown;
    }
    printf("[mproc] HE woke up, payload visible in shmem\n");

    /* Reply tuple says where the echo string landed. */
    const uint32_t reply_off = (uint32_t)reply[0] |
                               ((uint32_t)reply[1] << 8) |
                               ((uint32_t)reply[2] << 16) |
                               ((uint32_t)reply[3] << 24);
    const uint32_t reply_len_bytes = (uint32_t)reply[4] |
                                     ((uint32_t)reply[5] << 8) |
                                     ((uint32_t)reply[6] << 16) |
                                     ((uint32_t)reply[7] << 24);
    char reply_buf[64];
    if (reply_len_bytes >= sizeof(reply_buf)) reply_len_bytes = sizeof(reply_buf) - 1;
    alp_shmem_read_at(shmem, reply_off, reply_buf, reply_len_bytes);
    reply_buf[reply_len_bytes] = '\0';
    printf("[mproc] HE replied       \"%s\" (%u bytes)\n",
           reply_buf, (unsigned)reply_len_bytes);
    printf("[mproc] HP read reply OK\n");
#endif

teardown:
    alp_mbox_close(mbox);
    alp_shmem_close(shmem);

done:
    printf("[mproc] done\n");
    return 0;
}
