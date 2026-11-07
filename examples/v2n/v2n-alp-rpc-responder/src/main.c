/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-alp-rpc-responder -- the E1M-V2N101 / V2M101 Cortex-M33 as a plain
 * <alp/rpc.h> responder for the Linux A55.
 *
 * The A55 opens the same channel with alp_rpc_open() (its backend,
 * src/backends/rpc/yocto_uio_drv.c, needs the Alp AMP beacon this CM33 image
 * publishes from boot) and calls "echo"; this side answers by sending the
 * payload back on the same method -- the "peer replies with the same method"
 * convention documented on alp_rpc_call() in <alp/rpc.h>.
 *
 * Nothing here touches OpenAMP, the MHU or the resource table: the ipc_service
 * backend in zephyr/drivers/ipc/ipc_alp_v2n_rpmsg.c owns all of that.  The A55
 * must name the CM33 endpoint address explicitly (alp_rpc_config_t::dst_ept =
 * 1024, the first endpoint registered here).
 *
 * BENCH-UNVERIFIED through <alp/rpc.h>; the transport underneath is the one
 * examples/multicore/rpmsg-v2n proved on silicon.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <alp/rpc.h>

/* Channel identity: the A55 opens the same name. */
#define RPC_CHANNEL "alp_v2n_echo"
#define METHOD_ECHO "echo"

static alp_rpc_channel_t *g_ch;

/* Runs on the backend's manager thread: keep it short.  The payload buffer is
 * only valid for the duration of the callback, and alp_rpc_send() copies it
 * into an rpmsg TX buffer before returning. */
static void on_echo(const void *payload, size_t len, void *user)
{
	ARG_UNUSED(user);
	(void)alp_rpc_send(g_ch, METHOD_ECHO, payload, len);
}

int main(void)
{
	g_ch = alp_rpc_open(&(alp_rpc_config_t){ .name = RPC_CHANNEL });
	if (g_ch == NULL) {
		printk("v2n-alp-rpc-responder: alp_rpc_open failed: %d\n", (int)alp_last_error());
		return -1;
	}
	if (alp_rpc_subscribe(g_ch, METHOD_ECHO, on_echo, NULL) != ALP_OK) {
		printk("v2n-alp-rpc-responder: alp_rpc_subscribe failed\n");
		alp_rpc_close(g_ch);
		return -1;
	}

	/* Everything happens in the callback; the A55 decides when to attach. */
	k_sleep(K_FOREVER);
	return 0;
}
