/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * RPC NOSUPPORT stubs -- <alp/rpc.h> (framed RPC over OpenAMP /
 * RPMsg).  Split out of the former src/common/stub_backend.c
 * monolith (issue #673); owns every `alp_rpc_*` symbol not provided
 * by a vendor/OS backend.
 *
 * Bare-metal has no OpenAMP transport, so <alp/rpc.h>'s own doc
 * comment calls out this exact NOSUPPORT stub as the bare-metal path.
 * Guarded (ALP_VENDOR_OVERRIDES_RPC): Yocto's rpc_dispatch.c (the
 * registry-pattern class owner, routing to backends/rpc/yocto_drv.c)
 * is compiled unconditionally in src/yocto/CMakeLists.txt and already
 * defines every alp_rpc_* symbol, so it mutes this block there to
 * avoid a duplicate-definition link error (#607 install-review build;
 * the old top-of-block comment claiming "no vendor ever overrides
 * RPC" predates this stub existing at all).
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"
#include "alp/rpc.h"

#include "stub_internal.h"

#if !defined(ALP_VENDOR_OVERRIDES_RPC)
alp_rpc_channel_t *alp_rpc_open(const alp_rpc_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
void alp_rpc_close(alp_rpc_channel_t *ch)
{
	(void)ch;
}
const alp_capabilities_t *alp_rpc_capabilities(const alp_rpc_channel_t *ch)
{
	(void)ch;
	return NULL;
}
alp_status_t
alp_rpc_subscribe(alp_rpc_channel_t *ch, const char *method, alp_rpc_method_cb_t cb, void *user)
{
	(void)ch;
	(void)method;
	(void)cb;
	(void)user;
	return ALP_ERR_NOT_READY;
}
alp_status_t alp_rpc_unsubscribe(alp_rpc_channel_t *ch, const char *method)
{
	(void)ch;
	(void)method;
	return ALP_ERR_NOT_READY;
}
alp_status_t
alp_rpc_send(alp_rpc_channel_t *ch, const char *method, const void *payload, size_t len)
{
	(void)ch;
	(void)method;
	(void)payload;
	(void)len;
	return ALP_ERR_NOT_READY;
}
alp_status_t alp_rpc_call(alp_rpc_channel_t *ch,
                          const char        *method,
                          const void        *req,
                          size_t             req_len,
                          void              *resp,
                          size_t            *resp_len,
                          uint32_t           timeout_ms)
{
	(void)ch;
	(void)method;
	(void)req;
	(void)req_len;
	(void)resp;
	(void)timeout_ms;
	if (resp_len != NULL) *resp_len = 0;
	return ALP_ERR_NOT_READY;
}
#endif /* !ALP_VENDOR_OVERRIDES_RPC */
