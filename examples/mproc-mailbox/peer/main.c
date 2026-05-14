/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * mproc-mailbox -- M55-HE side of the dual-core mailbox roundtrip.
 * Sister to ../src/main.c (the HP-side).
 *
 * The pattern this peer shows
 * ===========================
 *
 * The HP-side application code stages a payload in shared memory
 * and signals this peer through a hardware mailbox.  This peer:
 *
 *   1. Waits on the same mbox (blocking).
 *   2. Reads the (offset, length) tuple the HP sent.
 *   3. Reads the payload from shared memory.
 *   4. Builds an echo response ("echo: " + original payload).
 *   5. Writes the echo back to a different shmem offset.
 *   6. Sends a reply via mbox with the response (offset, length).
 *
 * The peer runs forever -- one HP request triggers one reply
 * cycle, then it loops back to wait for the next.
 *
 * Build status (v1.0 prep)
 * ========================
 *
 * This file compiles standalone but only LINKS as a real image
 * once the v0.4 dual-image build flow lands in
 * `alplabai/alp-zephyr-modules`.  That flow defines:
 *
 *   - The HE-side board configuration (`alif_e7_dk_rtss_he`).
 *   - Sysbuild glue that produces TWO images per `west build` --
 *     the HP application + this peer.
 *   - The flasher that programs both into the matching SoC
 *     partitions.
 *
 * Until the dual-image flow lands, this file is BUILD-ONLY under
 * `west build -b alif_e7_dk_rtss_he examples/mproc-mailbox/peer`
 * (single-target manual invocation) and the testcase.yaml entry
 * marks it `build_only: true`.  Once dual-image is on, sysbuild
 * picks this up automatically.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "alp/mproc.h"

#define MBOX_ID         0u  /* same id the HP opens -- the mbox is bidirectional */
#define SHMEM_REGION_ID 0u  /* same id the HP opens -- the slice is shared       */

#define REQUEST_OFFSET  0u   /* where HP stages outbound payload */
#define RESPONSE_OFFSET 256u /* where HE stages echo response    */
#define ECHO_PREFIX     "echo: "
#define MAX_PAYLOAD     128u /* fits comfortably below RESPONSE_OFFSET */

int main(void)
{
    printf("[mproc-peer] HE side coming up\n");

    alp_shmem_t *shmem = alp_shmem_open(SHMEM_REGION_ID);
    alp_mbox_t  *mbox  = alp_mbox_open(MBOX_ID);
    if (shmem == NULL || mbox == NULL) {
        printf("[mproc-peer]   open failed: last_err=%d\n", (int)alp_last_error());
        return 1;
    }

    printf("[mproc-peer] waiting on mbox id=%u shmem id=%u\n",
           (unsigned)MBOX_ID, (unsigned)SHMEM_REGION_ID);

    /* Steady-state echo loop.  One HP request -> one HE reply, then
     * back to the mbox wait.  Designed to be re-entered indefinitely
     * with no GC pressure (every buffer is stack- or .bss-allocated). */
    for (;;) {
        /* Wait for the HP to signal.  The mbox payload is the
         * (offset, length) tuple naming where the request lives in
         * shared memory.  Blocking with no timeout so the peer
         * idles in WFI between requests. */
        uint8_t      req_tuple[8] = { 0 };
        size_t       req_tuple_len = 0u;
        const alp_status_t rs = alp_mbox_recv(mbox, req_tuple,
                                              sizeof(req_tuple),
                                              &req_tuple_len,
                                              /* timeout_ms */ UINT32_MAX);
        if (rs != ALP_OK) {
            printf("[mproc-peer]   mbox_recv error %d -- retrying\n", (int)rs);
            continue;
        }
        if (req_tuple_len < 8u) {
            printf("[mproc-peer]   undersized tuple (%u bytes) -- dropping\n",
                   (unsigned)req_tuple_len);
            continue;
        }

        const uint32_t req_off = (uint32_t)req_tuple[0] |
                                 ((uint32_t)req_tuple[1] << 8) |
                                 ((uint32_t)req_tuple[2] << 16) |
                                 ((uint32_t)req_tuple[3] << 24);
        uint32_t req_len = (uint32_t)req_tuple[4] |
                           ((uint32_t)req_tuple[5] << 8) |
                           ((uint32_t)req_tuple[6] << 16) |
                           ((uint32_t)req_tuple[7] << 24);
        if (req_len > MAX_PAYLOAD) {
            /* Don't trust the producer to bound this -- clamp so a
             * stale or hostile request can't overrun our stack
             * buffer. */
            req_len = MAX_PAYLOAD;
        }
        printf("[mproc-peer] request offset=%u len=%u\n",
               (unsigned)req_off, (unsigned)req_len);

        /* Pull the actual payload bytes out of shared memory.
         * Backend handles cache-invalidate so the HP's stale
         * write-buffer can't leak into the read. */
        char request_buf[MAX_PAYLOAD + 1] = { 0 };
        if (req_len > 0u) {
            const alp_status_t ss = alp_shmem_read_at(shmem,
                                                     req_off,
                                                     request_buf,
                                                     req_len);
            if (ss != ALP_OK) {
                printf("[mproc-peer]   shmem read failed %d\n", (int)ss);
                continue;
            }
        }
        request_buf[req_len] = '\0';
        printf("[mproc-peer] payload  \"%s\"\n", request_buf);

        /* Build the echo response.  Truncate if the prefix + payload
         * would exceed MAX_PAYLOAD; the protocol is a demo so
         * silent-truncate is acceptable. */
        char response_buf[MAX_PAYLOAD + sizeof(ECHO_PREFIX)] = { 0 };
        const size_t prefix_len = sizeof(ECHO_PREFIX) - 1u;
        memcpy(response_buf, ECHO_PREFIX, prefix_len);
        size_t response_len = prefix_len + req_len;
        if (response_len > MAX_PAYLOAD) response_len = MAX_PAYLOAD;
        memcpy(response_buf + prefix_len, request_buf, response_len - prefix_len);

        /* Stage the response in shmem.  Cache-flush on the way out
         * is the backend's responsibility. */
        if (alp_shmem_write_at(shmem, RESPONSE_OFFSET,
                               response_buf, response_len) != ALP_OK) {
            printf("[mproc-peer]   shmem write failed\n");
            continue;
        }

        /* Signal the HP -- mbox payload mirrors the request shape:
         * (offset, length) tuple pointing at the staged response. */
        const uint8_t reply_tuple[8] = {
            (uint8_t)(RESPONSE_OFFSET & 0xFFu),
            (uint8_t)((RESPONSE_OFFSET >> 8)  & 0xFFu),
            (uint8_t)((RESPONSE_OFFSET >> 16) & 0xFFu),
            (uint8_t)((RESPONSE_OFFSET >> 24) & 0xFFu),
            (uint8_t)(response_len & 0xFFu),
            (uint8_t)((response_len >> 8)  & 0xFFu),
            (uint8_t)((response_len >> 16) & 0xFFu),
            (uint8_t)((response_len >> 24) & 0xFFu),
        };
        if (alp_mbox_send(mbox, reply_tuple, sizeof(reply_tuple)) != ALP_OK) {
            printf("[mproc-peer]   mbox send failed\n");
            continue;
        }
        printf("[mproc-peer] replied \"%s\" (%u bytes)\n",
               response_buf, (unsigned)response_len);
    }

    /* Unreachable -- the loop never exits.  The teardown below is
     * dead code but kept for documentation of the close sequence
     * a non-loop variant of this peer would use. */
    alp_mbox_close(mbox);
    alp_shmem_close(shmem);
    return 0;
}
