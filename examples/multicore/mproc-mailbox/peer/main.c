/*
 * Copyright 2026 Alp Lab AB
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
 *   1. Opens the same mbox + shmem region.
 *   2. Registers an mbox callback that fires on each HP send.
 *   3. In the callback: reads the (offset, length) tuple, pulls
 *      the payload out of shared memory, builds an echo
 *      response, writes it back at a different shmem offset,
 *      and signals the HP via mbox.
 *
 * The peer runs forever in a low-power idle loop -- the mbox
 * callback fires from the SDK's mailbox thread.
 *
 * Build status (v1.0 prep)
 * ========================
 *
 * Compiles standalone today.  Sysbuild glue that builds it
 * alongside the HP image in one `west build` invocation lands
 * with the v0.4 dual-image flow in
 * `alplabai/alp-zephyr-modules`.  Until then customers run two
 * `west build` invocations -- one for HP, one for HE -- and
 * flash both partitions.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "alp/mproc.h"
#include "alp/peripheral.h"

/* Region name resolved against the alp-shmemN DT aliases.  Must
 * match the HP side; both peers see the same physical bytes via
 * the alp-shmem0 alias. */
#define SHMEM_REGION_NAME "alp_shmem0"
#define SHMEM_REGION_SIZE 512u
#define MBOX_CHANNEL 0u
#define RESPONSE_OFFSET 256u /* must match HP's expectation */
#define ECHO_PREFIX "echo: "
#define MAX_PAYLOAD 128u

static struct {
	alp_shmem_t *shmem;
	alp_mbox_t  *mbox;
	void        *shmem_base;
	size_t       shmem_size;
} g_peer;

/* Mailbox inbound-message callback.  Fires when the HP signals
 * via the shared mbox channel.  Pulls the request out of
 * shared memory, builds the echo response, stages it back,
 * signals via mbox.  Runs on the SDK's mailbox thread. */
static void mbox_inbound(uint32_t channel, const void *data, size_t len, void *user)
{
	(void)channel;
	(void)user;
	if (len < 8u || g_peer.shmem_base == NULL) {
		printf("[mproc-peer]   undersized tuple (%zu bytes) -- dropping\n", len);
		return;
	}

	const uint8_t *b = data;
	const size_t   offset =
	    (size_t)b[0] | ((size_t)b[1] << 8) | ((size_t)b[2] << 16) | ((size_t)b[3] << 24);
	size_t req_len =
	    (size_t)b[4] | ((size_t)b[5] << 8) | ((size_t)b[6] << 16) | ((size_t)b[7] << 24);
	if (req_len > MAX_PAYLOAD) req_len = MAX_PAYLOAD;
	if (offset + req_len > g_peer.shmem_size) return;

	printf("[mproc-peer] request offset=%u len=%u\n", (unsigned)offset, (unsigned)req_len);

	/* Pull the request bytes out of shared memory.  Backend
     * handles cache-invalidate so the HP's stale write-buffer
     * can't leak into the read (cacheable=false at open). */
	char request_buf[MAX_PAYLOAD + 1] = { 0 };
	memcpy(request_buf, (const uint8_t *)g_peer.shmem_base + offset, req_len);
	request_buf[req_len] = '\0';
	printf("[mproc-peer] payload  \"%s\"\n", request_buf);

	/* Build the echo response.  Truncate if prefix + payload
     * would exceed MAX_PAYLOAD; the protocol is a demo so
     * silent-truncate is acceptable. */
	char         response_buf[MAX_PAYLOAD + sizeof(ECHO_PREFIX)] = { 0 };
	const size_t prefix_len                                      = sizeof(ECHO_PREFIX) - 1u;
	memcpy(response_buf, ECHO_PREFIX, prefix_len);
	size_t response_len = prefix_len + req_len;
	if (response_len > MAX_PAYLOAD) response_len = MAX_PAYLOAD;
	memcpy(response_buf + prefix_len, request_buf, response_len - prefix_len);

	/* Stage the response at RESPONSE_OFFSET.  HP's mbox callback
     * reads back via this offset. */
	memcpy((uint8_t *)g_peer.shmem_base + RESPONSE_OFFSET, response_buf, response_len);

	/* Signal the HP -- (offset, length) tuple pointing at the
     * staged response. */
	const uint8_t reply_tuple[8] = {
		(uint8_t)(RESPONSE_OFFSET & 0xFFu),
		(uint8_t)((RESPONSE_OFFSET >> 8) & 0xFFu),
		(uint8_t)((RESPONSE_OFFSET >> 16) & 0xFFu),
		(uint8_t)((RESPONSE_OFFSET >> 24) & 0xFFu),
		(uint8_t)(response_len & 0xFFu),
		(uint8_t)((response_len >> 8) & 0xFFu),
		(uint8_t)((response_len >> 16) & 0xFFu),
		(uint8_t)((response_len >> 24) & 0xFFu),
	};
	if (alp_mbox_send(g_peer.mbox, reply_tuple, sizeof(reply_tuple), 100u) != ALP_OK) {
		printf("[mproc-peer]   mbox send failed\n");
		return;
	}
	printf("[mproc-peer] replied \"%s\" (%u bytes)\n", response_buf, (unsigned)response_len);
}

int main(void)
{
	printf("[mproc-peer] HE side coming up\n");

	const alp_shmem_config_t shmem_cfg = {
		.name      = SHMEM_REGION_NAME,
		.size      = SHMEM_REGION_SIZE,
		.cacheable = false,
	};
	g_peer.shmem                     = alp_shmem_open(&shmem_cfg);

	const alp_mbox_config_t mbox_cfg = {
		.channel = MBOX_CHANNEL,
		.peer    = ALP_CORE_M55_HP,
	};
	g_peer.mbox = alp_mbox_open(&mbox_cfg);

	if (g_peer.shmem == NULL || g_peer.mbox == NULL) {
		printf("[mproc-peer]   open failed: last_err=%d\n", (int)alp_last_error());
		return 1;
	}

	if (alp_shmem_view(g_peer.shmem, &g_peer.shmem_base, &g_peer.shmem_size) != ALP_OK) {
		printf("[mproc-peer]   shmem view failed\n");
		return 1;
	}

	if (alp_mbox_set_callback(g_peer.mbox, mbox_inbound, NULL) != ALP_OK) {
		printf("[mproc-peer]   set_callback failed\n");
		return 1;
	}

	printf("[mproc-peer] waiting on mbox channel=%u shmem=%zu bytes\n", (unsigned)MBOX_CHANNEL,
	       g_peer.shmem_size);

	/* Steady-state idle.  The mbox callback fires from the SDK
     * thread on every HP send; main just keeps the system
     * alive.  k_sleep instead of busy-wait so the core can
     * enter WFI between events. */
	for (;;) {
		k_sleep(K_FOREVER);
	}

	/* Unreachable but kept for documentation. */
	alp_mbox_close(g_peer.mbox);
	alp_shmem_close(g_peer.shmem);
	return 0;
}
