/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software RPC fallback.  Wildcard backend at priority 0 -- picked
 * only when no hardware backend is linked into the build
 * (native_sim trimmed-image case).  No real OpenAMP / RPMsg
 * transport exists under native_sim, so the stub lets examples
 * that include <alp/rpc.h> compile and exercise the dispatcher
 * without pulling in CONFIG_OPENAMP / CONFIG_IPC_SERVICE.
 *
 * Contract:
 *   - open               -> ALP_OK (no underlying bring-up)
 *   - subscribe / unsub  -> ALP_OK (the customer's registration is
 *                          accepted; the stub silently drops every
 *                          would-be inbound frame since no peer
 *                          ever sends one)
 *   - send               -> ALP_OK (frame silently dropped)
 *   - call               -> ALP_ERR_TIMEOUT (no reply ever comes;
 *                          mirrors the documented header contract
 *                          for a peer that fails to respond)
 *   - close              -> no-op
 *
 * Matches the design spec Section 5 sw_fallback contract.
 *
 * @par Cost: ROM ~250 B, zero RAM (no per-handle backend state --
 *      every state->be_data is left NULL).
 * @par Performance: O(1) per call; every op short-circuits to
 *      ALP_OK / ALP_ERR_TIMEOUT with no Zephyr-subsystem touch.
 *      call() returns immediately with timeout regardless of the
 *      caller's timeout_ms -- the stub does NOT block.
 */

#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/rpc.h>

#include "rpc_ops.h"

static alp_status_t sw_open(const alp_rpc_config_t *cfg,
                            alp_rpc_backend_state_t *st,
                            alp_capabilities_t *caps_out)
{
    (void)cfg;
    st->be_data     = NULL;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t sw_subscribe(alp_rpc_backend_state_t *st, const char *method,
                                 alp_rpc_method_cb_t cb, void *user)
{
    (void)st;
    (void)method;
    (void)cb;
    (void)user;
    /* The stub accepts the registration silently -- no inbound frames
     * will ever be delivered under native_sim, so the test surface is
     * "the dispatcher routed the call into the backend at all". */
    return ALP_OK;
}

static alp_status_t sw_unsubscribe(alp_rpc_backend_state_t *st, const char *method)
{
    (void)st;
    (void)method;
    return ALP_OK;
}

static alp_status_t sw_send(alp_rpc_backend_state_t *st, const char *method,
                            const void *payload, size_t len)
{
    (void)st;
    (void)method;
    (void)payload;
    (void)len;
    /* Frame silently dropped -- no peer exists under native_sim. */
    return ALP_OK;
}

static alp_status_t sw_call(alp_rpc_backend_state_t *st, const char *method,
                            const void *req, size_t req_len,
                            void *resp, size_t *resp_len, uint32_t timeout_ms)
{
    (void)st;
    (void)method;
    (void)req;
    (void)req_len;
    (void)resp;
    (void)timeout_ms;
    if (resp_len != NULL) {
        *resp_len = 0;
    }
    /* No reply ever comes -- mirrors the documented header contract
     * for a peer that fails to respond.  Returns immediately
     * regardless of timeout_ms; the stub does NOT block. */
    return ALP_ERR_TIMEOUT;
}

static void sw_close(alp_rpc_backend_state_t *st)
{
    (void)st;
}

/* ---------- Registration ---------- */

static const alp_rpc_ops_t _ops = {
    .open        = sw_open,
    .subscribe   = sw_subscribe,
    .unsubscribe = sw_unsubscribe,
    .send        = sw_send,
    .call        = sw_call,
    .close       = sw_close,
};

ALP_BACKEND_REGISTER(rpc, sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_fallback",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
