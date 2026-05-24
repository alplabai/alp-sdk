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
 *   [mproc] HE replied via mbox callback
 *   [mproc] HE replied       "echo: hello-from-HP" (19 bytes)
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
 * `examples/multicore/mproc-mailbox/peer/main.c`.  Both halves build
 * standalone today; sysbuild glue that builds them in one
 * `west build` invocation lands alongside the v0.4 dual-image
 * flow in `alplabai/alp-zephyr-modules`.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "alp/mproc.h"
#include "alp/peripheral.h"

/* Region name resolved against the alp-shmemN DT aliases in the
 * board overlay -- here, alp-shmem0 in boards/<board>.overlay or
 * the orchestrator-emitted carve-out for AEN dual-core builds. */
#define SHMEM_REGION_NAME "alp_shmem0"
#define SHMEM_REGION_SIZE 512u
#define MBOX_CHANNEL      0u
#define REQUEST_OFFSET    0u
#define RESPONSE_OFFSET   256u  /* gap from request so the two never overlap */
#define MAX_PAYLOAD       128u

static const char *PAYLOAD = "hello-from-HP";

static struct {
    volatile bool   got_reply;
    volatile size_t reply_offset;
    volatile size_t reply_len;
} g_state;

/* Mailbox inbound-message callback.  Fires when the HE peer
 * signals back with the (offset, length) tuple naming the
 * staged echo response.  Runs on the SDK's mbox thread; we
 * just stash the values so main() can drain them. */
static void mbox_inbound(uint32_t channel, const void *data, size_t len, void *user)
{
    (void)channel;
    (void)user;
    if (len < 8u) return;
    const uint8_t *b = data;
    g_state.reply_offset = (size_t)b[0] | ((size_t)b[1] << 8) |
                           ((size_t)b[2] << 16) | ((size_t)b[3] << 24);
    g_state.reply_len    = (size_t)b[4] | ((size_t)b[5] << 8) |
                           ((size_t)b[6] << 16) | ((size_t)b[7] << 24);
    g_state.got_reply    = true;
}

int main(void)
{
    printf("[mproc] init mbox + shmem\n");

    const alp_shmem_config_t shmem_cfg = {
        .name      = SHMEM_REGION_NAME,
        .size      = SHMEM_REGION_SIZE,
        .cacheable = false,
    };
    alp_shmem_t *shmem = alp_shmem_open(&shmem_cfg);

    const alp_mbox_config_t mbox_cfg = {
        .channel = MBOX_CHANNEL,
        .peer    = ALP_CORE_M55_HE,
    };
    alp_mbox_t  *mbox  = alp_mbox_open(&mbox_cfg);

    if (shmem == NULL || mbox == NULL) {
        printf("[mproc]   open failed: last_err=%d\n", (int)alp_last_error());
        goto done;
    }

    /* Get a pointer view of the shared region.  Both cores see
     * the same physical bytes; cache flush on the producer side
     * is the backend's responsibility (cacheable=false above). */
    void  *shmem_base = NULL;
    size_t shmem_size = 0u;
    if (alp_shmem_view(shmem, &shmem_base, &shmem_size) != ALP_OK) {
        printf("[mproc]   shmem view failed\n");
        goto teardown;
    }
    if (shmem_size < RESPONSE_OFFSET + MAX_PAYLOAD) {
        printf("[mproc]   shmem too small (%zu)\n", shmem_size);
        goto teardown;
    }

    /* Stage the payload at REQUEST_OFFSET in the shared region.
     * memcpy is the right primitive here -- the SDK's shmem
     * surface gives back a raw pointer + size and trusts the
     * caller to write through it. */
    const size_t payload_len = strlen(PAYLOAD);
    memcpy((uint8_t *)shmem_base + REQUEST_OFFSET, PAYLOAD, payload_len);

    /* Send the mailbox notification.  Payload here is the
     * `(offset, length)` tuple that points at the staged bytes
     * in shmem; the peer reads the actual data through its own
     * shmem view. */
    uint8_t mbox_msg[8] = {
        (uint8_t)REQUEST_OFFSET, 0, 0, 0,            /* offset (LE u32) */
        (uint8_t)payload_len,    0, 0, 0,            /* length (LE u32) */
    };
    if (alp_mbox_send(mbox, mbox_msg, sizeof(mbox_msg), 100u) != ALP_OK) {
        printf("[mproc]   mbox send failed\n");
        goto teardown;
    }
    printf("[mproc] sending payload  \"%s\" (%u bytes)\n", PAYLOAD, (unsigned)payload_len);

    /* Register the receive callback before waiting.  On real
     * silicon the peer sends back via the same mbox; the
     * callback drains the (offset, length) tuple naming the
     * echo response. */
    if (alp_mbox_set_callback(mbox, mbox_inbound, NULL) != ALP_OK) {
        printf("[mproc]   set_callback failed\n");
        goto teardown;
    }

#ifdef CONFIG_BOARD_NATIVE_SIM
    printf("[mproc]   native_sim: no peer core; skipping reply wait\n");
#else
    /* Poll for the reply.  On HiL the mailbox-ISR-driven
     * callback flips g_state.got_reply within ~1 ms of the
     * peer's send. */
    for (int i = 0; i < 100 && !g_state.got_reply; ++i) {
        k_msleep(10);
    }
    if (!g_state.got_reply) {
        printf("[mproc]   reply timeout (peer not running?)\n");
        goto teardown;
    }
    printf("[mproc] HE replied via mbox callback\n");

    if (g_state.reply_len > MAX_PAYLOAD) g_state.reply_len = MAX_PAYLOAD;
    char reply_buf[MAX_PAYLOAD + 1];
    memcpy(reply_buf, (uint8_t *)shmem_base + g_state.reply_offset, g_state.reply_len);
    reply_buf[g_state.reply_len] = '\0';
    printf("[mproc] HE replied       \"%s\" (%u bytes)\n", reply_buf, (unsigned)g_state.reply_len);
#endif

teardown:
    alp_mbox_close(mbox);
    alp_shmem_close(shmem);

done:
    printf("[mproc] done\n");
    return 0;
}
