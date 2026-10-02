/* SPDX-License-Identifier: Apache-2.0 */

/*
 * coreMQTT TransportInterface_t on top of <alp/cloud_transport.h>.  The return
 * conventions already match (byte count, 0 = retry, negative = error or peer
 * gone), so these are pass-throughs.
 */

#include <stddef.h>

#include "alp_cloud_transport_coremqtt.h"

static int32_t coremqtt_recv(NetworkContext_t *ctx, void *buf, size_t len)
{
	if (ctx == NULL) {
		return ALP_ERR_INVAL;
	}
	return alp_cloud_transport_recv(ctx->transport, buf, len);
}

static int32_t coremqtt_send(NetworkContext_t *ctx, const void *buf, size_t len)
{
	if (ctx == NULL) {
		return ALP_ERR_INVAL;
	}
	return alp_cloud_transport_send(ctx->transport, buf, len);
}

void alp_cloud_transport_coremqtt(TransportInterface_t  *iface,
                                  NetworkContext_t      *ctx,
                                  alp_cloud_transport_t *t)
{
	if (iface == NULL || ctx == NULL) {
		return;
	}
	ctx->transport         = t;
	iface->recv            = coremqtt_recv;
	iface->send            = coremqtt_send;
	iface->writev          = NULL;
	iface->pNetworkContext = ctx;
}
